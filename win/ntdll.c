/* ntdll.c — homemade NT syscall layer for the losedoze Win32 subsystem.
 * Dispatch table indexed by syscall numbers generated from the
 * windows-syscalls nt.csv reference tables (win/gen_ntsyscalls.py); handlers
 * implement NT semantics directly on Linux syscalls. Original code. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "host.h"
#include "ntsyscalls.inc"

enum {
#define X(name, nr) NR_##name = nr,
    NT_SYSCALL_LIST(X)
#undef X
};

static const char *const nt_names[NT_NR_MAX] = {
#define X(name, nr) [nr] = #name,
    NT_SYSCALL_LIST(X)
#undef X
};

const char *nt_name(W32 nr) {
  if (nr < NT_NR_MAX && nt_names[nr]) return nt_names[nr];
  return "Nt???";
}

/* ---------------- handle layer (shared with kernel32.c) ---------------- */

W32Handle *w32_handle_new(int kind, int fd) {
  W32Handle *h = calloc(1, sizeof *h);
  if (!h) return NULL;
  h->magic = W32_HANDLE_MAGIC;
  h->kind = kind;
  h->fd = fd;
  return h;
}
int w32_handle_kind(HANDLE h, int want) {
  W32Handle *w = (W32Handle *)h;
  if (!w || w->magic != W32_HANDLE_MAGIC) return 0;
  return want == 0 || w->kind == want;
}
int w32_handle_fd(HANDLE h) { return w32_handle_kind(h, 0) ? ((W32Handle *)h)->fd : -1; }
W32Handle *w32_handle(HANDLE h) {
  return w32_handle_kind(h, 0) ? (W32Handle *)h : NULL;
}

/* ---------------- string helpers (shared) ---------------- */
int w16_to_utf8(const W16 *s, int n, char *out, size_t cap) {
  size_t o = 0;
  if (n < 0) n = 0;
  for (int i = 0; i < n && s[i]; i++) {
    unsigned c = s[i];
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] <= 0xdfff) {
      c = 0x10000 + ((c - 0xd800) << 10) + (s[i + 1] - 0xdc00);
      i++;
    }
    if (o + 4 >= cap) break;
    if (c < 0x80) out[o++] = (char)c;
    else if (c < 0x800) {
      out[o++] = (char)(0xc0 | (c >> 6));
      out[o++] = (char)(0x80 | (c & 63));
    } else if (c < 0x10000) {
      out[o++] = (char)(0xe0 | (c >> 12));
      out[o++] = (char)(0x80 | ((c >> 6) & 63));
      out[o++] = (char)(0x80 | (c & 63));
    } else {
      out[o++] = (char)(0xf0 | (c >> 18));
      out[o++] = (char)(0x80 | ((c >> 12) & 63));
      out[o++] = (char)(0x80 | ((c >> 6) & 63));
      out[o++] = (char)(0x80 | (c & 63));
    }
  }
  out[o] = 0;
  return (int)o;
}

/* NT object path -> unix path. \??\C:\foo -> /foo; \Device\HarddiskVolumeN\x
 * -> /x; plain unix path passes through. Returns 0 ok, -1 unsupported. */
int nt_path_to_unix(const char *in, char *out, size_t cap) {
  const char *p = in;
  if (!strncmp(p, "\\??\\", 4)) p += 4;
  else if (!strncmp(p, "\\Device\\HarddiskVolume", 22)) {
    p += 22;
    while (*p && *p != '\\') p++;
  }
  if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
    p += 2;
  if (*p == 0) { snprintf(out, cap, "/"); return 0; }
  if (*p == '/') { /* already a unix absolute path */
    snprintf(out, cap, "%s", p);
    return 0;
  }
  if (*p != '\\') { /* relative-to-root unix path */
    snprintf(out, cap, "/%s", p);
    return 0;
  }
  size_t o = 0;
  for (p++; *p && o + 1 < cap; p++)
    out[o++] = *p == '\\' ? '/' : *p;
  out[o] = 0;
  return 0;
}

/* ---------------- NT trace ---------------- */
static int nt_trace_on;
static void nt_trace(W32 nr) {
  if (nt_trace_on) host_log("nt: %s (0x%x)", nt_name(nr), nr);
}

/* ---------------- handlers ---------------- */
typedef long (*ntfn)(const long *);

static long h_NtCreateFile(const long *a) {
  HANDLE *out = (HANDLE *)a[0];
  OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a[2];
  IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a[3];
  W32 disp = (W32)a[7];
  if (!out || !oa || !oa->ObjectName || !oa->ObjectName->Buffer)
    return STATUS_ACCESS_DENIED;
  char u8[1024], path[1024];
  w16_to_utf8(oa->ObjectName->Buffer, oa->ObjectName->Length / 2, u8, sizeof u8);
  if (nt_path_to_unix(u8, path, sizeof path) != 0)
    return STATUS_OBJECT_PATH_NOT_FOUND;
  int flags = 0;
  W32 access = (W32)a[1];
  if ((access & GENERIC_WRITE) && (access & GENERIC_READ)) flags = O_RDWR;
  else if (access & GENERIC_WRITE) flags = O_WRONLY;
  else flags = O_RDONLY;
  switch (disp) {
  case 2: flags |= O_CREAT | O_EXCL; break;              /* FILE_CREATE */
  case 3: flags |= O_CREAT; break;                       /* FILE_OPEN_IF */
  case 4: case 5: flags |= O_CREAT | O_TRUNC; break;     /* OVERWRITE[_IF] */
  default: break;                                        /* FILE_OPEN */
  }
  int fd = open(path, flags | O_CLOEXEC, 0644);
  if (fd < 0 || getenv("NT_TRACE"))
    fprintf(stderr, "nt: CreateFile u8=%s path=%s flags=%x disp=%u errno=%d\n",
            u8, path, flags, disp, fd < 0 ? errno : 0);
  if (fd < 0) {
    if (errno == ENOENT) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (errno == EEXIST) return STATUS_OBJECT_NAME_COLLISION;
    if (errno == EACCES) return STATUS_ACCESS_DENIED;
    return STATUS_OBJECT_PATH_NOT_FOUND;
  }
  W32Handle *h = w32_handle_new(HK_FD, fd);
  if (!h) { close(fd); return STATUS_NO_MEMORY; }
  *out = (HANDLE)h;
  if (iosb) { iosb->u.Status = STATUS_SUCCESS; iosb->Information = (ULONG_PTR)disp; }
  return STATUS_SUCCESS;
}

static long h_NtReadFile(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a[4];
  void *buf = (void *)a[5];
  W32 len = (W32)a[6];
  if (!h || h->fd < 0) return STATUS_INVALID_HANDLE;
  ssize_t n = read(h->fd, buf, len);
  if (n < 0) {
    if (errno == EAGAIN) { if (iosb) iosb->u.Status = STATUS_PENDING; return STATUS_PENDING; }
    n = -1;
  }
  if (iosb) { iosb->u.Status = n < 0 ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
              iosb->Information = n > 0 ? (ULONG_PTR)n : 0; }
  return n < 0 ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
}

static long h_NtWriteFile(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a[4];
  const void *buf = (const void *)a[5];
  W32 len = (W32)a[6];
  if (!h || h->fd < 0) return STATUS_INVALID_HANDLE;
  ssize_t n = write(h->fd, buf, len);
  if (iosb) { iosb->u.Status = n < 0 ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
              iosb->Information = n > 0 ? (ULONG_PTR)n : 0; }
  return n < 0 ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
}

static long h_NtClose(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  if (!h) return STATUS_INVALID_HANDLE;
  if (h->fd >= 0 && h->kind == HK_FD) close(h->fd);
  h->magic = 0;
  free(h);
  return STATUS_SUCCESS;
}

static long h_NtAllocateVirtualMemory(const long *a) {
  PVOID *base = (PVOID *)a[1];
  ULONG_PTR *psize = (ULONG_PTR *)a[3];
  W32 type = (W32)a[4], prot = (W32)a[5];
  (void)type;
  if (!base || !psize) return STATUS_ACCESS_DENIED;
  size_t sz = *psize ? (size_t)*psize : 4096;
  sz = (sz + 0xfff) & ~0xffful;
  int pr = PROT_READ | PROT_WRITE;
  if (prot & PAGE_EXECUTE_READWRITE) pr |= PROT_EXEC;
  void *p = mmap(*base, sz, pr, MAP_PRIVATE | MAP_ANONYMOUS | (*base ? MAP_FIXED_NOREPLACE : 0), -1, 0);
  if (p == MAP_FAILED) return STATUS_NO_MEMORY;
  *base = p;
  *psize = sz;
  return STATUS_SUCCESS;
}

static long h_NtFreeVirtualMemory(const long *a) {
  PVOID *base = (PVOID *)a[1];
  ULONG_PTR *psize = (ULONG_PTR *)a[2];
  W32 type = (W32)a[3];
  if (!base) return STATUS_ACCESS_DENIED;
  if (type & MEM_RELEASE) {
    size_t sz = psize && *psize ? (size_t)*psize : 0;
    if (munmap(*base, sz ? sz : 4096) != 0) return STATUS_ACCESS_DENIED;
    *base = NULL;
    if (psize) *psize = 0;
  }
  return STATUS_SUCCESS;
}

static long h_NtDelayExecution(const long *a) {
  W32S alert = (W32S)a[0];
  W64S *interval = (W64S *)a[1]; /* 100ns units, negative = relative */
  (void)alert;
  if (!interval) return STATUS_SUCCESS;
  if (*interval >= 0) return STATUS_SUCCESS; /* absolute times: ceiling */
  struct timespec ts;
  W64S t = -*interval;
  ts.tv_sec = (time_t)(t / 10000000);
  ts.tv_nsec = (long)((t % 10000000) * 100);
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
  return STATUS_SUCCESS;
}

static long h_NtWaitForSingleObject(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  W32S alert = (W32S)a[1];
  W64S *timeout = (W64S *)a[2];
  (void)alert;
  if (!h) return STATUS_INVALID_HANDLE;
  if (h->kind == HK_EVENT) return STATUS_SUCCESS; /* auto/manual: always signaled */
  if (timeout && *timeout == 0) return STATUS_PENDING;
  return STATUS_SUCCESS; /* single-threaded host: no real wait needed */
}

/* FILE_INFORMATION_CLASS: standard=5, position=14 */
static long h_NtQueryInformationFile(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a[1];
  void *buf = (void *)a[2];
  W32 len = (W32)a[3];
  int cls = (int)a[4];
  if (!h || h->fd < 0) return STATUS_INVALID_HANDLE;
  if (cls == 5 && len >= sizeof(FILE_STANDARD_INFORMATION)) {
    struct stat st;
    if (fstat(h->fd, &st) != 0) return STATUS_ACCESS_DENIED;
    FILE_STANDARD_INFORMATION *si = buf;
    si->EndOfFile = (W64)st.st_size;
    si->AllocationSize = (W64)st.st_size;
    si->NumberOfLinks = (ULONG)st.st_nlink;
    si->DeletePending = 0;
    si->Directory = S_ISDIR(st.st_mode);
  } else if (cls == 14 && len >= sizeof(FILE_POSITION_INFORMATION)) {
    FILE_POSITION_INFORMATION *pi = buf;
    off_t o = lseek(h->fd, 0, SEEK_CUR);
    if (o < 0) return STATUS_ACCESS_DENIED;
    pi->CurrentByteOffset = (W64)o;
  } else
    return STATUS_NOT_IMPLEMENTED;
  if (iosb) { iosb->u.Status = STATUS_SUCCESS; iosb->Information = len; }
  return STATUS_SUCCESS;
}

static long h_NtSetInformationFile(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a[1];
  void *buf = (void *)a[2];
  W32 len = (W32)a[3];
  int cls = (int)a[4];
  if (!h || h->fd < 0) return STATUS_INVALID_HANDLE;
  if (cls == 14 && len >= sizeof(FILE_POSITION_INFORMATION)) {
    FILE_POSITION_INFORMATION *pi = buf;
    if (lseek(h->fd, (off_t)pi->CurrentByteOffset, SEEK_SET) < 0)
      return STATUS_ACCESS_DENIED;
  } else
    return STATUS_NOT_IMPLEMENTED;
  if (iosb) { iosb->u.Status = STATUS_SUCCESS; iosb->Information = len; }
  return STATUS_SUCCESS;
}

/* ---------------- dispatch table (numbers from windows-syscalls) -------- */
static ntfn nt_tab[NT_NR_MAX];
static int nt_ready;

static void nt_register(void) {
#define REG(name) nt_tab[NR_##name] = h_##name
  REG(NtCreateFile);
  REG(NtReadFile);
  REG(NtWriteFile);
  REG(NtClose);
  REG(NtAllocateVirtualMemory);
  REG(NtFreeVirtualMemory);
  REG(NtDelayExecution);
  REG(NtWaitForSingleObject);
  REG(NtQueryInformationFile);
  REG(NtSetInformationFile);
#undef REG
  nt_tab[NR_NtOpenFile] = h_NtCreateFile;
  nt_trace_on = getenv("NT_TRACE") != NULL;
  nt_ready = 1;
}

static long nt_dispatch(W32 nr, const long *args) {
  if (!nt_ready) nt_register();
  nt_trace(nr);
  if (nr < NT_NR_MAX && nt_tab[nr]) return nt_tab[nr](args);
  host_log("nt: %s (0x%x) not implemented", nt_name(nr), nr);
  return STATUS_NOT_IMPLEMENTED;
}

/* NtOpenFile maps to NtCreateFile (FILE_OPEN) */

/* ---------------- exported Nt* / Zw* stubs ---------------- */
#define ARGV(n) long a[n]; memset(a, 0, sizeof a)

/* Zw* are the Windows names for the same syscalls Nt* dispatch to (ntdll
 * exports both families; real binaries may import either spelling). */
NTSTATUS ZwCreateFile(HANDLE *FileHandle, ULONG Access, OBJECT_ATTRIBUTES *Obj,
                      IO_STATUS_BLOCK *Iosb, PVOID AllocSize, ULONG FileAttr,
                      ULONG Share, ULONG Disp, ULONG Opts, PVOID Ea, ULONG EaLen) {
  return NtCreateFile(FileHandle, Access, Obj, Iosb, AllocSize, FileAttr,
                      Share, Disp, Opts, Ea, EaLen);
}

NTSTATUS ZwOpenFile(HANDLE *FileHandle, ULONG Access, OBJECT_ATTRIBUTES *Obj,
                    IO_STATUS_BLOCK *Iosb, ULONG Share, ULONG Opts) {
  return NtOpenFile(FileHandle, Access, Obj, Iosb, Share, Opts);
}

NTSTATUS ZwReadFile(HANDLE fh, HANDLE ev, PVOID apc, PVOID apcctx,
                     IO_STATUS_BLOCK *Iosb, PVOID Buf, ULONG Len, PVOID off,
                     PVOID key) {
  return NtReadFile(fh, ev, apc, apcctx, Iosb, Buf, Len, off, key);
}

NTSTATUS ZwWriteFile(HANDLE fh, HANDLE ev, PVOID apc, PVOID apcctx,
                      IO_STATUS_BLOCK *Iosb, PVOID Buf, ULONG Len, PVOID off,
                      PVOID key) {
  return NtWriteFile(fh, ev, apc, apcctx, Iosb, Buf, Len, off, key);
}

NTSTATUS ZwClose(HANDLE h) {
  return NtClose(h);
}

NTSTATUS ZwAllocateVirtualMemory(HANDLE proc, PVOID *base, ULONG_PTR zero,
                                 ULONG_PTR *size, ULONG type, ULONG prot) {
  return NtAllocateVirtualMemory(proc, base, zero, size, type, prot);
}

NTSTATUS ZwFreeVirtualMemory(HANDLE proc, PVOID *base, ULONG_PTR *size,
                             ULONG type) {
  return NtFreeVirtualMemory(proc, base, size, type);
}

NTSTATUS ZwDelayExecution(BOOL Alertable, W64S *Interval) {
  return NtDelayExecution(Alertable, Interval);
}

NTSTATUS ZwWaitForSingleObject(HANDLE h, BOOL Alertable, W64S *Timeout) {
  return NtWaitForSingleObject(h, Alertable, Timeout);
}

NTSTATUS ZwQueryInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf,
                                ULONG len, int cls) {
  return NtQueryInformationFile(h, iosb, buf, len, cls);
}

NTSTATUS ZwSetInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf,
                              ULONG len, int cls) {
  return NtSetInformationFile(h, iosb, buf, len, cls);
}

NTSTATUS NtCreateFile(HANDLE *FileHandle, ULONG Access, OBJECT_ATTRIBUTES *Obj,
                      IO_STATUS_BLOCK *Iosb, PVOID AllocSize, ULONG FileAttr,
                      ULONG Share, ULONG Disp, ULONG Opts, PVOID Ea, ULONG EaLen) {
  ARGV(11);
  a[0] = (long)FileHandle; a[1] = (long)Access; a[2] = (long)Obj;
  a[3] = (long)Iosb; a[4] = (long)AllocSize; a[5] = (long)FileAttr;
  a[6] = (long)Share; a[7] = (long)Disp; a[8] = (long)Opts;
  a[9] = (long)Ea; a[10] = (long)EaLen;
  return (NTSTATUS)nt_dispatch(NR_NtCreateFile, a);
}

NTSTATUS NtOpenFile(HANDLE *FileHandle, ULONG Access, OBJECT_ATTRIBUTES *Obj,
                    IO_STATUS_BLOCK *Iosb, ULONG Share, ULONG Opts) {
  ARGV(11);
  a[0] = (long)FileHandle; a[1] = (long)Access; a[2] = (long)Obj;
  a[3] = (long)Iosb; a[6] = (long)Share; a[7] = 1 /*FILE_OPEN*/; a[8] = (long)Opts;
  return (NTSTATUS)nt_dispatch(NR_NtOpenFile, a);
}

NTSTATUS NtReadFile(HANDLE fh, HANDLE ev, PVOID apc, PVOID apcctx,
                    IO_STATUS_BLOCK *Iosb, PVOID Buf, ULONG Len, PVOID off,
                    PVOID key) {
  ARGV(7);
  a[0] = (long)fh; a[1] = (long)ev; a[2] = (long)apc; a[3] = (long)apcctx;
  a[4] = (long)Iosb; a[5] = (long)Buf; a[6] = (long)Len;
  (void)off; (void)key;
  return (NTSTATUS)nt_dispatch(NR_NtReadFile, a);
}

NTSTATUS NtWriteFile(HANDLE fh, HANDLE ev, PVOID apc, PVOID apcctx,
                     IO_STATUS_BLOCK *Iosb, PVOID Buf, ULONG Len, PVOID off,
                     PVOID key) {
  ARGV(7);
  a[0] = (long)fh; a[1] = (long)ev; a[2] = (long)apc; a[3] = (long)apcctx;
  a[4] = (long)Iosb; a[5] = (long)Buf; a[6] = (long)Len;
  (void)off; (void)key;
  return (NTSTATUS)nt_dispatch(NR_NtWriteFile, a);
}

NTSTATUS NtClose(HANDLE h) {
  ARGV(1);
  a[0] = (long)h;
  return (NTSTATUS)nt_dispatch(NR_NtClose, a);
}

NTSTATUS NtAllocateVirtualMemory(HANDLE proc, PVOID *base, ULONG_PTR zero,
                                 ULONG_PTR *size, ULONG type, ULONG prot) {
  ARGV(6);
  (void)proc; (void)zero;
  a[1] = (long)base; a[3] = (long)size; a[4] = (long)type; a[5] = (long)prot;
  return (NTSTATUS)nt_dispatch(NR_NtAllocateVirtualMemory, a);
}

NTSTATUS NtFreeVirtualMemory(HANDLE proc, PVOID *base, ULONG_PTR *size,
                             ULONG type) {
  ARGV(4);
  (void)proc;
  a[1] = (long)base; a[2] = (long)size; a[3] = (long)type;
  return (NTSTATUS)nt_dispatch(NR_NtFreeVirtualMemory, a);
}

NTSTATUS NtDelayExecution(BOOL Alertable, W64S *Interval) {
  ARGV(2);
  a[0] = (long)Alertable; a[1] = (long)Interval;
  return (NTSTATUS)nt_dispatch(NR_NtDelayExecution, a);
}

NTSTATUS NtWaitForSingleObject(HANDLE h, BOOL Alertable, W64S *Timeout) {
  ARGV(3);
  a[0] = (long)h; a[1] = (long)Alertable; a[2] = (long)Timeout;
  return (NTSTATUS)nt_dispatch(NR_NtWaitForSingleObject, a);
}

NTSTATUS NtQueryInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf,
                                ULONG len, int cls) {
  ARGV(5);
  a[0] = (long)h; a[1] = (long)iosb; a[2] = (long)buf; a[3] = (long)len;
  a[4] = (long)cls;
  return (NTSTATUS)nt_dispatch(NR_NtQueryInformationFile, a);
}

NTSTATUS NtSetInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf,
                              ULONG len, int cls) {
  ARGV(5);
  a[0] = (long)h; a[1] = (long)iosb; a[2] = (long)buf; a[3] = (long)len;
  a[4] = (long)cls;
  return (NTSTATUS)nt_dispatch(NR_NtSetInformationFile, a);
}

void nt_init(void) { nt_register(); }
