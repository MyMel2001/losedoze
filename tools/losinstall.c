/* losinstall.c — Losedoze disk installer.
 *
 * Installs the running live system onto another block device (HDD, SSD, USB
 * stick, NVMe):
 *   - MBR partition table: ext4 root (label LOSEDOZE) + swap
 *   - the rootfs copied from the live medium (the ISO this system booted from)
 *   - kernel + initramfs on the root partition, GRUB boot sector + core image
 *     in the post-MBR gap
 * The installed system boots on its own: the initramfs finds the root
 * partition by its ext4 label (no root= kernel parameter, no udev).
 *
 * GUI (our Win32 layer) by default. Headless for the smoke test:
 *   losinstall --list
 *   losinstall --yes --device=/dev/sda --swap=1024
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/fs.h>

#include "w32.h"

extern int los_init(void);

#define LABEL      "LOSEDOZE"
#define SWAP_LABEL "LOSEDOZE-SWAP"
#define GRUB_DIR   "/usr/lib/losedoze/installer"
/* The live root is a loop mount of a file on read-only media: it cannot be
 * written, so the installer works in tmpfs (/tmp is a tmpfs in rcS) */
#define MNT_TARGET "/tmp/losinstall-target"
#define MNT_SRC    "/tmp/losinstall-src"
#define MNT_ROOT   "/tmp/losinstall-root"
#define LOGFILE    "/tmp/losinstall.log"
#define ERRFILE    "/tmp/losinstall.err"
#define SECTOR     512ULL
#define START_LBA  2048ULL              /* 1 MiB: leaves room for core.img */

/* ASCII -> W16 literal (the image has no wide literals; ASCII only here) */
static LPCWSTR wa(const char *s) {
  static W16 bufs[8][96];
  static int i;
  W16 *b = bufs[i = (i + 1) % 8];
  int n = 0;
  for (; s[n] && n < 95; n++) b[n] = (W8)s[n];
  b[n] = 0;
  return b;
}

static COLORREF rgb(int r, int g, int b) {
  return (COLORREF)((r << 16) | (g << 8) | b);
}

/* ---------------- console markers ---------------- */
static void mark(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  FILE *f = fopen("/dev/console", "a");
  if (f) {
    fprintf(f, "%s\n", buf);
    fclose(f);
  }
  f = fopen(LOGFILE, "a");          /* the GUI shows the newest line */
  if (f) {
    fprintf(f, "%s\n", buf);
    fclose(f);
  }
  /* stdout too: the headless smoke redirects it to a file and greps the
   * result there, so the markers must reach it */
  printf("%s\n", buf);
  fflush(stdout);
}

/* ---------------- small helpers ---------------- */
static int read_file(const char *path, char *buf, size_t cap) {
  FILE *f = fopen(path, "r");
  size_t n;
  if (!f) return -1;
  n = fread(buf, 1, cap - 1, f);
  fclose(f);
  buf[n] = 0;
  return 0;
}

static int mkdirs(const char *path) {
  char tmp[512], *p;
  snprintf(tmp, sizeof tmp, "%s", path);
  for (p = tmp + 1; *p; p++) {
    if (*p != '/') continue;
    *p = 0;
    mkdir(tmp, 0755);
    *p = '/';
  }
  return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/* stderr of the last command goes to ERRFILE: when mke2fs or mount fails the
 * reason is what tells us why, and the console needs it */
static int run(const char *cmd) {
  char wrapped[2400];
  int rc;
  if (strstr(cmd, "2>"))
    snprintf(wrapped, sizeof wrapped, "%s", cmd);
  else
    snprintf(wrapped, sizeof wrapped, "%s 2>%s", cmd, ERRFILE);
  rc = system(wrapped);
  if (rc == -1) return -1;
  return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

/* the last line the failed command complained about */
static void mark_error(const char *what) {
  char buf[1024];
  FILE *f = fopen(ERRFILE, "r");
  size_t n = 0;
  if (!f) {
    mark("LOSEDOZE INSTALL: %s failed", what);
    return;
  }
  n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
  if (!n) {
    mark("LOSEDOZE INSTALL: %s failed (no message)", what);
    return;
  }
  mark("LOSEDOZE INSTALL: %s failed: %s", what, buf);
}

static int file_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

/* ---------------- disks ---------------- */
#define MAXDISKS 8
struct disk {
  char name[32];
  char type[24];
  unsigned long long bytes;
  int usb, ssd, nvme;
};

static void part_path(const char *name, int idx, char *out, size_t cap) {
  size_t n = strlen(name);
  int digit = n && isdigit((unsigned char)name[n - 1]);
  snprintf(out, cap, "/dev/%s%s%d", name, digit ? "p" : "", idx);
}

static int disks_scan(struct disk *out, int max) {
  DIR *d = opendir("/sys/block");
  struct dirent *e;
  int n = 0;
  if (!d) return 0;
  while ((e = readdir(d)) && n < max) {
    char path[512], buf[64], link[1024];
    unsigned long long sectors = 0;
    struct disk *dk = &out[n];
    int len;

    if (e->d_name[0] == '.') continue;
    /* the live medium and virtual disks are not install targets */
    if (!strncmp(e->d_name, "loop", 4) || !strncmp(e->d_name, "ram", 3) ||
        !strncmp(e->d_name, "sr", 2) || !strncmp(e->d_name, "fd", 2) ||
        !strncmp(e->d_name, "dm-", 3) || !strncmp(e->d_name, "zram", 4) ||
        !strncmp(e->d_name, "md", 2) || !strncmp(e->d_name, "nbd", 3) ||
        !strncmp(e->d_name, "bcache", 6))
      continue;

    memset(dk, 0, sizeof *dk);
    snprintf(dk->name, sizeof dk->name, "%.31s", e->d_name);
    snprintf(path, sizeof path, "/sys/block/%s/size", dk->name);
    if (read_file(path, buf, sizeof buf) == 0) sectors = strtoull(buf, NULL, 10);
    if (!sectors) continue;
    dk->bytes = sectors * SECTOR;

    /* rotational 0 = SSD, 1 = HDD */
    snprintf(path, sizeof path, "/sys/block/%s/queue/rotational", dk->name);
    dk->ssd = 1;
    if (read_file(path, buf, sizeof buf) == 0 && buf[0] == '1') dk->ssd = 0;

    /* USB disks sit behind a usb controller in sysfs */
    snprintf(path, sizeof path, "/sys/block/%s", dk->name);
    len = readlink(path, link, sizeof link - 1);
    if (len > 0) {
      link[len] = 0;
      if (strstr(link, "/usb")) dk->usb = 1;
    }
    if (!strncmp(dk->name, "nvme", 4)) dk->nvme = 1;

    if (dk->usb)
      snprintf(dk->type, sizeof dk->type, "USB %s", dk->ssd ? "SSD" : "HDD");
    else if (dk->nvme)
      snprintf(dk->type, sizeof dk->type, "NVMe SSD");
    else if (!strncmp(dk->name, "vd", 2))
      snprintf(dk->type, sizeof dk->type, "virtio disk");
    else
      snprintf(dk->type, sizeof dk->type, dk->ssd ? "SSD" : "HDD");
    n++;
  }
  closedir(d);
  return n;
}

static void disk_print(FILE *f, const struct disk *d) {
  fprintf(f, "  /dev/%-12s %-14s %6.1f GB\n", d->name, d->type,
          (double)d->bytes / (1024.0 * 1024.0 * 1024.0));
}

/* ---------------- MBR ---------------- */
static void chs_bytes(unsigned long long lba, unsigned char *out) {
  unsigned long long c, h, s;
  if (lba >= 1024ULL * 255 * 63) {          /* beyond CHS reach: LBA only */
    out[0] = 0xFE; out[1] = 0xFF; out[2] = 0xFF;
    return;
  }
  c = lba / (255 * 63);
  h = (lba / 63) % 255;
  s = lba % 63 + 1;
  out[0] = (unsigned char)h;
  out[1] = (unsigned char)(s | ((c >> 2) & 0xC0));
  out[2] = (unsigned char)(c & 0xFF);
}

/* One 16-byte MBR partition entry. */
struct pent {
  unsigned char boot;
  unsigned char chs[3];
  unsigned char type;
  unsigned char echs[3];
  unsigned int start, count;
} __attribute__((packed));

static int mbr_write(const char *dev, unsigned long long p1_start,
                     unsigned long long p1_count, unsigned long long p2_start,
                     unsigned long long p2_count) {
  unsigned char mbr[512];
  struct pent e[4];
  int fd, i;
  memset(mbr, 0, sizeof mbr);
  memset(e, 0, sizeof e);

  e[0].boot = 0x80;                         /* bootable */
  e[0].type = 0x83;                        /* Linux */
  e[0].start = (unsigned int)p1_start;
  e[0].count = (unsigned int)p1_count;
  chs_bytes(p1_start, e[0].chs);
  chs_bytes(p1_start + p1_count - 1, e[0].echs);

  e[1].boot = 0x00;
  e[1].type = 0x82;                        /* Linux swap */
  e[1].start = (unsigned int)p2_start;
  e[1].count = (unsigned int)p2_count;
  chs_bytes(p2_start, e[1].chs);
  chs_bytes(p2_start + p2_count - 1, e[1].echs);

  for (i = 0; i < 4; i++) memcpy(mbr + 446 + i * 16, &e[i], 16);
  mbr[510] = 0x55;
  mbr[511] = 0xAA;

  fd = open(dev, O_RDWR);
  if (fd < 0) {
    mark("LOSEDOZE INSTALL: cannot open %s: %s", dev, strerror(errno));
    return -1;
  }
  if (pwrite(fd, mbr, 512, 0) != 512) {
    mark("LOSEDOZE INSTALL: writing MBR to %s failed: %s", dev, strerror(errno));
    close(fd);
    return -1;
  }
  fsync(fd);
  close(fd);
  /* make the kernel pick up the new table */
  fd = open(dev, O_RDONLY);
  if (fd >= 0) {
    ioctl(fd, BLKRRPART, 0);
    close(fd);
  }
  return 0;
}

/* wait for the kernel to create the partition nodes */
static int wait_part(const char *name, int idx, int timeout_s) {
  char p[80];
  int i;
  part_path(name, idx, p, sizeof p);
  for (i = 0; i < timeout_s * 10; i++) {
    if (file_exists(p)) return 0;
    usleep(100000);
  }
  return -1;
}

/* ---------------- GRUB ---------------- */
static int copy_file(const char *src, const char *dst) {
  char cmd[1024];
  snprintf(cmd, sizeof cmd, "/bin/cp -f '%s' '%s'", src, dst);
  return run(cmd);
}

static int grub_install(const char *dev) {
  char boot[512], core[512];
  int fd;
  FILE *f;
  size_t n;

  snprintf(boot, sizeof boot, "%s/boot.img", GRUB_DIR);
  snprintf(core, sizeof core, "%s/core.img", GRUB_DIR);
  if (!file_exists(boot) || !file_exists(core)) {
    mark("LOSEDOZE INSTALL: GRUB images missing in %s", GRUB_DIR);
    return -1;
  }
  fd = open(dev, O_RDWR);
  if (fd < 0) {
    mark("LOSEDOZE INSTALL: cannot open %s for GRUB: %s", dev, strerror(errno));
    return -1;
  }
  /* only the first 446 bytes: they are the boot code, the partition table
   * that follows must survive */
  f = fopen(boot, "rb");
  if (!f) { close(fd); return -1; }
  n = fread(boot, 1, 446, f);
  fclose(f);
  if (n != 446 || pwrite(fd, boot, 446, 0) != 446) {
    mark("LOSEDOZE INSTALL: writing GRUB boot sector failed");
    close(fd);
    return -1;
  }
  /* core image in the gap right behind the MBR (it is ~150 kB, so it goes
   * out in chunks) */
  f = fopen(core, "rb");
  if (!f) { close(fd); return -1; }
  {
    static char buf[65536];
    off_t off = (off_t)SECTOR;
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
      if (pwrite(fd, buf, got, off) != (long)got) {
        mark("LOSEDOZE INSTALL: writing GRUB core image failed");
        fclose(f);
        close(fd);
        return -1;
      }
      off += got;
    }
    fclose(f);
    if (off == (off_t)SECTOR) {
      mark("LOSEDOZE INSTALL: GRUB core image is empty");
      close(fd);
      return -1;
    }
  }
  fsync(fd);
  close(fd);
  return 0;
}
/* ---------------- the install itself ---------------- */
static const char *GRUB_CFG =
    "set default=0\n"
    "set timeout=5\n"
    "\n"
    "menuentry \"Losedoze\" {\n"
    "    linux /boot/vmlinuz console=tty0 console=ttyS0,115200n8 quiet\n"
    "    initrd /boot/initramfs.cpio.gz\n"
    "}\n";

/* the live medium: the ISO we booted from holds the pristine rootfs */
static int mount_source(void) {
  char cmd[512];
  int d;
  mkdirs(MNT_SRC);
  mkdirs(MNT_ROOT);
  for (d = 0; d < 3; d++) {
    snprintf(cmd, sizeof cmd, "/bin/mount -t iso9660 -o ro /dev/sr%d %s", d, MNT_SRC);
    if (run(cmd) != 0) continue;
    snprintf(cmd, sizeof cmd, "/bin/mount -t ext4 -o loop,ro %s/rootfs.ext4 %s",
             MNT_SRC, MNT_ROOT);
    if (run(cmd) != 0) {
      snprintf(cmd, sizeof cmd, "/bin/umount %s", MNT_SRC);
      run(cmd);
      continue;
    }
    return 0;
  }
  return -1;
}

static void unmount_source(void) {
  char cmd[256];
  snprintf(cmd, sizeof cmd, "/bin/umount %s", MNT_ROOT);
  run(cmd);
  snprintf(cmd, sizeof cmd, "/bin/umount %s", MNT_SRC);
  run(cmd);
}

/* everything below runs in a child process so the GUI stays responsive */
static int install_run(const struct disk *d, long swap_mb) {
  char dev[64], p1[80], p2[80], cmd[2048];
  unsigned long long total, swap_sectors = 0, p1_sectors, p2_start;
  int rc = -1;
  FILE *lf;

  truncate(LOGFILE, 0);
  snprintf(dev, sizeof dev, "/dev/%s", d->name);
  total = d->bytes / SECTOR;
  if (swap_mb > 0) swap_sectors = (unsigned long long)swap_mb * 1024 * 1024 / SECTOR;
  p1_sectors = total - START_LBA - swap_sectors - START_LBA;
  if (p1_sectors < 16 * 1024) {          /* 8 MiB */
    mark("LOSEDOZE INSTALL: %s is too small for an install", dev);
    return -1;
  }
  p2_start = START_LBA + p1_sectors;

  lf = fopen(LOGFILE, "a");              /* the console log, for the GUI */
  if (lf) {
    fprintf(lf, "losinstall: %s (%s, %llu MB), swap %ld MB\n", dev, d->type,
            d->bytes / (1024 * 1024), swap_mb);
    fclose(lf);
  }
  mark("LOSEDOZE INSTALL: target %s (%s, %llu MB) pid=%d ppid=%d", dev, d->type,
       d->bytes / (1024 * 1024), (int)getpid(), (int)getppid());
  {   /* how was this process started? (argv of the installer itself) */
    char cl[256] = "";
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) {
      ssize_t n = read(fd, cl, sizeof cl - 1);
      if (n > 0) {
        ssize_t i;
        for (i = 0; i < n - 1; i++) if (cl[i] == 0) cl[i] = ' ';
        cl[n] = 0;
      }
      close(fd);
    }
    mark("LOSEDOZE INSTALL: argv=%s", cl);
  }
  mark("LOSEDOZE INSTALL: layout ext4 %llu MB + swap %ld MB",
       p1_sectors * SECTOR / (1024 * 1024), swap_mb);

  if (mbr_write(dev, START_LBA, p1_sectors, p2_start, swap_sectors) != 0)
    return -1;
  mark("LOSEDOZE INSTALL: partition table written");
  if (wait_part(d->name, 1, 10) != 0) {
    mark("LOSEDOZE INSTALL: partition 1 did not appear");
    return -1;
  }
  part_path(d->name, 1, p1, sizeof p1);
  part_path(d->name, 2, p2, sizeof p2);

  snprintf(cmd, sizeof cmd, "/usr/sbin/mke2fs -q -t ext4 -L %s %s", LABEL, p1);
  if (run(cmd) != 0) {
    mark_error("mke2fs");
    return -1;
  }
  mark("LOSEDOZE INSTALL: ext4 filesystem created on %s", p1);
  if (swap_mb > 0) {
    if (wait_part(d->name, 2, 10) != 0) {
      mark("LOSEDOZE INSTALL: partition 2 did not appear");
      return -1;
    }
    snprintf(cmd, sizeof cmd, "/bin/mkswap -L %s %s", SWAP_LABEL, p2);
    if (run(cmd) != 0) {
      mark_error("mkswap");
      return -1;
    }
    mark("LOSEDOZE INSTALL: swap created on %s (%ld MB)", p2, swap_mb);
  }

  if (mkdirs(MNT_TARGET) != 0) {
    mark("LOSEDOZE INSTALL: cannot create %s: %s", MNT_TARGET, strerror(errno));
    return -1;
  }
  snprintf(cmd, sizeof cmd, "/bin/mount -t ext4 %s %s", p1, MNT_TARGET);
  if (run(cmd) != 0) {
    mark_error("mount");
    return -1;
  }
  if (mount_source() != 0) {
    mark("LOSEDOZE INSTALL: no live medium found (need the ISO to install from)");
    snprintf(cmd, sizeof cmd, "/bin/umount %s", MNT_TARGET);
    run(cmd);
    return -1;
  }
  mark("LOSEDOZE INSTALL: copying the system, this takes a minute");

  snprintf(cmd, sizeof cmd,
           "/bin/tar -C %s -cf - . 2>>%s | /bin/tar -C %s -xf - 2>>%s",
           MNT_ROOT, ERRFILE, MNT_TARGET, ERRFILE);
  if (run(cmd) != 0 || !file_exists(MNT_TARGET "/bin/busybox")) {
    mark_error("copy");
    goto out;
  }
  mark("LOSEDOZE INSTALL: system copied");

  /* kernel + initramfs live on the medium, not in the rootfs */
  mkdirs(MNT_TARGET "/boot/grub");
  mkdirs(MNT_TARGET "/etc/losedoze");
  if (copy_file(MNT_SRC "/boot/vmlinuz", MNT_TARGET "/boot/vmlinuz") != 0 ||
      copy_file(MNT_SRC "/boot/initramfs.cpio.gz", MNT_TARGET "/boot/initramfs.cpio.gz") != 0) {
    mark("LOSEDOZE INSTALL: copying the kernel failed");
    goto out;
  }
  {
    FILE *f = fopen(MNT_TARGET "/boot/grub/grub.cfg", "w");
    if (f) { fputs(GRUB_CFG, f); fclose(f); }
  }
  {
    FILE *f = fopen(MNT_TARGET "/etc/losedoze/installed", "w");
    if (f) { fprintf(f, "installed\n"); fclose(f); }
  }
  mark("LOSEDOZE INSTALL: boot files written");

  /* fstab: no udev in this image, so the device paths are what resolve */
  {
    FILE *f = fopen(MNT_TARGET "/etc/fstab", "w");
    if (f) {
      fprintf(f, "# Losedoze installed system\n");
      fprintf(f, "%s\t/\text4\tdefaults\t0 1\n", p1);
      if (swap_mb > 0) fprintf(f, "%s\tnone\tswap\tsw\t0 0\n", p2);
      fclose(f);
    }
  }

  if (grub_install(dev) != 0) goto out;
  mark("LOSEDOZE INSTALL: GRUB boot sector and core image written");

  snprintf(cmd, sizeof cmd, "/bin/umount %s", MNT_TARGET);
  run(cmd);
  unmount_source();
  mark("LOSEDOZE INSTALL OK: %s is ready, reboot and boot from it", dev);
  rc = 0;

out:
  unmount_source();
  snprintf(cmd, sizeof cmd, "/bin/umount %s", MNT_TARGET);
  run(cmd);
  return rc;
}


/* ---------------- self test ----------------
 * The MBR layout and the boot sector write are the part that is easy to get
 * wrong (the boot sector must not eat the partition table), so they are
 * checked here on a file: no disk, no root, no guest.
 */
static int selftest(void) {
  const char *path = "/tmp/losinstall-selftest.img";
  unsigned long long total = 2ULL * 1024 * 1024 * 1024 / SECTOR;
  unsigned long long swap_sectors = 512ULL * 1024 * 1024 / SECTOR;
  unsigned long long p1_count = total - START_LBA - swap_sectors - START_LBA;
  unsigned long long p2_start = START_LBA + p1_count;
  unsigned char mbr[512], boot[512];
  struct pent *e;
  int fd, i, fails = 0;

  fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) { printf("losinstall: selftest: cannot create %s\n", path); return 1; }
  close(fd);
  if (mbr_write(path, START_LBA, p1_count, p2_start, swap_sectors) != 0) {
    printf("losinstall: selftest: mbr_write failed\n");
    return 1;
  }
  /* a fake GRUB boot sector: the first 446 bytes, the table behind it */
  memset(boot, 0, sizeof boot);
  boot[0] = 0xEB; boot[1] = 0x63; boot[2] = 0x90;
  memcpy(boot + 8, "GRUB", 4);
  fd = open(path, O_RDWR);
  if (fd < 0 || pwrite(fd, boot, 446, 0) != 446) {
    printf("losinstall: selftest: boot sector write failed\n");
    return 1;
  }
  close(fd);

  fd = open(path, O_RDONLY);
  if (fd < 0 || read(fd, mbr, 512) != 512) {
    printf("losinstall: selftest: read back failed\n");
    return 1;
  }
  close(fd);
  remove(path);

  if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
    printf("losinstall: selftest: no MBR signature\n");
    fails++;
  }
  if (mbr[0] != 0xEB || memcmp(mbr + 8, "GRUB", 4)) {
    printf("losinstall: selftest: boot sector not written\n");
    fails++;
  }
  e = (struct pent *)(mbr + 446);
  if (e[0].type != 0x83 || e[0].start != (unsigned int)START_LBA ||
      e[0].count != (unsigned int)p1_count) {
    printf("losinstall: selftest: root partition entry wrong (type=%02x start=%u count=%u)\n",
           e[0].type, e[0].start, e[0].count);
    fails++;
  }
  if (e[1].type != 0x82 || e[1].start != (unsigned int)p2_start ||
      e[1].count != (unsigned int)swap_sectors) {
    printf("losinstall: selftest: swap partition entry wrong (type=%02x start=%u count=%u)\n",
           e[1].type, e[1].start, e[1].count);
    fails++;
  }
  for (i = 2; i < 4; i++)
    if (e[i].type) {
      printf("losinstall: selftest: slot %d is not empty\n", i);
      fails++;
    }
  if (fails) return 1;
  printf("LOSEDOZE INSTALL SELFTEST OK (root lba=%llu sectors=%llu, swap lba=%llu sectors=%llu)\n",
         START_LBA, p1_count, p2_start, swap_sectors);
  return 0;
}

/* ---------------- GUI ---------------- */
#define W 560
#define H 380
static const long SWAP_SIZES[] = { 0, 256, 512, 1024, 2048, 4096 };
#define NSWAP (int)(sizeof SWAP_SIZES / sizeof SWAP_SIZES[0])

enum { ST_DISKS = 0, ST_CONFIRM, ST_RUN, ST_DONE };
static struct disk disks[MAXDISKS];
static int ndisks, sel, swap_idx = 3, stage;
static HWND hwnd;
static pid_t worker;
static char status[256] = "";

static void paint(HWND h) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  RECT all = { 0, 0, W, H };
  int i;
  char line[256];
  if (!dc) return;

  HBRUSH bg = CreateSolidBrush(rgb(0x1f, 0x1f, 0x1f));
  FillRect(dc, &all, bg);
  DeleteObject(bg);
  SetBkMode(dc, 1);
  SetTextColor(dc, rgb(0xff, 0xff, 0xff));
  TextOutW(dc, 24, 20, wa("Install Losedoze to a disk"), -1);
  SetTextColor(dc, rgb(0x9a, 0x9a, 0x9a));
  TextOutW(dc, 24, 44, wa("ext4 root + swap, GRUB bootloader, your files stay on the disk"), -1);

  if (stage == ST_DISKS) {
    SetTextColor(dc, rgb(0xdd, 0xdd, 0xdd));
    TextOutW(dc, 24, 76, wa("Target disk:"), -1);
    for (i = 0; i < ndisks; i++) {
      int y = 104 + i * 26;
      if (i == sel) {
        RECT hl = { 16, y - 6, W - 16, y + 22 };
        HBRUSH hb = CreateSolidBrush(rgb(0x2f, 0x6f, 0xed));
        FillRect(dc, &hl, hb);
        DeleteObject(hb);
        SetTextColor(dc, rgb(0xff, 0xff, 0xff));
      } else {
        SetTextColor(dc, rgb(0xcc, 0xcc, 0xcc));
      }
      snprintf(line, sizeof line, "  /dev/%-12.12s %-14.14s %6.1f GB", disks[i].name,
               disks[i].type, (double)disks[i].bytes / (1024.0 * 1024.0 * 1024.0));
      TextOutW(dc, 28, y, wa(line), -1);
    }
    if (!ndisks) {
      SetTextColor(dc, rgb(0xdd, 0x77, 0x77));
      TextOutW(dc, 24, 104, wa("no disk found (plug one in)"), -1);
    }
    snprintf(line, sizeof line, "Swap partition: %ld MB   (left/right to change)",
             SWAP_SIZES[swap_idx]);
    SetTextColor(dc, rgb(0xdd, 0xdd, 0xdd));
    TextOutW(dc, 24, 272, wa(line), -1);

    RECT btn = { 24, 306, 200, 340 };
    HBRUSH hb = CreateSolidBrush(rgb(0x2f, 0x6f, 0xed));
    FillRect(dc, &btn, hb);
    DeleteObject(hb);
    SetTextColor(dc, rgb(0xff, 0xff, 0xff));
    TextOutW(dc, 40, 315, wa("[ Install ]"), -1);
    SetTextColor(dc, rgb(0x9a, 0x9a, 0x9a));
    TextOutW(dc, 24, 350, wa("up/down pick disk   left/right swap   enter install   q quit"), -1);
  } else if (stage == ST_CONFIRM) {
    snprintf(line, sizeof line, "Erase /dev/%s (%s) and install?", disks[sel].name,
             disks[sel].type);
    SetTextColor(dc, rgb(0xff, 0xcc, 0x66));
    TextOutW(dc, 24, 120, wa(line), -1);
    SetTextColor(dc, rgb(0xdd, 0xdd, 0xdd));
    TextOutW(dc, 24, 156, wa("All data on this disk is lost."), -1);
    snprintf(line, sizeof line, "enter = yes,  q = no");
    TextOutW(dc, 24, 200, wa(line), -1);
  } else if (stage == ST_RUN) {
    SetTextColor(dc, rgb(0xff, 0xff, 0xff));
    TextOutW(dc, 24, 120, wa("Installing..."), -1);
    SetTextColor(dc, rgb(0xcc, 0xcc, 0xcc));
    TextOutW(dc, 24, 160, wa(status), -1);
  } else {
    SetTextColor(dc, rgb(0x8f, 0xd0, 0x8f));
    TextOutW(dc, 24, 120, wa(status), -1);
    SetTextColor(dc, rgb(0x9a, 0x9a, 0x9a));
    TextOutW(dc, 24, 160, wa("q closes the installer"), -1);
  }
  EndPaint(h, &ps);
}

static void start_install(void) {
  pid_t p = fork();
  if (p == 0) {
    int rc = install_run(&disks[sel], SWAP_SIZES[swap_idx]);
    _exit(rc == 0 ? 0 : 1);
  }
  if (p < 0) {
    stage = ST_DONE;
    snprintf(status, sizeof status, "cannot start the installer");
  } else {
    worker = p;
    stage = ST_RUN;
    snprintf(status, sizeof status, "preparing %s", disks[sel].name);
  }
  InvalidateRect(hwnd, NULL, 1);
}

static void poll_worker(void) {
  int st;
  pid_t r;
  char buf[512];
  FILE *f;

  if (stage != ST_RUN || !worker) return;
  f = fopen(LOGFILE, "r");
  if (f) {
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    /* last line of the log is the newest progress */
    if (n) {
      char *nl = strrchr(buf, '\n');
      snprintf(status, sizeof status, "%.255s", nl ? nl + 1 : buf);
    }
  }
  r = waitpid(worker, &st, WNOHANG);
  if (r == worker) {
    worker = 0;
    stage = ST_DONE;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
      snprintf(status, sizeof status, "Installed. Reboot and boot from the disk.");
    else
      snprintf(status, sizeof status, "Install failed (see the console log).");
    InvalidateRect(hwnd, NULL, 1);
  }
}

static LRESULT wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: paint(h); return 0;
    case WM_KEYDOWN:
      if (stage == ST_RUN) return 0;
      switch ((int)wp) {
        case VK_UP:
          if (stage == ST_DISKS && ndisks) sel = (sel + ndisks - 1) % ndisks;
          break;
        case VK_DOWN:
          if (stage == ST_DISKS && ndisks) sel = (sel + 1) % ndisks;
          break;
        case VK_LEFT:
          if (stage == ST_DISKS && swap_idx > 0) swap_idx--;
          break;
        case VK_RIGHT:
          if (stage == ST_DISKS && swap_idx < NSWAP - 1) swap_idx++;
          break;
        case VK_RETURN: case VK_SPACE:
          if (stage == ST_DISKS) {
            if (!ndisks) break;
            stage = ST_CONFIRM;
          } else if (stage == ST_CONFIRM) {
            start_install();
          }
          break;
        case 'q': case 'Q': case VK_ESCAPE:
          if (stage == ST_CONFIRM) stage = ST_DISKS;
          else if (stage != ST_RUN) PostQuitMessage(0);
          break;
      }
      InvalidateRect(hwnd, NULL, 1);
      return 0;
    case WM_LBUTTONDOWN: {
      int y = (int)((lp >> 16) & 0xffff);
      if (stage != ST_DISKS) break;
      for (int i = 0; i < ndisks; i++) {
        int ry = 104 + i * 26;
        if (y >= ry - 6 && y < ry + 22) { sel = i; InvalidateRect(hwnd, NULL, 1); }
      }
      if (y >= 306 && y < 340) { stage = ST_CONFIRM; InvalidateRect(hwnd, NULL, 1); }
      break;
    }
    case WM_CLOSE: PostQuitMessage(0); break;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

/* ---------------- main ---------------- */
static void usage(void) {
  printf("usage: losinstall [--list] [--yes --device=DEV] [--swap=MB]\n");
}

int main(int argc, char **argv) {
  const char *dev = NULL;
  /* the image has busybox symlinks for the same names in /bin; the PATH
   * decides which one runs, and busybox's mke2fs is ext2 only */
  setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
  long swap_mb = 1024;
  int yes = 0, list = 0, i;

  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--list")) list = 1;
    else if (!strcmp(argv[i], "--yes")) yes = 1;
    else if (!strncmp(argv[i], "--device=", 9)) dev = argv[i] + 9;
    else if (!strncmp(argv[i], "--swap=", 7)) swap_mb = atol(argv[i] + 7);
    else if (!strcmp(argv[i], "--selftest")) return selftest();
    else if (!strcmp(argv[i], "--help")) { usage(); return 0; }
    else { usage(); return 2; }
  }

  ndisks = disks_scan(disks, MAXDISKS);
  if (list) {
    printf("Losedoze installer -- disks found:\n");
    for (i = 0; i < ndisks; i++) disk_print(stdout, &disks[i]);
    if (!ndisks) printf("  (none)\n");
    return 0;
  }

  if (yes) {
    struct disk target;
    int found = 0;
    memset(&target, 0, sizeof target);
    if (dev) {
      const char *nm = strrchr(dev, '/');
      nm = nm ? nm + 1 : dev;
      for (i = 0; i < ndisks; i++) {
        if (!strcmp(disks[i].name, nm)) { target = disks[i]; found = 1; break; }
      }
      if (!found) {
        mark("LOSEDOZE INSTALL: %s is not an installable disk", dev);
        return 1;
      }
    } else if (ndisks) {
      target = disks[0];
    } else {
      mark("LOSEDOZE INSTALL: no disk found");
      return 1;
    }
    return install_run(&target, swap_mb) == 0 ? 0 : 1;
  }

  if (!los_init()) {
    fprintf(stderr, "losinstall: no compositor\n");
    return 1;
  }
  if (!ndisks) {
    /* still open the window: it says no disk was found */
    printf("LOSEDOZE INSTALLER OK\n");
    fflush(stdout);
  }
  WNDCLASSW wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = wndproc;
  wc.lpszClassName = wa("LosedozeInstaller");
  if (!RegisterClassExW(&wc)) {
    fprintf(stderr, "losinstall: RegisterClassEx failed\n");
    return 1;
  }
  hwnd = CreateWindowExW(0, wa("LosedozeInstaller"), wa("Install Losedoze"),
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, W, H,
                         NULL, NULL, NULL, NULL);
  if (!hwnd) {
    fprintf(stderr, "losinstall: CreateWindowEx failed\n");
    return 1;
  }
  printf("LOSEDOZE INSTALLER OK\n");
  fflush(stdout);

  /* PeekMessageW + poll: it paints, takes queued input and pumps the
   * compositor socket, so the window can follow the install while it runs */
  MSG m;
  for (;;) {
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
      if (m.message == WM_QUIT) return 0;
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
    if (stage == ST_RUN) {
      poll_worker();
      InvalidateRect(hwnd, NULL, 1);
    }
    usleep(30000);
  }
  return 0;
}
