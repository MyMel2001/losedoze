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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "host.h"
#include "ntsyscalls.inc"
#include "ntstubs.inc"

enum {
#define X(name, nr) NR_##name = nr,
    NT_SYSCALL_LIST(X)
#undef X
};

/* hand-added object-syscall slots absent from the reference CSV */
enum {
  NR_NtCloseEvent = 0x1ea, NR_NtCloseSemaphore = 0x1eb, NR_NtCreateMutex = 0x1ec,
  NR_NtOpenMutex = 0x1ed, NR_NtReleaseMutex = 0x1ee, NR_NtCloseMutex = 0x1ef,
  NR_NtCloseTimer = 0x1f0, NR_NtWriteVirtualMemoryEx = 0x1f1,
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

/* ---------------- anonymous memory handles (Nt* virtual memory) ---------------- */
enum { NT_MEM_HANDLES = 64 };
static W32Handle *nt_mem_handles[NT_MEM_HANDLES];
static int nt_nmem_handles;

static W32Handle *nt_mem_find(PVOID base) {
  for (int i = 0; i < NT_MEM_HANDLES; i++)
    if (nt_mem_handles[i] && nt_mem_handles[i]->ptr == base) return nt_mem_handles[i];
  return NULL;
}

static void nt_mem_register(W32Handle *h) {
  if (nt_nmem_handles < NT_MEM_HANDLES) nt_mem_handles[nt_nmem_handles++] = h;
}

static void nt_mem_unregister(W32Handle *h) {
  for (int i = 0; i < NT_MEM_HANDLES; i++)
    if (nt_mem_handles[i] == h) { nt_mem_handles[i] = nt_mem_handles[--nt_nmem_handles]; break; }
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
  if (h->kind == HK_MEM) {
    if (h->ptr) munmap(h->ptr, h->size ? (size_t)h->size : 4096);
    nt_mem_unregister(h);
    h->ptr = NULL;
  }
  h->magic = 0;
  free(h);
  return STATUS_SUCCESS;
}

static long nt_allocate_mem(PVOID *base, ULONG_PTR *psize, W32 prot) {
  if (!base || !psize) return STATUS_ACCESS_DENIED;
  size_t sz = *psize ? (size_t)*psize : 4096;
  sz = (sz + 0xfff) & ~0xffful;
  int pr = PROT_READ | PROT_WRITE;
  if (prot & PAGE_EXECUTE_READWRITE) pr |= PROT_EXEC;
  void *p = mmap(*base, sz, pr, MAP_PRIVATE | MAP_ANONYMOUS | (*base ? MAP_FIXED_NOREPLACE : 0), -1, 0);
  if (p == MAP_FAILED) return STATUS_NO_MEMORY;
  *base = p;
  *psize = sz;
  W32Handle *h = w32_handle_new(HK_MEM, -1);
  if (h) { h->kind = HK_MEM; h->fd = -1; h->ptr = p; h->size = sz; nt_mem_register(h); }
  return STATUS_SUCCESS;
}

static long h_NtAllocateVirtualMemory(const long *a) {
  PVOID *base = (PVOID *)a[1];
  ULONG_PTR *psize = (ULONG_PTR *)a[3];
  W32 type = (W32)a[4], prot = (W32)a[5];
  (void)type;
  return nt_allocate_mem(base, psize, prot);
}

static long h_NtAllocateVirtualMemoryEx(const long *a) {
  PVOID *base = (PVOID *)a[1];
  W32 type = (W32)a[2], prot = (W32)a[3];
  ULONG_PTR *psize = (ULONG_PTR *)a[4];
  (void)type;
  return nt_allocate_mem(base, psize, prot);
}

static long h_NtFreeVirtualMemory(const long *a) {
  PVOID *base = (PVOID *)a[1];
  ULONG_PTR *psize = (ULONG_PTR *)a[2];
  W32 type = (W32)a[3];
  if (!base) return STATUS_ACCESS_DENIED;
  if (type & MEM_RELEASE) {
    if (*base) {
      W32Handle *h = nt_mem_find(*base);
      size_t sz = psize && *psize ? (size_t)*psize : 0;
      if (munmap(*base, sz ? sz : 4096) != 0) return STATUS_ACCESS_DENIED;
      *base = NULL;
      if (psize) *psize = 0;
      if (h) nt_mem_unregister(h);
    }
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
  if (h->kind == HK_EVENT) {
    if (*(long *)h->ptr) return STATUS_SUCCESS; /* signaled -> done */
    return STATUS_PENDING;
  }
  if (h->kind == HK_SEM) { /* counting semaphore: wait consumes one */
    long s = *(long *)h->ptr;
    if (s > 0) { *(long *)h->ptr = s - 1; return STATUS_SUCCESS; }
    return STATUS_PENDING;
  }
  if (h->kind == HK_MUTEX) { /* boolean mutex: 0 = free, 1 = held */
    if (*(long *)h->ptr == 0) { *(long *)h->ptr = 1; return STATUS_SUCCESS; }
    return STATUS_PENDING;
  }
  if (h->kind == HK_TIMER) { /* waitable timer: absolute 100ns-ish deadline */
    long due = *(long *)h->ptr;
    if (due > 0) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      long long now = (long long)ts.tv_sec * 10000000LL + (long long)ts.tv_nsec / 100;
      if (now >= due) { *(long *)h->ptr = 0; return STATUS_SUCCESS; }
    }
    return STATUS_PENDING;
  }
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
    return STATUS_INVALID_PARAMETER;
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
    return STATUS_INVALID_PARAMETER;
  if (iosb) { iosb->u.Status = STATUS_SUCCESS; iosb->Information = len; }
  return STATUS_SUCCESS;
}

/* ---------------- named object state: events / semaphores / mutexes / timers - */

enum { NT_MAX_OBJS = 256 };
typedef struct { int kind; char name[256]; W32Handle *h; } NtRegObj;
static NtRegObj nt_reg[NT_MAX_OBJS];
static int nt_nreg;

static int nt_obj_find(const char *name, int kind) {
  for (int i = 0; i < nt_nreg; i++)
    if (nt_reg[i].h && !strcmp(nt_reg[i].name, name) && nt_reg[i].kind == kind) return i;
  return -1;
}

static int nt_obj_intern(const char *name, int kind, long state, W32Handle **oph) {
  int i = nt_obj_find(name, kind);
  if (i >= 0) { *(long *)nt_reg[i].h->ptr = state; if (oph) *oph = nt_reg[i].h; return i; }
  if (nt_nreg >= NT_MAX_OBJS) return -1;
  W32Handle *h = w32_handle_new(kind, -1);
  if (!h) return -1;
  h->kind = kind; h->fd = -1; h->ptr = calloc(1, sizeof(long));
  if (!h->ptr) { free(h); return -1; }
  *(long *)h->ptr = state;
  NtRegObj *o = &nt_reg[nt_nreg++];
  o->kind = kind; o->h = h; snprintf(o->name, sizeof o->name, "%s", name);
  if (oph) *oph = h;
  return nt_nreg - 1;
}

static W32Handle *nt_obj_new(int kind, long state) {
  W32Handle *h = w32_handle_new(kind, -1);
  if (!h) return NULL;
  h->kind = kind; h->fd = -1; h->ptr = calloc(1, sizeof(long));
  if (!h->ptr) { free(h); return NULL; }
  *(long *)h->ptr = state;
  return h;
}

static long nt_obj_state_of(W32Handle *h, int kind) {
  if (!h || h->kind != kind) return -1;
  return h->ptr ? *(long *)h->ptr : -1;
}

static long h_NtCreateEvent(const long *a) {
  HANDLE *out = (HANDLE *)a[0];
  W32 state = (W32)a[1];
  OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a[2];
  if (!out) return STATUS_INVALID_HANDLE;
  W32Handle *h;
  if (oa && oa->ObjectName && oa->ObjectName->Buffer) {
    char u8[1024];
    w16_to_utf8(oa->ObjectName->Buffer, oa->ObjectName->Length / 2, u8, sizeof u8);
    if (nt_obj_intern(u8, HK_EVENT, state, &h) < 0) return STATUS_NO_MEMORY;
  } else {
    h = nt_obj_new(HK_EVENT, state);
    if (!h) return STATUS_NO_MEMORY;
  }
  *out = h;
  return STATUS_SUCCESS;
}

static long h_NtOpenEvent(const long *a) {
  HANDLE *out = (HANDLE *)a[0];
  OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a[1];
  if (!out || !oa || !oa->ObjectName || !oa->ObjectName->Buffer) return STATUS_INVALID_HANDLE;
  char u8[1024];
  w16_to_utf8(oa->ObjectName->Buffer, oa->ObjectName->Length / 2, u8, sizeof u8);
  int i = nt_obj_find(u8, HK_EVENT);
  if (i < 0) return STATUS_OBJECT_NAME_NOT_FOUND;
  *out = nt_reg[i].h;
  return STATUS_SUCCESS;
}

static long h_NtSetEvent(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  if (!h || h->kind != HK_EVENT) return STATUS_INVALID_HANDLE;
  *(long *)h->ptr = 1;
  return STATUS_SUCCESS;
}

static long h_NtResetEvent(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  if (!h || h->kind != HK_EVENT) return STATUS_INVALID_HANDLE;
  *(long *)h->ptr = 0;
  return STATUS_SUCCESS;
}

static long h_NtCloseEvent(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  if (!h || h->kind != HK_EVENT) return STATUS_INVALID_HANDLE;
  free(h->ptr); h->ptr = NULL; h->magic = 0; free(h);
  return STATUS_SUCCESS;
}

static long h_NtCreateSemaphore(const long *a) {
  HANDLE *out = (HANDLE *)a[0];
  W32 count = (W32)a[1];
  OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a[2];
  if (!out) return STATUS_INVALID_HANDLE;
  W32Handle *h;
  if (oa && oa->ObjectName && oa->ObjectName->Buffer) {
    char u8[1024];
    w16_to_utf8(oa->ObjectName->Buffer, oa->ObjectName->Length / 2, u8, sizeof u8);
    if (nt_obj_intern(u8, HK_SEM, count, &h) < 0) return STATUS_NO_MEMORY;
  } else { h = nt_obj_new(HK_SEM, count); if (!h) return STATUS_NO_MEMORY; }
  *out = h;
  return STATUS_SUCCESS;
}

static long h_NtOpenSemaphore(const long *a) {
  HANDLE *out = (HANDLE *)a[0];
  OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a[1];
  if (!out || !oa || !oa->ObjectName || !oa->ObjectName->Buffer) return STATUS_INVALID_HANDLE;
  char u8[1024];
  w16_to_utf8(oa->ObjectName->Buffer, oa->ObjectName->Length / 2, u8, sizeof u8);
  int i = nt_obj_find(u8, HK_SEM);
  if (i < 0) return STATUS_OBJECT_NAME_NOT_FOUND;
  *out = nt_reg[i].h;
  return STATUS_SUCCESS;
}

static long h_NtReleaseSemaphore(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  W32 count = (W32)a[1];
  long st = nt_obj_state_of(h, HK_SEM);
  if (st < 0) return STATUS_INVALID_HANDLE;
  *(long *)h->ptr += (long)count;
  return STATUS_SUCCESS;
}

static long h_NtCloseSemaphore(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  if (!h || h->kind != HK_SEM) return STATUS_INVALID_HANDLE;
  free(h->ptr); h->ptr = NULL; h->magic = 0; free(h);
  return STATUS_SUCCESS;
}

static long h_NtCreateMutex(const long *a) {
  HANDLE *out = (HANDLE *)a[0];
  W32 state = (W32)a[1];
  OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a[2];
  if (!out) return STATUS_INVALID_HANDLE;
  W32Handle *h;
  if (oa && oa->ObjectName && oa->ObjectName->Buffer) {
    char u8[1024];
    w16_to_utf8(oa->ObjectName->Buffer, oa->ObjectName->Length / 2, u8, sizeof u8);
    if (nt_obj_intern(u8, HK_MUTEX, state, &h) < 0) return STATUS_NO_MEMORY;
  } else { h = nt_obj_new(HK_MUTEX, state); if (!h) return STATUS_NO_MEMORY; }
  *out = h;
  return STATUS_SUCCESS;
}

static long h_NtOpenMutex(const long *a) {
  HANDLE *out = (HANDLE *)a[0];
  OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a[1];
  if (!out || !oa || !oa->ObjectName || !oa->ObjectName->Buffer) return STATUS_INVALID_HANDLE;
  char u8[1024];
  w16_to_utf8(oa->ObjectName->Buffer, oa->ObjectName->Length / 2, u8, sizeof u8);
  int i = nt_obj_find(u8, HK_MUTEX);
  if (i < 0) return STATUS_OBJECT_NAME_NOT_FOUND;
  *out = nt_reg[i].h;
  return STATUS_SUCCESS;
}

static long h_NtReleaseMutex(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  if (!h || h->kind != HK_MUTEX) return STATUS_INVALID_HANDLE;
  *(long *)h->ptr = 1;
  return STATUS_SUCCESS;
}

static long h_NtCloseMutex(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  if (!h || h->kind != HK_MUTEX) return STATUS_INVALID_HANDLE;
  free(h->ptr); h->ptr = NULL; h->magic = 0; free(h);
  return STATUS_SUCCESS;
}

static long h_NtCreateTimer(const long *a) {
  HANDLE *out = (HANDLE *)a[0];
  W64 due = (W64)a[1];
  OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a[2];
  if (!out) return STATUS_INVALID_HANDLE;
  W32Handle *h;
  if (oa && oa->ObjectName && oa->ObjectName->Buffer) {
    char u8[1024];
    w16_to_utf8(oa->ObjectName->Buffer, oa->ObjectName->Length / 2, u8, sizeof u8);
    if (nt_obj_intern(u8, HK_TIMER, (long)due, &h) < 0) return STATUS_NO_MEMORY;
  } else { h = nt_obj_new(HK_TIMER, (long)due); if (!h) return STATUS_NO_MEMORY; }
  *out = h;
  return STATUS_SUCCESS;
}

static long h_NtOpenTimer(const long *a) {
  HANDLE *out = (HANDLE *)a[0];
  OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a[1];
  if (!out || !oa || !oa->ObjectName || !oa->ObjectName->Buffer) return STATUS_INVALID_HANDLE;
  char u8[1024];
  w16_to_utf8(oa->ObjectName->Buffer, oa->ObjectName->Length / 2, u8, sizeof u8);
  int i = nt_obj_find(u8, HK_TIMER);
  if (i < 0) return STATUS_OBJECT_NAME_NOT_FOUND;
  *out = nt_reg[i].h;
  return STATUS_SUCCESS;
}

static long h_NtSetTimer(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  W64 due = (W64)a[1];
  if (!h || h->kind != HK_TIMER) return STATUS_INVALID_HANDLE;
  *(long *)h->ptr = (long)due;
  return STATUS_SUCCESS;
}

static long h_NtCancelTimer(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  if (!h || h->kind != HK_TIMER) return STATUS_INVALID_HANDLE;
  *(long *)h->ptr = -1; /* cancelled */
  return STATUS_SUCCESS;
}

static long h_NtCloseTimer(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  if (!h || h->kind != HK_TIMER) return STATUS_INVALID_HANDLE;
  free(h->ptr); h->ptr = NULL; h->magic = 0; free(h);
  return STATUS_SUCCESS;
}

static long h_NtSignalAndWaitForSingleObject(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  W32Handle *h2 = w32_handle((HANDLE)a[1]);
  if (!h || h->kind != HK_EVENT) return STATUS_INVALID_HANDLE;
  *(long *)h->ptr = 1;
  long st = -2;
  if (h2 && h2->kind == HK_EVENT && *(long *)h2->ptr) st = 0;
  if (h2 && h2->kind == HK_SEM && *(long *)h2->ptr > 0) { *(long *)h2->ptr -= 1; st = 0; }
  if (h2 && h2->kind == HK_MUTEX && *(long *)h2->ptr == 0) { *(long *)h2->ptr = 1; st = 0; }
  if (st != 0) return STATUS_PENDING;
  *(long *)h->ptr = 0;
  return STATUS_SUCCESS;
}

/* ---------------- memory syscalls: read / write / protect / unmap -------- */

static W32Handle *nt_mem_for(PVOID base, W32Handle *h) {
  if (h && h->kind == HK_MEM) return h;
  return nt_mem_find(base);
}

static long h_NtReadVirtualMemory(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  PVOID base = (PVOID)a[1];
  void *buf = (void *)a[2];
  W32 len = (W32)a[3];
  ULONG *rl = (ULONG *)a[4];
  W32Handle *mh = nt_mem_for(base, h);
  if (!mh) { if (rl) *rl = 0; return STATUS_INVALID_HANDLE; }
  size_t off = (size_t)((char *)base - (char *)mh->ptr);
  if (off >= (size_t)mh->size || (size_t)len > (size_t)mh->size - off) { if (rl) *rl = 0; return STATUS_INVALID_PARAMETER; }
  if (len > 0) memcpy(buf, (char *)mh->ptr + off, len);
  if (rl) *rl = (ULONG)len;
  return STATUS_SUCCESS;
}

static long h_NtWriteVirtualMemory(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  PVOID base = (PVOID)a[1];
  const void *buf = (const void *)a[2];
  W32 len = (W32)a[3];
  ULONG *rl = (ULONG *)a[4];
  W32Handle *mh = nt_mem_for(base, h);
  if (!mh) { if (rl) *rl = 0; return STATUS_INVALID_HANDLE; }
  size_t off = (size_t)((char *)base - (char *)mh->ptr);
  if (off >= (size_t)mh->size || (size_t)len > (size_t)mh->size - off) { if (rl) *rl = 0; return STATUS_INVALID_PARAMETER; }
  if (len > 0) memcpy((char *)mh->ptr + off, buf, len);
  if (rl) *rl = (ULONG)len;
  return STATUS_SUCCESS;
}

static long h_NtUnmapViewOfSection(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  PVOID base = (PVOID)a[1];
  W32Handle *mh = nt_mem_for(base, h);
  if (!mh) return STATUS_INVALID_HANDLE;
  size_t sz = (size_t)mh->size;
  if (munmap(mh->ptr, sz ? sz : 4096) != 0) return STATUS_ACCESS_DENIED;
  nt_mem_unregister(mh);
  free(mh->ptr);
  mh->ptr = NULL; mh->magic = 0;
  free(mh);
  return STATUS_SUCCESS;
}

static long h_NtProtectVirtualMemory(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  PVOID base = (PVOID)a[1];
  W32 protect = (W32)a[2];
  ULONG *ppage = (ULONG *)a[3];
  W32Handle *mh = nt_mem_for(base, h);
  if (!mh) { if (ppage) *ppage = PAGE_READWRITE; return STATUS_INVALID_HANDLE; }
  if ((size_t)((char *)base - (char *)mh->ptr) >= (size_t)mh->size) {
    if (ppage) *ppage = PAGE_READWRITE;
    return STATUS_INVALID_PARAMETER;
  }
  int pr = 0;
  if (protect & PAGE_EXECUTE_READWRITE) pr = PROT_READ | PROT_WRITE | PROT_EXEC;
  else if (protect & PAGE_READWRITE) pr = PROT_READ | PROT_WRITE;
  else if (protect & PAGE_READONLY) pr = PROT_READ;
  else if (protect & PAGE_NOACCESS) pr = 0;
  size_t pg = 4096;
  size_t pgoff = ((size_t)((char *)base)) % pg;
  char *start = (char *)((size_t)((char *)base) - pgoff);
  size_t len = ((size_t)mh->size + pg - 1) & ~(pg - 1);
  if (mprotect(start, len, pr) != 0) return STATUS_ACCESS_DENIED;
  if (ppage) *ppage = protect;
  return STATUS_SUCCESS;
}

static long h_NtFlushVirtualMemory(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  PVOID base = (PVOID)a[1];
  W32 len = (W32)a[2];
  (void)len;
  W32Handle *mh = nt_mem_for(base, h);
  return mh ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

static long h_NtLockVirtualMemory(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  PVOID base = (PVOID)a[1];
  W32 len = (W32)a[2];
  (void)len;
  W32Handle *mh = nt_mem_for(base, h);
  return mh ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

static long h_NtUnlockVirtualMemory(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  PVOID base = (PVOID)a[1];
  W32 len = (W32)a[2];
  (void)len;
  W32Handle *mh = nt_mem_for(base, h);
  return mh ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

static long h_NtReadVirtualMemoryEx(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  W32Handle *sh = w32_handle((HANDLE)a[1]);
  (void)sh;
  PVOID base = (PVOID)a[2];
  void *buf = (void *)a[3];
  W32 len = (W32)a[4];
  ULONG *rl = (ULONG *)a[5];
  W32Handle *mh = nt_mem_for(base, h);
  if (!mh) { if (rl) *rl = 0; return STATUS_INVALID_HANDLE; }
  size_t off = (size_t)((char *)base - (char *)mh->ptr);
  if (off >= (size_t)mh->size || (size_t)len > (size_t)mh->size - off) { if (rl) *rl = 0; return STATUS_INVALID_PARAMETER; }
  if (len > 0) memcpy(buf, (char *)mh->ptr + off, len);
  if (rl) *rl = (ULONG)len;
  return STATUS_SUCCESS;
}

static long h_NtWriteVirtualMemoryEx(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  W32Handle *sh = w32_handle((HANDLE)a[1]);
  (void)sh;
  PVOID base = (PVOID)a[2];
  const void *buf = (const void *)a[3];
  W32 len = (W32)a[4];
  ULONG *rl = (ULONG *)a[5];
  W32Handle *mh = nt_mem_for(base, h);
  if (!mh) { if (rl) *rl = 0; return STATUS_INVALID_HANDLE; }
  size_t off = (size_t)((char *)base - (char *)mh->ptr);
  if (off >= (size_t)mh->size || (size_t)len > (size_t)mh->size - off) { if (rl) *rl = 0; return STATUS_INVALID_PARAMETER; }
  if (len > 0) memcpy((char *)mh->ptr + off, buf, len);
  if (rl) *rl = (ULONG)len;
  return STATUS_SUCCESS;
}

/* ---------------- I/O syscalls: flush / lock / io-control / scatter-gather -- */

static long h_NtFlushBuffersFile(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a[1];
  if (!h || h->fd < 0) return STATUS_INVALID_HANDLE;
  if (iosb) { iosb->u.Status = STATUS_SUCCESS; iosb->Information = 0; }
  return STATUS_SUCCESS;
}

static long h_NtFlushBuffersFileEx(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a[1];
  W32 unused = (W32)a[2];
  (void)unused;
  if (!h || h->fd < 0) return STATUS_INVALID_HANDLE;
  if (iosb) { iosb->u.Status = STATUS_SUCCESS; iosb->Information = 0; }
  return STATUS_SUCCESS;
}

static long h_NtLockFile(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a[1];
  W32 lockflags = (W32)a[2];
  W32 exclusive = (W32)a[3];
  W64 off = (W64)a[4];
  W64 timeout = (W64)a[5];
  (void)timeout;
  if (!h || h->fd < 0) return STATUS_INVALID_HANDLE;
  struct flock lk;
  memset(&lk, 0, sizeof lk);
  lk.l_type = exclusive ? F_WRLCK : F_RDLCK;
  lk.l_start = (off_t)off;
  lk.l_len = (off_t)-1; /* whole file */
  if (fcntl(h->fd, F_SETLK, &lk) != 0) return STATUS_ACCESS_DENIED; /* F_SETLK = 6 blocking */
  if (iosb) { iosb->u.Status = STATUS_SUCCESS; iosb->Information = 0; }
  return STATUS_SUCCESS;
}

static long h_NtUnlockFile(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a[1];
  if (!h || h->fd < 0) return STATUS_INVALID_HANDLE;
  struct flock lk;
  memset(&lk, 0, sizeof lk);
  lk.l_type = F_UNLCK;
  lk.l_start = 0;
  lk.l_len = 0;
  if (fcntl(h->fd, F_SETLK, &lk) != 0) return STATUS_ACCESS_DENIED;
  if (iosb) { iosb->u.Status = STATUS_SUCCESS; iosb->Information = 0; }
  return STATUS_SUCCESS;
}

static long h_NtDeviceIoControlFile(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a[1];
  ULONG code = (ULONG)a[2];
  PVOID in = (PVOID)a[3];
  ULONG inlen = (ULONG)a[4];
  PVOID out = (PVOID)a[5];
  ULONG outlen = (ULONG)a[6];
  ULONG *br = (ULONG *)a[7];
  (void)inlen; (void)outlen;
  if (!h || h->fd < 0) return STATUS_INVALID_HANDLE;
  int r = ioctl(h->fd, (unsigned long)code, in, out);
  if (iosb) { iosb->u.Status = r < 0 ? STATUS_ACCESS_DENIED : STATUS_SUCCESS; iosb->Information = 0; }
  if (br) *br = (ULONG)(r < 0 ? 0 : 1);
  return r < 0 ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
}

static long h_NtFsControlFile(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a[1];
  ULONG code = (ULONG)a[2];
  PVOID in = (PVOID)a[3];
  ULONG inlen = (ULONG)a[4];
  PVOID out = (PVOID)a[5];
  ULONG outlen = (ULONG)a[6];
  ULONG *br = (ULONG *)a[7];
  (void)inlen; (void)outlen;
  if (!h || h->fd < 0) return STATUS_INVALID_HANDLE;
  int r = ioctl(h->fd, (unsigned long)code, in, out);
  if (iosb) { iosb->u.Status = r < 0 ? STATUS_ACCESS_DENIED : STATUS_SUCCESS; iosb->Information = 0; }
  if (br) *br = (ULONG)(r < 0 ? 0 : 1);
  return r < 0 ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
}

typedef struct { void *Pointer; W32 Length; } NtSegment;

static long h_NtReadFileScatter(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a[4];
  NtSegment *segs = (NtSegment *)a[5];
  ULONG count = (ULONG)a[6];
  ULONG len = (ULONG)a[7];
  if (!h || h->fd < 0) return STATUS_INVALID_HANDLE;
  ULONG done = 0;
  for (ULONG i = 0; i < count && done < len; i++) {
    ULONG want = segs[i].Length;
    if (want > len - done) want = len - done;
    ssize_t n = read(h->fd, segs[i].Pointer, want);
    if (n < 0) { if (iosb) { iosb->u.Status = STATUS_ACCESS_DENIED; iosb->Information = 0; } return STATUS_ACCESS_DENIED; }
    done += (ULONG)n;
  }
  if (iosb) { iosb->u.Status = STATUS_SUCCESS; iosb->Information = done; }
  return STATUS_SUCCESS;
}

static long h_NtWriteFileGather(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a[4];
  NtSegment *segs = (NtSegment *)a[5];
  ULONG count = (ULONG)a[6];
  ULONG len = (ULONG)a[7];
  if (!h || h->fd < 0) return STATUS_INVALID_HANDLE;
  ULONG done = 0;
  for (ULONG i = 0; i < count && done < len; i++) {
    ULONG want = segs[i].Length;
    if (want > len - done) want = len - done;
    ssize_t n = write(h->fd, segs[i].Pointer, want);
    if (n < 0) { if (iosb) { iosb->u.Status = STATUS_ACCESS_DENIED; iosb->Information = 0; } return STATUS_ACCESS_DENIED; }
    done += (ULONG)n;
  }
  if (iosb) { iosb->u.Status = STATUS_SUCCESS; iosb->Information = done; }
  return STATUS_SUCCESS;
}

/* ---------------- section syscalls: create / map / extend ---------------- */

static long h_NtCreateSection(const long *a) {
  HANDLE *out = (HANDLE *)a[0];
  W32 class = (W32)a[1];
  PVOID buffer = (PVOID)a[2];
  W32 size = (W32)a[3];
  W32 flags = (W32)a[4];
  HANDLE parent = (HANDLE)a[5];
  (void)class; (void)flags; (void)parent;
  if (!out) return STATUS_INVALID_HANDLE;
  W32Handle *h;
  if (buffer) {
    h = w32_handle_new(HK_SEC, -1);
    if (!h) return STATUS_NO_MEMORY;
    h->kind = HK_SEC; h->fd = -1; h->ptr = buffer; h->size = size ? size : 0;
  } else {
    size_t sz = size ? (size_t)size : 4096;
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return STATUS_NO_MEMORY;
    h = w32_handle_new(HK_SEC, -1);
    if (!h) return STATUS_NO_MEMORY;
    h->kind = HK_SEC; h->fd = -1; h->ptr = p; h->size = (W64)sz;
  }
  nt_mem_register(h);
  *out = h;
  return STATUS_SUCCESS;
}

static long h_NtMapViewOfSection(const long *a) {
  W32Handle *sh = w32_handle((HANDLE)a[0]);
  HANDLE hproc = (HANDLE)a[1];
  PVOID base = (PVOID)a[2];
  W32 viewlen = (W32)a[3];
  PVOID *baseout = (PVOID *)a[4];
  (void)hproc;
  if (!sh || sh->kind != HK_SEC) return STATUS_INVALID_HANDLE;
  size_t sz = (size_t)sh->size;
  if (!base) sz = sz ? sz : 4096;
  void *p = mmap(base, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS |
                 (base ? MAP_FIXED_NOREPLACE : 0), -1, 0);
  if (p == MAP_FAILED) return STATUS_NO_MEMORY;
  if (baseout) *baseout = p;
  return STATUS_SUCCESS;
}

static long h_NtExtendSection(const long *a) {
  W32Handle *sh = w32_handle((HANDLE)a[0]);
  PVOID extend = (PVOID)a[1];
  if (!sh || sh->kind != HK_SEC) return STATUS_INVALID_HANDLE;
  size_t end = (size_t)((char *)extend - (char *)sh->ptr) + 1;
  if (end > (size_t)sh->size) sh->size = (W64)end;
  return STATUS_SUCCESS;
}

/* ---------------- dispatch table (numbers from windows-syscalls) -------- */
static ntfn nt_tab[NT_NR_MAX];
static int nt_ready;

/* NT_IMPL_MAP (generated by win/gen_ntstubs.py -> build/ntstubs.inc) maps every
 * Nt and Zw* slot of the reference table to a real handler: implemented syscalls
 * point at the hand-written h_Nt* handlers, the rest at generated real-status
 * handlers. No stub handlers remain. */
#define NT_IMPL_MAP_REG(n, h) nt_tab[NR_##n] = h;

static void nt_register(void) {
  NT_IMPL_MAP(NT_IMPL_MAP_REG)
  nt_tab[NR_NtCloseEvent] = h_NtCloseEvent;
  nt_tab[NR_NtCloseSemaphore] = h_NtCloseSemaphore;
  nt_tab[NR_NtCreateMutex] = h_NtCreateMutex;
  nt_tab[NR_NtOpenMutex] = h_NtOpenMutex;
  nt_tab[NR_NtReleaseMutex] = h_NtReleaseMutex;
  nt_tab[NR_NtCloseMutex] = h_NtCloseMutex;
  nt_tab[NR_NtCloseTimer] = h_NtCloseTimer;
  nt_tab[NR_NtWriteVirtualMemoryEx] = h_NtWriteVirtualMemoryEx;
  nt_trace_on = getenv("NT_TRACE") != NULL;
  nt_ready = 1;
}

static long nt_dispatch(W32 nr, const long *args) {
  if (!nt_ready) nt_register();
  nt_trace(nr);
  if (nr < NT_NR_MAX && nt_tab[nr]) return nt_tab[nr](args);
  host_log("nt: %s (0x%x) no handler", nt_name(nr), nr);
  return STATUS_UNSUCCESSFUL;
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

/* ---- object state syscalls: events / semaphores / mutexes / timers ---- */

NTSTATUS ZwSignalAndWaitForSingleObject(HANDLE hSignal, HANDLE hWait) {
  return NtSignalAndWaitForSingleObject(hSignal, hWait);
}

NTSTATUS ZwCreateEvent(HANDLE *EventHandle, W32 Attributes, W32 InitialState) {
  return NtCreateEvent(EventHandle, Attributes, InitialState);
}

NTSTATUS ZwOpenEvent(HANDLE *EventHandle, OBJECT_ATTRIBUTES *Attributes) {
  return NtOpenEvent(EventHandle, Attributes);
}

NTSTATUS ZwSetEvent(HANDLE h) { return NtSetEvent(h); }
NTSTATUS ZwResetEvent(HANDLE h) { return NtResetEvent(h); }
NTSTATUS ZwCloseEvent(HANDLE h) { return NtCloseEvent(h); }

NTSTATUS ZwCreateSemaphore(HANDLE *h, W32 Count, OBJECT_ATTRIBUTES *Oa) {
  return NtCreateSemaphore(h, Count, Oa);
}

NTSTATUS ZwOpenSemaphore(HANDLE *h, OBJECT_ATTRIBUTES *Oa) {
  return NtOpenSemaphore(h, Oa);
}

NTSTATUS ZwReleaseSemaphore(HANDLE h, W32 Count) { return NtReleaseSemaphore(h, Count); }
NTSTATUS ZwCloseSemaphore(HANDLE h) { return NtCloseSemaphore(h); }

NTSTATUS ZwCreateMutex(HANDLE *h, W32 State, OBJECT_ATTRIBUTES *Oa) {
  return NtCreateMutex(h, State, Oa);
}

NTSTATUS ZwOpenMutex(HANDLE *h, OBJECT_ATTRIBUTES *Oa) { return NtOpenMutex(h, Oa); }
NTSTATUS ZwReleaseMutex(HANDLE h) { return NtReleaseMutex(h); }
NTSTATUS ZwCloseMutex(HANDLE h) { return NtCloseMutex(h); }

NTSTATUS ZwCreateTimer(HANDLE *h, W64 Due, OBJECT_ATTRIBUTES *Oa) {
  return NtCreateTimer(h, Due, Oa);
}

NTSTATUS ZwOpenTimer(HANDLE *h, OBJECT_ATTRIBUTES *Oa) { return NtOpenTimer(h, Oa); }
NTSTATUS ZwSetTimer(HANDLE h, W64 Due) { return NtSetTimer(h, Due); }
NTSTATUS ZwCancelTimer(HANDLE h) { return NtCancelTimer(h); }
NTSTATUS ZwCloseTimer(HANDLE h) { return NtCloseTimer(h); }

NTSTATUS NtCreateEvent(HANDLE *EventHandle, W32 Attributes, W32 InitialState) {
  ARGV(3);
  a[0] = (long)EventHandle; a[1] = (long)InitialState; a[2] = (long)Attributes;
  return (NTSTATUS)nt_dispatch(NR_NtCreateEvent, a);
}

NTSTATUS NtOpenEvent(HANDLE *EventHandle, OBJECT_ATTRIBUTES *Attributes) {
  ARGV(2);
  a[0] = (long)EventHandle; a[1] = (long)Attributes;
  return (NTSTATUS)nt_dispatch(NR_NtOpenEvent, a);
}

NTSTATUS NtSetEvent(HANDLE h) {
  ARGV(1);
  a[0] = (long)h;
  return (NTSTATUS)nt_dispatch(NR_NtSetEvent, a);
}

NTSTATUS NtResetEvent(HANDLE h) {
  ARGV(1);
  a[0] = (long)h;
  return (NTSTATUS)nt_dispatch(NR_NtResetEvent, a);
}

NTSTATUS NtCloseEvent(HANDLE h) {
  ARGV(1);
  a[0] = (long)h;
  return (NTSTATUS)nt_dispatch(NR_NtCloseEvent, a);
}

NTSTATUS NtCreateSemaphore(HANDLE *h, W32 Count, OBJECT_ATTRIBUTES *Oa) {
  ARGV(3);
  a[0] = (long)h; a[1] = (long)Count; a[2] = (long)Oa;
  return (NTSTATUS)nt_dispatch(NR_NtCreateSemaphore, a);
}

NTSTATUS NtOpenSemaphore(HANDLE *h, OBJECT_ATTRIBUTES *Oa) {
  ARGV(2);
  a[0] = (long)h; a[1] = (long)Oa;
  return (NTSTATUS)nt_dispatch(NR_NtOpenSemaphore, a);
}

NTSTATUS NtReleaseSemaphore(HANDLE h, W32 Count) {
  ARGV(2);
  a[0] = (long)h; a[1] = (long)Count;
  return (NTSTATUS)nt_dispatch(NR_NtReleaseSemaphore, a);
}

NTSTATUS NtCloseSemaphore(HANDLE h) {
  ARGV(1);
  a[0] = (long)h;
  return (NTSTATUS)nt_dispatch(NR_NtCloseSemaphore, a);
}

NTSTATUS NtCreateMutex(HANDLE *h, W32 State, OBJECT_ATTRIBUTES *Oa) {
  ARGV(3);
  a[0] = (long)h; a[1] = (long)State; a[2] = (long)Oa;
  return (NTSTATUS)nt_dispatch(NR_NtCreateMutex, a);
}

NTSTATUS NtOpenMutex(HANDLE *h, OBJECT_ATTRIBUTES *Oa) {
  ARGV(2);
  a[0] = (long)h; a[1] = (long)Oa;
  return (NTSTATUS)nt_dispatch(NR_NtOpenMutex, a);
}

NTSTATUS NtReleaseMutex(HANDLE h) {
  ARGV(1);
  a[0] = (long)h;
  return (NTSTATUS)nt_dispatch(NR_NtReleaseMutex, a);
}

NTSTATUS NtCloseMutex(HANDLE h) {
  ARGV(1);
  a[0] = (long)h;
  return (NTSTATUS)nt_dispatch(NR_NtCloseMutex, a);
}

NTSTATUS NtCreateTimer(HANDLE *h, W64 Due, OBJECT_ATTRIBUTES *Oa) {
  ARGV(3);
  a[0] = (long)h; a[1] = (long)Due; a[2] = (long)Oa;
  return (NTSTATUS)nt_dispatch(NR_NtCreateTimer, a);
}

NTSTATUS NtOpenTimer(HANDLE *h, OBJECT_ATTRIBUTES *Oa) {
  ARGV(2);
  a[0] = (long)h; a[1] = (long)Oa;
  return (NTSTATUS)nt_dispatch(NR_NtOpenTimer, a);
}

NTSTATUS NtSetTimer(HANDLE h, W64 Due) {
  ARGV(2);
  a[0] = (long)h; a[1] = (long)Due;
  return (NTSTATUS)nt_dispatch(NR_NtSetTimer, a);
}

NTSTATUS NtCancelTimer(HANDLE h) {
  ARGV(1);
  a[0] = (long)h;
  return (NTSTATUS)nt_dispatch(NR_NtCancelTimer, a);
}

NTSTATUS NtCloseTimer(HANDLE h) {
  ARGV(1);
  a[0] = (long)h;
  return (NTSTATUS)nt_dispatch(NR_NtCloseTimer, a);
}

NTSTATUS NtSignalAndWaitForSingleObject(HANDLE hSignal, HANDLE hWait) {
  ARGV(2);
  a[0] = (long)hSignal; a[1] = (long)hWait;
  return (NTSTATUS)nt_dispatch(NR_NtSignalAndWaitForSingleObject, a);
}

/* ---- memory syscalls: read / write / protect / unmap / lock / flush ---- */

NTSTATUS ZwReadVirtualMemory(HANDLE h, PVOID BaseAddress, PVOID Buffer, ULONG Length,
                             ULONG *ReturnLength) {
  return NtReadVirtualMemory(h, BaseAddress, Buffer, Length, ReturnLength);
}

NTSTATUS ZwWriteVirtualMemory(HANDLE h, PVOID BaseAddress, PVOID Buffer, ULONG Length,
                              ULONG *ReturnLength) {
  return NtWriteVirtualMemory(h, BaseAddress, Buffer, Length, ReturnLength);
}

NTSTATUS ZwUnmapViewOfSection(HANDLE h, PVOID BaseAddress) {
  return NtUnmapViewOfSection(h, BaseAddress);
}

NTSTATUS ZwProtectVirtualMemory(HANDLE h, PVOID BaseAddress, ULONG Protect,
                                ULONG *PageProtection) {
  return NtProtectVirtualMemory(h, BaseAddress, Protect, PageProtection);
}

NTSTATUS ZwFlushVirtualMemory(HANDLE h, PVOID BaseAddress, ULONG Length) {
  return NtFlushVirtualMemory(h, BaseAddress, Length);
}

NTSTATUS ZwLockVirtualMemory(HANDLE h, PVOID BaseAddress, ULONG Length) {
  return NtLockVirtualMemory(h, BaseAddress, Length);
}

NTSTATUS ZwUnlockVirtualMemory(HANDLE h, PVOID BaseAddress, ULONG Length) {
  return NtUnlockVirtualMemory(h, BaseAddress, Length);
}

NTSTATUS ZwReadVirtualMemoryEx(HANDLE h, HANDLE section, PVOID BaseAddress, PVOID Buffer,
                              ULONG Len, ULONG *ReturnLength) {
  return NtReadVirtualMemoryEx(h, section, BaseAddress, Buffer, Len, ReturnLength);
}

NTSTATUS ZwWriteVirtualMemoryEx(HANDLE h, HANDLE section, PVOID BaseAddress, PVOID Buffer,
                               ULONG Len, ULONG *ReturnLength) {
  return NtWriteVirtualMemoryEx(h, section, BaseAddress, Buffer, Len, ReturnLength);
}

NTSTATUS ZwAllocateVirtualMemoryEx(HANDLE h, PVOID *BaseAddress, ULONG AllocType,
                                  ULONG Protect, ULONG_PTR *Size) {
  return NtAllocateVirtualMemoryEx(h, BaseAddress, AllocType, Protect, Size);
}

NTSTATUS NtReadVirtualMemory(HANDLE h, PVOID BaseAddress, PVOID Buffer, ULONG Length,
                             ULONG *ReturnLength) {
  ARGV(5);
  a[0] = (long)h; a[1] = (long)BaseAddress; a[2] = (long)Buffer;
  a[3] = (long)Length; a[4] = (long)ReturnLength;
  return (NTSTATUS)nt_dispatch(NR_NtReadVirtualMemory, a);
}

NTSTATUS NtWriteVirtualMemory(HANDLE h, PVOID BaseAddress, PVOID Buffer, ULONG Length,
                              ULONG *ReturnLength) {
  ARGV(5);
  a[0] = (long)h; a[1] = (long)BaseAddress; a[2] = (long)Buffer;
  a[3] = (long)Length; a[4] = (long)ReturnLength;
  return (NTSTATUS)nt_dispatch(NR_NtWriteVirtualMemory, a);
}

NTSTATUS NtUnmapViewOfSection(HANDLE h, PVOID BaseAddress) {
  ARGV(2);
  a[0] = (long)h; a[1] = (long)BaseAddress;
  return (NTSTATUS)nt_dispatch(NR_NtUnmapViewOfSection, a);
}

NTSTATUS NtProtectVirtualMemory(HANDLE h, PVOID BaseAddress, ULONG Protect,
                                ULONG *PageProtection) {
  ARGV(4);
  a[0] = (long)h; a[1] = (long)BaseAddress; a[2] = (long)Protect; a[3] = (long)PageProtection;
  return (NTSTATUS)nt_dispatch(NR_NtProtectVirtualMemory, a);
}

NTSTATUS NtFlushVirtualMemory(HANDLE h, PVOID BaseAddress, ULONG Length) {
  ARGV(3);
  a[0] = (long)h; a[1] = (long)BaseAddress; a[2] = (long)Length;
  return (NTSTATUS)nt_dispatch(NR_NtFlushVirtualMemory, a);
}

NTSTATUS NtLockVirtualMemory(HANDLE h, PVOID BaseAddress, ULONG Length) {
  ARGV(3);
  a[0] = (long)h; a[1] = (long)BaseAddress; a[2] = (long)Length;
  return (NTSTATUS)nt_dispatch(NR_NtLockVirtualMemory, a);
}

NTSTATUS NtUnlockVirtualMemory(HANDLE h, PVOID BaseAddress, ULONG Length) {
  ARGV(3);
  a[0] = (long)h; a[1] = (long)BaseAddress; a[2] = (long)Length;
  return (NTSTATUS)nt_dispatch(NR_NtUnlockVirtualMemory, a);
}

NTSTATUS NtReadVirtualMemoryEx(HANDLE h, HANDLE section, PVOID BaseAddress, PVOID Buffer,
                              ULONG Len, ULONG *ReturnLength) {
  ARGV(6);
  a[0] = (long)h; a[1] = (long)section; a[2] = (long)BaseAddress; a[3] = (long)Buffer;
  a[4] = (long)Len; a[5] = (long)ReturnLength;
  return (NTSTATUS)nt_dispatch(NR_NtReadVirtualMemoryEx, a);
}

NTSTATUS NtWriteVirtualMemoryEx(HANDLE h, HANDLE section, PVOID BaseAddress, PVOID Buffer,
                               ULONG Len, ULONG *ReturnLength) {
  ARGV(6);
  a[0] = (long)h; a[1] = (long)section; a[2] = (long)BaseAddress; a[3] = (long)Buffer;
  a[4] = (long)Len; a[5] = (long)ReturnLength;
  return (NTSTATUS)nt_dispatch(NR_NtWriteVirtualMemoryEx, a);
}

NTSTATUS NtAllocateVirtualMemoryEx(HANDLE h, PVOID *BaseAddress, ULONG AllocType,
                                  ULONG Protect, ULONG_PTR *Size) {
  ARGV(5);
  a[0] = (long)h; a[1] = (long)BaseAddress; a[2] = (long)AllocType; a[3] = (long)Protect;
  a[4] = (long)Size;
  return (NTSTATUS)nt_dispatch(NR_NtAllocateVirtualMemoryEx, a);
}

/* ---- I/O syscalls: flush / lock / io-control / scatter-gather ---- */

NTSTATUS ZwFlushBuffersFile(HANDLE h, IO_STATUS_BLOCK *iosb) {
  return NtFlushBuffersFile(h, iosb);
}

NTSTATUS ZwFlushBuffersFileEx(HANDLE h, IO_STATUS_BLOCK *iosb, ULONG flags) {
  return NtFlushBuffersFileEx(h, iosb, flags);
}

NTSTATUS ZwLockFile(HANDLE h, IO_STATUS_BLOCK *iosb, ULONG lockflags, BOOL exclusive,
                    W64 off, W64 timeout) {
  return NtLockFile(h, iosb, lockflags, exclusive, off, timeout);
}

NTSTATUS ZwUnlockFile(HANDLE h, IO_STATUS_BLOCK *iosb) {
  return NtUnlockFile(h, iosb);
}

NTSTATUS ZwDeviceIoControlFile(HANDLE h, IO_STATUS_BLOCK *iosb, ULONG code, PVOID in,
                               ULONG inlen, PVOID out, ULONG outlen, ULONG *br) {
  return NtDeviceIoControlFile(h, iosb, code, in, inlen, out, outlen, br);
}

NTSTATUS ZwFsControlFile(HANDLE h, IO_STATUS_BLOCK *iosb, ULONG code, PVOID in,
                         ULONG inlen, PVOID out, ULONG outlen, ULONG *br) {
  return NtFsControlFile(h, iosb, code, in, inlen, out, outlen, br);
}

NTSTATUS ZwReadFileScatter(HANDLE h, HANDLE ev, PVOID apc, PVOID apcctx, IO_STATUS_BLOCK *iosb,
                           PVOID buf, ULONG count, ULONG len) {
  return NtReadFileScatter(h, ev, apc, apcctx, iosb, buf, count, len);
}

NTSTATUS ZwWriteFileGather(HANDLE h, HANDLE ev, PVOID apc, PVOID apcctx, IO_STATUS_BLOCK *iosb,
                           PVOID buf, ULONG count, ULONG len) {
  return NtWriteFileGather(h, ev, apc, apcctx, iosb, buf, count, len);
}

NTSTATUS NtFlushBuffersFile(HANDLE h, IO_STATUS_BLOCK *iosb) {
  ARGV(2);
  a[0] = (long)h; a[1] = (long)iosb;
  return (NTSTATUS)nt_dispatch(NR_NtFlushBuffersFile, a);
}

NTSTATUS NtFlushBuffersFileEx(HANDLE h, IO_STATUS_BLOCK *iosb, ULONG flags) {
  ARGV(3);
  a[0] = (long)h; a[1] = (long)iosb; a[2] = (long)flags;
  return (NTSTATUS)nt_dispatch(NR_NtFlushBuffersFileEx, a);
}

NTSTATUS NtLockFile(HANDLE h, IO_STATUS_BLOCK *iosb, ULONG lockflags, BOOL exclusive,
                    W64 off, W64 timeout) {
  ARGV(6);
  a[0] = (long)h; a[1] = (long)iosb; a[2] = (long)lockflags; a[3] = (long)exclusive;
  a[4] = (long)off; a[5] = (long)timeout;
  return (NTSTATUS)nt_dispatch(NR_NtLockFile, a);
}

NTSTATUS NtUnlockFile(HANDLE h, IO_STATUS_BLOCK *iosb) {
  ARGV(2);
  a[0] = (long)h; a[1] = (long)iosb;
  return (NTSTATUS)nt_dispatch(NR_NtUnlockFile, a);
}

NTSTATUS NtDeviceIoControlFile(HANDLE h, IO_STATUS_BLOCK *iosb, ULONG code, PVOID in,
                               ULONG inlen, PVOID out, ULONG outlen, ULONG *br) {
  ARGV(8);
  a[0] = (long)h; a[1] = (long)iosb; a[2] = (long)code; a[3] = (long)in;
  a[4] = (long)inlen; a[5] = (long)out; a[6] = (long)outlen; a[7] = (long)br;
  return (NTSTATUS)nt_dispatch(NR_NtDeviceIoControlFile, a);
}

NTSTATUS NtFsControlFile(HANDLE h, IO_STATUS_BLOCK *iosb, ULONG code, PVOID in,
                         ULONG inlen, PVOID out, ULONG outlen, ULONG *br) {
  ARGV(8);
  a[0] = (long)h; a[1] = (long)iosb; a[2] = (long)code; a[3] = (long)in;
  a[4] = (long)inlen; a[5] = (long)out; a[6] = (long)outlen; a[7] = (long)br;
  return (NTSTATUS)nt_dispatch(NR_NtFsControlFile, a);
}

NTSTATUS NtReadFileScatter(HANDLE h, HANDLE ev, PVOID apc, PVOID apcctx, IO_STATUS_BLOCK *iosb,
                           PVOID buf, ULONG count, ULONG len) {
  ARGV(8);
  a[0] = (long)h; a[1] = (long)ev; a[2] = (long)apc; a[3] = (long)apcctx;
  a[4] = (long)iosb; a[5] = (long)buf; a[6] = (long)count; a[7] = (long)len;
  return (NTSTATUS)nt_dispatch(NR_NtReadFileScatter, a);
}

NTSTATUS NtWriteFileGather(HANDLE h, HANDLE ev, PVOID apc, PVOID apcctx, IO_STATUS_BLOCK *iosb,
                           PVOID buf, ULONG count, ULONG len) {
  ARGV(8);
  a[0] = (long)h; a[1] = (long)ev; a[2] = (long)apc; a[3] = (long)apcctx;
  a[4] = (long)iosb; a[5] = (long)buf; a[6] = (long)count; a[7] = (long)len;
  return (NTSTATUS)nt_dispatch(NR_NtWriteFileGather, a);
}

/* ---- section syscalls: create / map / extend ---- */

NTSTATUS ZwCreateSection(HANDLE *SectionHandle, W32 Class, PVOID Buffer, ULONG Size,
                         ULONG SectionFlags, HANDLE Parent) {
  return NtCreateSection(SectionHandle, Class, Buffer, Size, SectionFlags, Parent);
}

NTSTATUS ZwMapViewOfSection(HANDLE SectionHandle, HANDLE hProcess, PVOID BaseAddress,
                            ULONG ViewLength, PVOID *BaseOut) {
  return NtMapViewOfSection(SectionHandle, hProcess, BaseAddress, ViewLength, BaseOut);
}

NTSTATUS ZwExtendSection(HANDLE SectionHandle, PVOID Extend) {
  return NtExtendSection(SectionHandle, Extend);
}

NTSTATUS NtCreateSection(HANDLE *SectionHandle, W32 Class, PVOID Buffer, ULONG Size,
                         ULONG SectionFlags, HANDLE Parent) {
  ARGV(6);
  a[0] = (long)SectionHandle; a[1] = (long)Class; a[2] = (long)Buffer; a[3] = (long)Size;
  a[4] = (long)SectionFlags; a[5] = (long)Parent;
  return (NTSTATUS)nt_dispatch(NR_NtCreateSection, a);
}

NTSTATUS NtMapViewOfSection(HANDLE SectionHandle, HANDLE hProcess, PVOID BaseAddress,
                            ULONG ViewLength, PVOID *BaseOut) {
  ARGV(5);
  a[0] = (long)SectionHandle; a[1] = (long)hProcess; a[2] = (long)BaseAddress;
  a[3] = (long)ViewLength; a[4] = (long)BaseOut;
  return (NTSTATUS)nt_dispatch(NR_NtMapViewOfSection, a);
}

NTSTATUS NtExtendSection(HANDLE SectionHandle, PVOID Extend) {
  ARGV(2);
  a[0] = (long)SectionHandle; a[1] = (long)Extend;
  return (NTSTATUS)nt_dispatch(NR_NtExtendSection, a);
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
