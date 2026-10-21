/* pe.c — PE32+ image loader for the losedoze homemade Win32 subsystem.
 * Maps an EXE (or DLL) image, applies base relocations, binds imports to
 * host builtin DLL export tables (kernel32/user32/gdi32/ntdll implemented in
 * w32host itself). Original code; PE format facts only. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

#include "host.h"

#define MZ 0x5a4d
#define PE_SIG 0x00004550
#define PE32P 0x20b
#define IMG_FILE_MACHINE_AMD64 0x8664
#define DIR_IMPORT 1
#define DIR_BASERELOC 5
#define REL_DIR64 10
#define SCN_MEM_EXECUTE 0x20000000u
#define SCN_MEM_READ 0x40000000u
#define SCN_MEM_WRITE 0x80000000u
#define DLL_MAGIC 0x4c4c5744u  /* "WDLL" */
#define PE_MAGIC 0x4d494550u   /* "PEIM" */

typedef struct DllEnt {
  W32 magic;
  char name[64];
  const char *const *funcs;
  void **fns;
} DllEnt;

struct PeImage {
  W32 magic;
  W8 *base;
  size_t size;
  void *entry;
  int relocated;
  char name[256];
};

#define MAX_DLL 16
static DllEnt dlls[MAX_DLL];
static int ndll;
static PeImage *main_image;

void pe_register_dll(const char *name, const char *const *funcs, void **fns) {
  if (ndll >= MAX_DLL) return;
  DllEnt *d = &dlls[ndll++];
  d->magic = DLL_MAGIC;
  snprintf(d->name, sizeof d->name, "%s", name);
  d->funcs = funcs; d->fns = fns;
}

static DllEnt *find_dll(const char *name) {
  for (int i = 0; i < ndll; i++)
    if (!strcasecmp(dlls[i].name, name)) return &dlls[i];
  return NULL;
}

HMODULE pe_dll_handle(const char *name) { return (HMODULE)find_dll(name); }

FARPROC pe_get_export(HMODULE h, const char *name) {
  if (!h) return NULL;
  W32 mag = *(W32 *)h;
  if (mag == DLL_MAGIC) {
    DllEnt *d = (DllEnt *)h;
    for (int i = 0; d->funcs[i]; i++)
      if (!strcmp(d->funcs[i], name)) return (FARPROC)d->fns[i];
    return NULL;
  }
  if (mag == PE_MAGIC) { /* intra-image export (our apps export nothing) */
    (void)name;
    return NULL;
  }
  return NULL;
}

const char *pe_image_name(PeImage *m) { return m ? m->name : "?"; }

void *pe_entry(PeImage *m) { return m ? m->entry : NULL; }
HMODULE pe_module(PeImage *m) { return (HMODULE)m; }

static W8 *pe_opt(PeImage *m) {
  return m->base + *(W32 *)(m->base + 0x3c) + 24;
}

static int apply_relocs(PeImage *m, W64 old_base) {
  W8 *opt = pe_opt(m);
  W64 ddir = *(W32 *)(opt + 108) > DIR_BASERELOC ? *(W32 *)(opt + 112 + DIR_BASERELOC * 8) : 0;
  W32 dsz = *(W32 *)(opt + 112 + DIR_BASERELOC * 8 + 4);
  if (!ddir || !dsz) return -1; /* none present */
  W8 *p = m->base + ddir, *end = p + dsz;
  W64S delta = (W64S)((W64)m->base - old_base);
  while (p + 8 <= end) {
    W32 pg = *(W32 *)p, blk = *(W32 *)(p + 4);
    if (blk < 8) break;
    for (W32 i = 8; i + 2 <= blk; i += 2) {
      W16 e = *(W16 *)(p + i);
      unsigned type = e >> 12, off = e & 0xfff;
      if (type == 0) continue;          /* padding */
      if (type != REL_DIR64) return -2; /* unsupported relocation */
      W64 *slot = (W64 *)(m->base + pg + off);
      *slot = (W64)((W64S)*slot + delta);
    }
    p += blk;
  }
  return 0;
}

static int bind_imports(PeImage *m) {
  W8 *opt = pe_opt(m);
  W64 ddir = *(W32 *)(opt + 112 + DIR_IMPORT * 8);
  W32 dsz = *(W32 *)(opt + 112 + DIR_IMPORT * 8 + 4);
  if (!ddir || !dsz) return 0;
  W8 *d = m->base + ddir, *end = d + dsz;
  for (; d + 20 <= end; d += 20) {
    W32 oft = *(W32 *)d, name_rva = *(W32 *)(d + 12), ft = *(W32 *)(d + 16);
    if (!oft && !name_rva && !ft) break;
    if (name_rva >= m->size || ft >= m->size || (oft && oft >= m->size)) {
      fprintf(stderr, "pe: import descriptor out of range (name=%x ft=%x)\n",
              name_rva, ft);
      return -4;
    }
    const char *dllname = (const char *)(m->base + name_rva);
    DllEnt *ent = find_dll(dllname);
    if (!ent) {
      fprintf(stderr, "pe: unknown import dll '%s'\n", dllname);
      return -1;
    }
    W64 *iat = (W64 *)(m->base + ft);
    W64 *int_ = (W64 *)(m->base + (oft ? oft : ft));
    for (size_t i = 0; int_[i]; i++) {
      W64 t = int_[i];
      if (t & 0x8000000000000000ull) {
        fprintf(stderr, "pe: %s: import by ordinal %llu unsupported\n",
                dllname, (unsigned long long)(t & 0xffff));
        return -2;
      }
      const char *fname = (const char *)(m->base + (W32)t + 2); /* hint + name */
      FARPROC fn = pe_get_export((HMODULE)ent, fname);
      if (!fn) {
        fprintf(stderr, "pe: missing import %s!%s\n", dllname, fname);
        return -3;
      }
      iat[i] = (W64)fn;
    }
  }
  return 0;
}

static void set_section_prot(PeImage *m) {
  W32 lfanew = *(W32 *)(m->base + 0x3c);
  W16 nsec = *(W16 *)(m->base + lfanew + 6);
  W16 optsz = *(W16 *)(m->base + lfanew + 20);
  W8 *sec = m->base + lfanew + 24 + optsz;
  for (int i = 0; i < nsec; i++, sec += 40) {
    W32 va = *(W32 *)(sec + 12), vsz = *(W32 *)(sec + 8), ch = *(W32 *)(sec + 36);
    if (!va) continue;
    int pr = PROT_READ;
    if (ch & SCN_MEM_EXECUTE) pr |= PROT_EXEC;
    if (ch & SCN_MEM_WRITE) pr |= PROT_WRITE;
    W64 s = (W64)(va & ~0xfffu), e = (va + (vsz ? vsz : 4096) + 0xfff) & ~0xfffu;
    if (e > m->size) e = m->size;
    if (e > s) mprotect(m->base + s, (size_t)(e - s), pr);
  }
}

PeImage *pe_load(const char *path, const char **err) {
  *err = NULL;
  int fd = open(path, O_RDONLY);
  if (fd < 0) { *err = "open"; return NULL; }
  off_t fsz = lseek(fd, 0, SEEK_END);
  if (fsz < 0x200) { *err = "size"; close(fd); return NULL; }
  W8 *file = mmap(NULL, (size_t)fsz, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (file == MAP_FAILED) { *err = "mmap file"; return NULL; }

  if (*(W16 *)file != MZ) { *err = "not MZ"; goto fail; }
  W32 lfanew = *(W32 *)(file + 0x3c);
  if (lfanew + 24 > (W64)fsz || *(W32 *)(file + lfanew) != PE_SIG) {
    *err = "not PE"; goto fail;
  }
  W8 *nt = file + lfanew;
  W16 machine = *(W16 *)(nt + 4), nsec = *(W16 *)(nt + 6);
  W16 optsz = *(W16 *)(nt + 20);
  W8 *opt = nt + 24;
  if (machine != IMG_FILE_MACHINE_AMD64) { *err = "not amd64"; goto fail; }
  if (*(W16 *)opt != PE32P) { *err = "not PE32+"; goto fail; }
  W32 entry_rva = *(W32 *)(opt + 16);
  W64 pref = *(W64 *)(opt + 24);
  W32 imgsz = *(W32 *)(opt + 56), hdrsz = *(W32 *)(opt + 60);
  if (!imgsz || imgsz > 256u * 1024 * 1024) { *err = "bad SizeOfImage"; goto fail; }

  W8 *base = NULL;
  if (pref) {
    base = mmap((void *)pref, imgsz, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (base == MAP_FAILED) base = NULL;
  }
  if (!base) {
    base = mmap(NULL, imgsz, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { *err = "mmap image"; goto fail; }
  }
  memset(base, 0, imgsz);
  memcpy(base, file, hdrsz < (W32)fsz ? hdrsz : (W32)fsz);

  W8 *sec = opt + optsz;
  for (int i = 0; i < nsec; i++, sec += 40) {
    W32 vsz = *(W32 *)(sec + 8), va = *(W32 *)(sec + 12);
    W32 rsz = *(W32 *)(sec + 16), praw = *(W32 *)(sec + 20);
    if (!va || va + vsz > imgsz) continue;
    size_t n = rsz;
    if (praw + n > (size_t)fsz) n = (size_t)fsz > praw ? (size_t)fsz - praw : 0;
    if (n > vsz) n = vsz;
    if (n) memcpy(base + va, file + praw, n);
  }
  munmap(file, (size_t)fsz);

  PeImage *m = calloc(1, sizeof *m);
  if (!m) { *err = "oom"; return NULL; }
  m->magic = PE_MAGIC;
  m->base = base;
  m->size = imgsz;
  snprintf(m->name, sizeof m->name, "%s", path);

  if ((W64)base != pref) {
    if (apply_relocs(m, pref ? pref : (W64)base) != 0) {
      *err = "relocation";
      return NULL;
    }
    m->relocated = 1;
  }
  if (bind_imports(m) != 0) { *err = "imports"; return NULL; }
  m->entry = entry_rva ? base + entry_rva : NULL;
  if (!m->entry) { *err = "no entry"; return NULL; }
  set_section_prot(m);
  if (!main_image) main_image = m;
  return m;
fail:
  munmap(file, (size_t)fsz);
  return NULL;
}
