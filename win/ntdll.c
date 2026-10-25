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
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>

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
  NR_NtCloseTimer = 0x1f0, NR_NtWriteVirtualMemoryEx = 0x1f1, NR_NtCloseKey = 0x1f2,
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

/* ---------------- system syscalls: clock / timer / info queries ------- */

static long h_NtSetSystemTime(const long *a) {
  W64 *sys = (W64 *)a[0];
  W64 *tzb = (W64 *)a[1];
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  W64 now = (W64)ts.tv_sec * 10000000LL + (W64)ts.tv_nsec / 100;
  if (sys) *sys = now;
  if (tzb) *tzb = 0;
  return STATUS_SUCCESS;
}

static long h_NtQueryTimerResolution(const long *a) {
  W32 *out = (W32 *)a[0];
  if (out) *out = 10000;
  return STATUS_SUCCESS;
}

static long h_NtSetTimerResolution(const long *a) {
  W32 desired = (W32)a[0];
  W32 setres = (W32)a[1];
  W32 *newres = (W32 *)a[2];
  (void)setres;
  if (newres) *newres = desired ? desired : 10000;
  return STATUS_SUCCESS;
}

static long h_NtQuerySystemInformation(const long *a) {
  W32 cls = (W32)a[0];
  PVOID buffer = (PVOID)a[1];
  W32 len = (W32)a[2];
  W32 *retlen = (W32 *)a[3];
  (void)cls; (void)buffer; (void)len;
  if (retlen) *retlen = 0;
  return STATUS_SUCCESS;
}

/* ---------------- registry syscalls: in-memory key store ------------------ */

static long h_NtCreateKey(const long *a) {
  HANDLE *out = (HANDLE *)a[0];
  OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a[1];
  W32 access = (W32)a[2];
  (void)access;
  if (!out) return STATUS_INVALID_HANDLE;
  W32Handle *h;
  if (oa && oa->ObjectName && oa->ObjectName->Buffer) {
    char u8[1024];
    w16_to_utf8(oa->ObjectName->Buffer, oa->ObjectName->Length / 2, u8, sizeof u8);
    if (nt_obj_intern(u8, HK_KEY, 0, &h) < 0) return STATUS_NO_MEMORY;
  } else {
    h = w32_handle_new(HK_KEY, -1);
    if (!h) return STATUS_NO_MEMORY;
    h->kind = HK_KEY; h->fd = -1; h->ptr = calloc(1, sizeof(long));
    if (!h->ptr) { free(h); return STATUS_NO_MEMORY; }
  }
  *out = h;
  return STATUS_SUCCESS;
}

static long h_NtOpenKey(const long *a) {
  HANDLE *out = (HANDLE *)a[0];
  W32 access = (W32)a[1];
  OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a[2];
  (void)access;
  if (!out || !oa || !oa->ObjectName || !oa->ObjectName->Buffer) return STATUS_INVALID_HANDLE;
  char u8[1024];
  w16_to_utf8(oa->ObjectName->Buffer, oa->ObjectName->Length / 2, u8, sizeof u8);
  int i = nt_obj_find(u8, HK_KEY);
  if (i < 0) return STATUS_OBJECT_NAME_NOT_FOUND;
  *out = nt_reg[i].h;
  return STATUS_SUCCESS;
}

static long h_NtQueryKey(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  W32 cls = (W32)a[1];
  PVOID buffer = (PVOID)a[2];
  W32 len = (W32)a[3];
  W32 *retlen = (W32 *)a[4];
  (void)cls; (void)buffer; (void)len;
  if (!h || h->kind != HK_KEY) return STATUS_INVALID_HANDLE;
  if (retlen) *retlen = 0;
  return STATUS_SUCCESS;
}

static long h_NtCloseKey(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  if (!h || h->kind != HK_KEY) return STATUS_INVALID_HANDLE;
  int i = -1;
  for (int k = 0; k < nt_nreg; k++)
    if (nt_reg[k].h == h) { i = k; break; }
  if (i >= 0) { nt_mem_unregister(h); free(h->ptr); h->ptr = NULL; h->magic = 0; free(h); nt_reg[i].h = NULL; }
  else { h->ptr = NULL; h->magic = 0; free(h); }
  return STATUS_SUCCESS;
}

/* ---------------- process / thread syscalls: create / open / terminate --- */

typedef struct { void *arg; long (*fn)(const long *); const long *a; } NtThreadCtx;
static NtThreadCtx g_nt_thread_ctx;
static void *nt_thread_trampoline(void *p) {
  NtThreadCtx *c = (NtThreadCtx *)p;
  long r = c->fn(c->a);
  return NULL;
}

static long h_NtCreateThread(const long *a) {
  HANDLE *out = (HANDLE *)a[0];
  void *arg = (void *)a[1];
  long (*fn)(const long *) = (long (*)(const long *))a[2];
  if (!out) return STATUS_INVALID_HANDLE;
  g_nt_thread_ctx.arg = arg; g_nt_thread_ctx.fn = fn; g_nt_thread_ctx.a = a;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t t;
  int rc = pthread_create(&t, &attr, nt_thread_trampoline, &g_nt_thread_ctx);
  pthread_attr_destroy(&attr);
  if (rc != 0) return STATUS_NO_MEMORY;
  W32Handle *h = w32_handle_new(HK_THREAD, -1);
  if (!h) return STATUS_NO_MEMORY;
  h->kind = HK_THREAD; h->fd = -1; h->ptr = NULL; h->size = 0;
  *out = h;
  return STATUS_SUCCESS;
}

static int nt_ext_is(const char *s, const char *ext) {
  if (!s) return 0;
  size_t l = strlen(s), e = strlen(ext);
  if (l < e) return 0;
  for (size_t i = 0; i < e; i++) {
    char c = s[l - e + i], d = ext[i];
    if (c >= 'A' && c <= 'Z') c += 32;
    if (d >= 'A' && d <= 'Z') d += 32;
    if (c != d) return 0;
  }
  return 1;
}

static long h_NtCreateProcess(const long *a) {
  HANDLE *out = (HANDLE *)a[0];
  const char *img = (const char *)a[1];
  if (!out || !img) return STATUS_INVALID_HANDLE;
  int is_exe = nt_ext_is(img, ".exe") || nt_ext_is(img, ".com");
  int is_bat = nt_ext_is(img, ".bat") || nt_ext_is(img, ".cmd") || nt_ext_is(img, ".btm");
  pid_t pid = fork();
  if (pid < 0) return STATUS_NO_MEMORY;
  if (pid == 0) { /* child: exec per file type */
    if (is_exe) { execl(img, "ntcreateprocess", (char *)NULL); }
    else { execl("/bin/sh", "/bin/sh", img, (char *)NULL); }
    _exit(127);
  }
  if (!is_exe && !is_bat) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); return STATUS_INVALID_PARAMETER; }
  W32Handle *h = w32_handle_new(HK_PROC, -1);
  if (!h) return STATUS_NO_MEMORY;
  h->kind = HK_PROC; h->fd = -1; h->ptr = NULL; h->size = 0;
  *out = h;
  return STATUS_SUCCESS;
}

static long h_NtOpenProcess(const long *a) {
  HANDLE *out = (HANDLE *)a[0];
  W32 access = (W32)a[1];
  OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a[2];
  (void)oa; (void)access;
  if (!out) return STATUS_INVALID_HANDLE;
  W32Handle *h = w32_handle_new(HK_PROC, -1);
  if (!h) return STATUS_NO_MEMORY;
  h->kind = HK_PROC; h->fd = -1; h->ptr = NULL; h->size = 0;
  *out = h;
  return STATUS_SUCCESS;
}

static long h_NtOpenThread(const long *a) {
  HANDLE *out = (HANDLE *)a[0];
  W32 access = (W32)a[1];
  OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a[2];
  (void)oa; (void)access;
  if (!out) return STATUS_INVALID_HANDLE;
  W32Handle *h = w32_handle_new(HK_THREAD, -1);
  if (!h) return STATUS_NO_MEMORY;
  h->kind = HK_THREAD; h->fd = -1; h->ptr = NULL; h->size = 0;
  *out = h;
  return STATUS_SUCCESS;
}

static long h_NtTerminateProcess(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  W32 code = (W32)a[1];
  if (!h) return STATUS_INVALID_HANDLE;
  if (h->kind == HK_PROC) exit((int)code);
  return STATUS_SUCCESS;
}

static long h_NtTerminateThread(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  if (!h) return STATUS_INVALID_HANDLE;
  if (h->kind == HK_THREAD) { h->ptr = NULL; h->magic = 0; free(h); }
  return STATUS_SUCCESS;
}

static long h_NtQueryInformationProcess(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  W32 cls = (W32)a[1];
  PVOID buf = (PVOID)a[2];
  W32 len = (W32)a[3];
  W32 *retlen = (W32 *)a[4];
  (void)cls;
  if (!h) return STATUS_INVALID_HANDLE;
  if (retlen) *retlen = 0;
  if (buf && len > 0) { W64 *p = (W64 *)buf; p[0] = 0; }
  return STATUS_SUCCESS;
}

static long h_NtQueryInformationThread(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  W32 cls = (W32)a[1];
  PVOID buf = (PVOID)a[2];
  W32 len = (W32)a[3];
  W32 *retlen = (W32 *)a[4];
  (void)cls;
  if (!h) return STATUS_INVALID_HANDLE;
  if (retlen) *retlen = 0;
  if (buf && len > 0) { W64 *p = (W64 *)buf; p[0] = 0; }
  return STATUS_SUCCESS;
}

static long h_NtCreateProcessEx(const long *a) { return h_NtCreateProcess(a); }
static long h_NtCreateThreadEx(const long *a) { return h_NtCreateThread(a); }

/* ---------------- virtual memory info / section-ex syscalls ------------- */

static long h_NtQueryVirtualMemory(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  W32 kind = (W32)a[1];
  PVOID base = (PVOID)a[2];
  W32 len = (W32)a[3];
  W32 *retlen = (W32 *)a[4];
  (void)kind;
  if (!h) { if (retlen) *retlen = 0; return STATUS_INVALID_HANDLE; }
  W32Handle *mh = nt_mem_find(base);
  size_t avail = 0;
  if (mh) {
    size_t off = (size_t)((char *)base - (char *)mh->ptr);
    if (off < (size_t)mh->size) avail = (size_t)mh->size - off;
  }
  if (retlen) *retlen = (W32)((size_t)len < avail ? (size_t)len : avail);
  return STATUS_SUCCESS;
}

static long h_NtUnmapViewOfSectionEx(const long *a) {
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

static long h_NtMapViewOfSectionEx(const long *a) { return h_NtMapViewOfSection(a); }
static long h_NtCreateSectionEx(const long *a) { return h_NtCreateSection(a); }

/* ---------------- event / semaphore state syscalls ---------------------- */

static long h_NtQueryEvent(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  W32 *outState = (W32 *)a[1];
  if (!h || h->kind != HK_EVENT) { if (outState) *outState = 0; return STATUS_INVALID_HANDLE; }
  if (outState) *outState = (W32)*(long *)h->ptr;
  return STATUS_SUCCESS;
}

static long h_NtQuerySemaphore(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  W32 *outCount = (W32 *)a[1];
  if (!h || h->kind != HK_SEM) { if (outCount) *outCount = 0; return STATUS_INVALID_HANDLE; }
  if (outCount) *outCount = (W32)*(long *)h->ptr;
  return STATUS_SUCCESS;
}

static long h_NtClearEvent(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  if (!h || h->kind != HK_EVENT) return STATUS_INVALID_HANDLE;
  *(long *)h->ptr = 0;
  return STATUS_SUCCESS;
}

static long h_NtPulseEvent(const long *a) {
  W32Handle *h = w32_handle((HANDLE)a[0]);
  if (!h || h->kind != HK_EVENT) return STATUS_INVALID_HANDLE;
  if (*(long *)h->ptr != 0) *(long *)h->ptr = 0; /* one-shot pulse */
  return STATUS_SUCCESS;
}

/* ---------------- performance / cpu syscalls ----------------------------- */

static long h_NtQueryPerformanceCounter(const long *a) {
  W64 *Frequency = (W64 *)a[0];
  W64 *Counter = (W64 *)a[1];
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  if (Frequency) *Frequency = 10000000; /* 100ns units */
  if (Counter) *Counter = (W64)ts.tv_sec * 10000000LL + (W64)ts.tv_nsec / 100;
  return STATUS_SUCCESS;
}

static long h_NtGetCurrentProcessorNumber(const long *a) {
  W32 *Number = (W32 *)a[0];
  if (Number) *Number = (W32)sysconf(_SC_NPROCESSORS_ONLN);
  return STATUS_SUCCESS;
}

static long h_NtGetCurrentProcessorNumberEx(const long *a) {
  W32 Info = (W32)a[0];
  W32 *Number = (W32 *)a[1];
  if (Number) *Number = (W32)sysconf(_SC_NPROCESSORS_ONLN);
  (void)Info;
  return STATUS_SUCCESS;
}

/* ---------------- batch M: power / utility / object-state syscalls ------- */

static long h_NtYieldExecution(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtFlushInstructionCache(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtFlushProcessWriteBuffers(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtGetDevicePowerState(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtGetWriteWatch(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtGetCachedSigningLevel(const long *a) { W32 *level = (W32 *)a[0]; if (level) *level = 0; return (long)STATUS_SUCCESS; }
static long h_NtGetNlsSectionPtr(const long *a) { W32 sec = (W32)a[0]; PVOID *p = (PVOID *)a[1]; (void)sec; if (p) *p = (PVOID)0; return (long)STATUS_SUCCESS; }
static long h_NtIsProcessInJob(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtIsSystemResumeAutomatic(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtIsUILanguageComitted(const long *a) { W32 *c = (W32 *)a[0]; if (c) *c = 0; return (long)STATUS_SUCCESS; }
static long h_NtInitiatePowerAction(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtLoadDriver(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtLockProductActivationKeys(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtLockRegistryKey(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtMakeTemporaryObject(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtMakePermanentObject(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtManageHotPatch(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtManagePartition(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtMapUserPhysicalPages(const long *a) {
  PVOID *addr = (PVOID *)a[1];
  W64 sz = (W64)a[2];
  void *p = mmap((void *)0, (size_t)(sz ? (size_t)sz : 4096), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return (long)STATUS_NO_MEMORY;
  if (addr) *addr = p;
  return (long)STATUS_SUCCESS;
}
static long h_NtMapUserPhysicalPagesScatter(const long *a) { return h_NtMapUserPhysicalPages(a); }
static long h_NtModifyBootEntry(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtModifyDriverEntry(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtNotifyChangeSession(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtPowerInformation(const long *a) { (void)a; return (long)STATUS_SUCCESS; }

static long h_NtAddBootEntry(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAddDriverEntry(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtDeleteBootEntry(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtDeleteDriverEntry(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtEnumerateBootEntries(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtEnumerateDriverEntries(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryBootEntryOrder(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetBootEntryOrder(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryBootOptions(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetBootOptions(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtDisableLastKnownGood(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtEnableLastKnownGood(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryIntervalProfile(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetIntervalProfile(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSerializeBoot(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtTraceControl(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtShutdownSystem(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSinglePhaseReject(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtPrePrepareComplete(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtPrePrepareEnlistment(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtPrepareComplete(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtPrepareEnlistment(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtPropagationComplete(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtPropagationFailed(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRollbackComplete(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRollbackEnlistment(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRollbackRegistryTransaction(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRollbackTransaction(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRollforwardTransactionManager(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRecoverTransactionManager(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRenameTransactionManager(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCommitComplete(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCommitEnlistment(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCommitRegistryTransaction(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCommitTransaction(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRecoverEnlistment(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRecoverResourceManager(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtShutdownWorkerFactory(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateWorkerFactory(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryInformationWorkerFactory(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetInformationWorkerFactory(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtReleaseWorkerFactoryWorker(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtWorkerFactoryWorkerReady(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateWaitCompletionPacket(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCancelWaitCompletionPacket(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAssociateWaitCompletionPacket(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateWaitablePort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreatePort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtConnectPort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAcceptConnectPort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryInformationPort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtListenPort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRequestPort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtReplyPort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtReplyWaitReceivePort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtReplyWaitReceivePortEx(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtReplyWaitReplyPort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSecureConnectPort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCompleteConnectPort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
/* ---------------- dispatch table (numbers from windows-syscalls) -------- */
/* ---------------- wait syscalls: multiple objects ---------------- */

static long h_NtWaitForMultipleObjects(const long *a) {
  const long *handles = (const long *)a[0];
  W32 count = (W32)a[1];
  W32 waitall = (W32)a[2];
  W64S *timeout = (W64S *)a[3];
  if (!handles || count == 0 || count > NT_MAX_OBJS) return STATUS_INVALID_PARAMETER;
  for (;;) {
    W32 satisfied = 0;
    int all = 1;
    for (W32 i = 0; i < count; i++) {
      W32Handle *h = w32_handle((HANDLE)handles[i]);
      int ok = 0;
      if (h) {
        if (h->kind == HK_EVENT) ok = *(long *)h->ptr != 0;
        else if (h->kind == HK_SEM) { long s = *(long *)h->ptr; if (s > 0) *(long *)h->ptr = s - 1; ok = s > 0; }
        else if (h->kind == HK_MUTEX) { ok = *(long *)h->ptr == 0; if (ok) *(long *)h->ptr = 1; }
        else if (h->kind == HK_TIMER) {
          long due = *(long *)h->ptr;
          if (due > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            long long now = (long long)ts.tv_sec * 10000000LL + (long long)ts.tv_nsec / 100;
            if (now >= due) { *(long *)h->ptr = 0; ok = 1; }
          } else ok = 1;
        }
      }
      if (ok) satisfied++;
      else { all = 0; if (!waitall) break; }
    }
    if (all || (satisfied > 0 && !waitall)) return STATUS_SUCCESS;
    if (timeout && *timeout > 0) {
      struct timespec ts;
      W64S t = (W64S)*timeout;
      ts.tv_sec = (time_t)(t / 10000000); ts.tv_nsec = (long)((t % 10000000) * 100);
      while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    }
    return STATUS_PENDING;
  }
}

static long h_NtWaitForMultipleObjects32(const long *a) {
  const long *handles = (const long *)a[0];
  W32 count = (W32)a[1];
  W32 waitall = (W32)a[2];
  W64S *timeout = (W64S *)a[3];
  if (!handles || count == 0 || count > NT_MAX_OBJS) return STATUS_INVALID_PARAMETER;
  for (;;) {
    W32 satisfied = 0;
    int all = 1;
    for (W32 i = 0; i < count; i++) {
      W32Handle *h = w32_handle((HANDLE)(W64)handles[i]);
      int ok = 0;
      if (h) {
        if (h->kind == HK_EVENT) ok = *(long *)h->ptr != 0;
        else if (h->kind == HK_SEM) { long s = *(long *)h->ptr; if (s > 0) *(long *)h->ptr = s - 1; ok = s > 0; }
        else if (h->kind == HK_MUTEX) { ok = *(long *)h->ptr == 0; if (ok) *(long *)h->ptr = 1; }
        else if (h->kind == HK_TIMER) {
          long due = *(long *)h->ptr;
          if (due > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            long long now = (long long)ts.tv_sec * 10000000LL + (long long)ts.tv_nsec / 100;
            if (now >= due) { *(long *)h->ptr = 0; ok = 1; }
          } else ok = 1;
        }
      }
      if (ok) satisfied++;
      else { all = 0; if (!waitall) break; }
    }
    if (all || (satisfied > 0 && !waitall)) return STATUS_SUCCESS;
    if (timeout && *timeout > 0) {
      struct timespec ts;
      W64S t = (W64S)*timeout;
      ts.tv_sec = (time_t)(t / 10000000); ts.tv_nsec = (long)((t % 10000000) * 100);
      while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    }
    return STATUS_PENDING;
  }
}

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
  nt_tab[NR_NtCloseKey] = h_NtCloseKey;
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

/* ---- system syscalls: clock / timer / info queries ---- */

NTSTATUS ZwSetSystemTime(W64 *SystemTime, W64 *TimeZoneBias) {
  return NtSetSystemTime(SystemTime, TimeZoneBias);
}

NTSTATUS NtSetSystemTime(W64 *SystemTime, W64 *TimeZoneBias) {
  ARGV(2);
  a[0] = (long)SystemTime; a[1] = (long)TimeZoneBias;
  return (NTSTATUS)nt_dispatch(NR_NtSetSystemTime, a);
}

NTSTATUS ZwQueryTimerResolution(W32 *TimerResolution) {
  return NtQueryTimerResolution(TimerResolution);
}

NTSTATUS NtQueryTimerResolution(W32 *TimerResolution) {
  ARGV(1);
  a[0] = (long)TimerResolution;
  return (NTSTATUS)nt_dispatch(NR_NtQueryTimerResolution, a);
}

NTSTATUS ZwSetTimerResolution(W32 DesiredResolution, BOOL SetResolution, W32 *NewResolution) {
  return NtSetTimerResolution(DesiredResolution, SetResolution, NewResolution);
}

NTSTATUS NtSetTimerResolution(W32 DesiredResolution, BOOL SetResolution, W32 *NewResolution) {
  ARGV(3);
  a[0] = (long)DesiredResolution; a[1] = (long)SetResolution; a[2] = (long)NewResolution;
  return (NTSTATUS)nt_dispatch(NR_NtSetTimerResolution, a);
}

NTSTATUS ZwQuerySystemInformation(W32 InfoClass, PVOID Buffer, W32 BufferLen, W32 *ReturnLength) {
  return NtQuerySystemInformation(InfoClass, Buffer, BufferLen, ReturnLength);
}

NTSTATUS NtQuerySystemInformation(W32 InfoClass, PVOID Buffer, W32 BufferLen, W32 *ReturnLength) {
  ARGV(4);
  a[0] = (long)InfoClass; a[1] = (long)Buffer; a[2] = (long)BufferLen; a[3] = (long)ReturnLength;
  return (NTSTATUS)nt_dispatch(NR_NtQuerySystemInformation, a);
}

/* ---- wait syscalls: multiple objects ---- */

NTSTATUS ZwWaitForMultipleObjects(const HANDLE *Handles, W32 Count, BOOL WaitAll,
                                 W64S *Timeout) {
  return NtWaitForMultipleObjects(Handles, Count, WaitAll, Timeout);
}

NTSTATUS NtWaitForMultipleObjects(const HANDLE *Handles, W32 Count, BOOL WaitAll,
                                 W64S *Timeout) {
  ARGV(4);
  a[0] = (long)Handles; a[1] = (long)Count; a[2] = (long)WaitAll; a[3] = (long)Timeout;
  return (NTSTATUS)nt_dispatch(NR_NtWaitForMultipleObjects, a);
}

NTSTATUS ZwWaitForMultipleObjects32(const HANDLE *Handles, W32 Count, BOOL WaitAll,
                                   W64S *Timeout) {
  return NtWaitForMultipleObjects32(Handles, Count, WaitAll, Timeout);
}

NTSTATUS NtWaitForMultipleObjects32(const HANDLE *Handles, W32 Count, BOOL WaitAll,
                                   W64S *Timeout) {
  ARGV(4);
  a[0] = (long)Handles; a[1] = (long)Count; a[2] = (long)WaitAll; a[3] = (long)Timeout;
  return (NTSTATUS)nt_dispatch(NR_NtWaitForMultipleObjects32, a);
}

/* ---- process / thread syscalls: create / open / terminate ---- */

NTSTATUS ZwCreateProcess(HANDLE *ProcessHandle, PVOID StartInfo) {
  return NtCreateProcess(ProcessHandle, StartInfo);
}

NTSTATUS NtCreateProcess(HANDLE *ProcessHandle, PVOID StartInfo) {
  ARGV(2);
  a[0] = (long)ProcessHandle; a[1] = (long)StartInfo;
  return (NTSTATUS)nt_dispatch(NR_NtCreateProcess, a);
}

NTSTATUS ZwCreateProcessEx(HANDLE *ProcessHandle, PVOID StartInfo) {
  return NtCreateProcessEx(ProcessHandle, StartInfo);
}

NTSTATUS NtCreateProcessEx(HANDLE *ProcessHandle, PVOID StartInfo) {
  ARGV(2);
  a[0] = (long)ProcessHandle; a[1] = (long)StartInfo;
  return (NTSTATUS)nt_dispatch(NR_NtCreateProcessEx, a);
}

NTSTATUS ZwCreateThread(HANDLE *ThreadHandle, void *Arg, long (*Start)(const long *)) {
  return NtCreateThread(ThreadHandle, Arg, Start);
}

NTSTATUS NtCreateThread(HANDLE *ThreadHandle, void *Arg, long (*Start)(const long *)) {
  ARGV(3);
  a[0] = (long)ThreadHandle; a[1] = (long)Arg; a[2] = (long)Start;
  return (NTSTATUS)nt_dispatch(NR_NtCreateThread, a);
}

NTSTATUS ZwCreateThreadEx(HANDLE *ThreadHandle, void *Arg, long (*Start)(const long *)) {
  return NtCreateThreadEx(ThreadHandle, Arg, Start);
}

NTSTATUS NtCreateThreadEx(HANDLE *ThreadHandle, void *Arg, long (*Start)(const long *)) {
  ARGV(3);
  a[0] = (long)ThreadHandle; a[1] = (long)Arg; a[2] = (long)Start;
  return (NTSTATUS)nt_dispatch(NR_NtCreateThreadEx, a);
}

NTSTATUS ZwOpenProcess(HANDLE *ProcessHandle, W32 Access, OBJECT_ATTRIBUTES *ObjectAttributes) {
  return NtOpenProcess(ProcessHandle, Access, ObjectAttributes);
}

NTSTATUS NtOpenProcess(HANDLE *ProcessHandle, W32 Access, OBJECT_ATTRIBUTES *ObjectAttributes) {
  ARGV(3);
  a[0] = (long)ProcessHandle; a[1] = (long)Access; a[2] = (long)ObjectAttributes;
  return (NTSTATUS)nt_dispatch(NR_NtOpenProcess, a);
}

NTSTATUS ZwOpenThread(HANDLE *ThreadHandle, W32 Access, OBJECT_ATTRIBUTES *ObjectAttributes) {
  return NtOpenThread(ThreadHandle, Access, ObjectAttributes);
}

NTSTATUS NtOpenThread(HANDLE *ThreadHandle, W32 Access, OBJECT_ATTRIBUTES *ObjectAttributes) {
  ARGV(3);
  a[0] = (long)ThreadHandle; a[1] = (long)Access; a[2] = (long)ObjectAttributes;
  return (NTSTATUS)nt_dispatch(NR_NtOpenThread, a);
}

NTSTATUS ZwTerminateProcess(HANDLE ProcessHandle, W32 ExitStatus) {
  return NtTerminateProcess(ProcessHandle, ExitStatus);
}

NTSTATUS NtTerminateProcess(HANDLE ProcessHandle, W32 ExitStatus) {
  ARGV(2);
  a[0] = (long)ProcessHandle; a[1] = (long)ExitStatus;
  return (NTSTATUS)nt_dispatch(NR_NtTerminateProcess, a);
}

NTSTATUS ZwTerminateThread(HANDLE ThreadHandle, W32 ExitStatus) {
  return NtTerminateThread(ThreadHandle, ExitStatus);
}

NTSTATUS NtTerminateThread(HANDLE ThreadHandle, W32 ExitStatus) {
  ARGV(2);
  a[0] = (long)ThreadHandle; a[1] = (long)ExitStatus;
  return (NTSTATUS)nt_dispatch(NR_NtTerminateThread, a);
}

NTSTATUS ZwQueryInformationProcess(HANDLE ProcessHandle, W32 InfoClass, PVOID Buffer,
                                  W32 BufferLen, W32 *ReturnLength) {
  return NtQueryInformationProcess(ProcessHandle, InfoClass, Buffer, BufferLen, ReturnLength);
}

NTSTATUS NtQueryInformationProcess(HANDLE ProcessHandle, W32 InfoClass, PVOID Buffer,
                                  W32 BufferLen, W32 *ReturnLength) {
  ARGV(5);
  a[0] = (long)ProcessHandle; a[1] = (long)InfoClass; a[2] = (long)Buffer;
  a[3] = (long)BufferLen; a[4] = (long)ReturnLength;
  return (NTSTATUS)nt_dispatch(NR_NtQueryInformationProcess, a);
}

NTSTATUS ZwQueryInformationThread(HANDLE ThreadHandle, W32 InfoClass, PVOID Buffer,
                                 W32 BufferLen, W32 *ReturnLength) {
  return NtQueryInformationThread(ThreadHandle, InfoClass, Buffer, BufferLen, ReturnLength);
}

NTSTATUS NtQueryInformationThread(HANDLE ThreadHandle, W32 InfoClass, PVOID Buffer,
                                 W32 BufferLen, W32 *ReturnLength) {
  ARGV(5);
  a[0] = (long)ThreadHandle; a[1] = (long)InfoClass; a[2] = (long)Buffer;
  a[3] = (long)BufferLen; a[4] = (long)ReturnLength;
  return (NTSTATUS)nt_dispatch(NR_NtQueryInformationThread, a);
}

/* ---- virtual memory info / section-ex syscalls ---- */

NTSTATUS ZwQueryVirtualMemory(HANDLE h, W32 InfoClass, PVOID BaseAddress, W32 Length,
                              W32 *ReturnLength) {
  return NtQueryVirtualMemory(h, InfoClass, BaseAddress, Length, ReturnLength);
}

NTSTATUS NtQueryVirtualMemory(HANDLE h, W32 InfoClass, PVOID BaseAddress, W32 Length,
                              W32 *ReturnLength) {
  ARGV(5);
  a[0] = (long)h; a[1] = (long)InfoClass; a[2] = (long)BaseAddress; a[3] = (long)Length;
  a[4] = (long)ReturnLength;
  return (NTSTATUS)nt_dispatch(NR_NtQueryVirtualMemory, a);
}

NTSTATUS ZwMapViewOfSectionEx(HANDLE SectionHandle, HANDLE hProcess, PVOID BaseAddress,
                              ULONG ViewLength, PVOID *BaseOut) {
  return NtMapViewOfSectionEx(SectionHandle, hProcess, BaseAddress, ViewLength, BaseOut);
}

NTSTATUS NtMapViewOfSectionEx(HANDLE SectionHandle, HANDLE hProcess, PVOID BaseAddress,
                              ULONG ViewLength, PVOID *BaseOut) {
  ARGV(5);
  a[0] = (long)SectionHandle; a[1] = (long)hProcess; a[2] = (long)BaseAddress;
  a[3] = (long)ViewLength; a[4] = (long)BaseOut;
  return (NTSTATUS)nt_dispatch(NR_NtMapViewOfSectionEx, a);
}

NTSTATUS ZwUnmapViewOfSectionEx(HANDLE h, PVOID BaseAddress) {
  return NtUnmapViewOfSectionEx(h, BaseAddress);
}

NTSTATUS NtUnmapViewOfSectionEx(HANDLE h, PVOID BaseAddress) {
  ARGV(2);
  a[0] = (long)h; a[1] = (long)BaseAddress;
  return (NTSTATUS)nt_dispatch(NR_NtUnmapViewOfSectionEx, a);
}

NTSTATUS ZwCreateSectionEx(HANDLE *SectionHandle, W32 Class, PVOID Buffer, ULONG Size,
                           ULONG SectionFlags, HANDLE Parent) {
  return NtCreateSectionEx(SectionHandle, Class, Buffer, Size, SectionFlags, Parent);
}

NTSTATUS NtCreateSectionEx(HANDLE *SectionHandle, W32 Class, PVOID Buffer, ULONG Size,
                           ULONG SectionFlags, HANDLE Parent) {
  ARGV(6);
  a[0] = (long)SectionHandle; a[1] = (long)Class; a[2] = (long)Buffer; a[3] = (long)Size;
  a[4] = (long)SectionFlags; a[5] = (long)Parent;
  return (NTSTATUS)nt_dispatch(NR_NtCreateSectionEx, a);
}

/* ---- event / semaphore state syscalls ---- */

NTSTATUS ZwQueryEvent(HANDLE h, W32 *PreviousState) {
  return NtQueryEvent(h, PreviousState);
}

NTSTATUS NtQueryEvent(HANDLE h, W32 *PreviousState) {
  ARGV(2);
  a[0] = (long)h; a[1] = (long)PreviousState;
  return (NTSTATUS)nt_dispatch(NR_NtQueryEvent, a);
}

NTSTATUS ZwQuerySemaphore(HANDLE h, W32 *Count) {
  return NtQuerySemaphore(h, Count);
}

NTSTATUS NtQuerySemaphore(HANDLE h, W32 *Count) {
  ARGV(2);
  a[0] = (long)h; a[1] = (long)Count;
  return (NTSTATUS)nt_dispatch(NR_NtQuerySemaphore, a);
}

NTSTATUS ZwClearEvent(HANDLE h) {
  return NtClearEvent(h);
}

NTSTATUS NtClearEvent(HANDLE h) {
  ARGV(1);
  a[0] = (long)h;
  return (NTSTATUS)nt_dispatch(NR_NtClearEvent, a);
}

NTSTATUS ZwPulseEvent(HANDLE h) {
  return NtPulseEvent(h);
}

NTSTATUS NtPulseEvent(HANDLE h) {
  ARGV(1);
  a[0] = (long)h;
  return (NTSTATUS)nt_dispatch(NR_NtPulseEvent, a);
}

/* ---- performance / cpu syscalls ---- */

NTSTATUS ZwQueryPerformanceCounter(W64 *Frequency, W64 *Counter) {
  return NtQueryPerformanceCounter(Frequency, Counter);
}

NTSTATUS NtQueryPerformanceCounter(W64 *Frequency, W64 *Counter) {
  ARGV(2);
  a[0] = (long)Frequency; a[1] = (long)Counter;
  return (NTSTATUS)nt_dispatch(NR_NtQueryPerformanceCounter, a);
}

NTSTATUS ZwGetCurrentProcessorNumber(W32 *Number) {
  return NtGetCurrentProcessorNumber(Number);
}

NTSTATUS NtGetCurrentProcessorNumber(W32 *Number) {
  ARGV(1);
  a[0] = (long)Number;
  return (NTSTATUS)nt_dispatch(NR_NtGetCurrentProcessorNumber, a);
}

NTSTATUS ZwGetCurrentProcessorNumberEx(W32 Info, W32 *Number) {
  return NtGetCurrentProcessorNumberEx(Info, Number);
}

NTSTATUS NtGetCurrentProcessorNumberEx(W32 Info, W32 *Number) {
  ARGV(2);
  a[0] = (long)Info; a[1] = (long)Number;
  return (NTSTATUS)nt_dispatch(NR_NtGetCurrentProcessorNumberEx, a);
}

/* ---- batch M: forward prototypes (Zw blocks precede the Nt definitions) */
NTSTATUS NtYieldExecution(void);
NTSTATUS NtFlushInstructionCache(HANDLE ImageHandle);
NTSTATUS NtFlushProcessWriteBuffers(HANDLE Process);
NTSTATUS NtGetDevicePowerState(HANDLE Device, W32 *State);
NTSTATUS NtGetWriteWatch(W32 *WatchInfo);
NTSTATUS NtGetCachedSigningLevel(W32 *Level);
NTSTATUS NtGetNlsSectionPtr(W32 Section, PVOID *Pointer);
NTSTATUS NtIsProcessInJob(HANDLE ProcessHandle, HANDLE JobHandle);
NTSTATUS NtIsSystemResumeAutomatic(void);
NTSTATUS NtIsUILanguageComitted(W32 *Committed);
NTSTATUS NtInitiatePowerAction(void);
NTSTATUS NtLoadDriver(void);
NTSTATUS NtLockProductActivationKeys(W32 *Locked);
NTSTATUS NtLockRegistryKey(HANDLE Key);
NTSTATUS NtMakeTemporaryObject(HANDLE Object);
NTSTATUS NtMakePermanentObject(HANDLE Object);
NTSTATUS NtManageHotPatch(HANDLE HotPatchObject);
NTSTATUS NtManagePartition(HANDLE Partition);
NTSTATUS NtMapUserPhysicalPages(HANDLE Handle, PVOID *VirtualAddress, W64 Size);
NTSTATUS NtMapUserPhysicalPagesScatter(HANDLE Handle, PVOID *VirtualAddress, W64 Size);
NTSTATUS NtModifyBootEntry(HANDLE BootEntry);
NTSTATUS NtModifyDriverEntry(HANDLE DriverEntry);
NTSTATUS NtNotifyChangeSession(HANDLE Session);
NTSTATUS NtPowerInformation(W32 *Information);

W32API NTSTATUS ZwAddBootEntry();
W32API NTSTATUS NtAddBootEntry();
W32API NTSTATUS ZwAddDriverEntry();
W32API NTSTATUS NtAddDriverEntry();
W32API NTSTATUS ZwDeleteBootEntry();
W32API NTSTATUS NtDeleteBootEntry();
W32API NTSTATUS ZwDeleteDriverEntry();
W32API NTSTATUS NtDeleteDriverEntry();
W32API NTSTATUS ZwEnumerateBootEntries();
W32API NTSTATUS NtEnumerateBootEntries();
W32API NTSTATUS ZwEnumerateDriverEntries();
W32API NTSTATUS NtEnumerateDriverEntries();
W32API NTSTATUS ZwQueryBootEntryOrder();
W32API NTSTATUS NtQueryBootEntryOrder();
W32API NTSTATUS ZwSetBootEntryOrder();
W32API NTSTATUS NtSetBootEntryOrder();
W32API NTSTATUS ZwQueryBootOptions();
W32API NTSTATUS NtQueryBootOptions();
W32API NTSTATUS ZwSetBootOptions();
W32API NTSTATUS NtSetBootOptions();
W32API NTSTATUS ZwDisableLastKnownGood();
W32API NTSTATUS NtDisableLastKnownGood();
W32API NTSTATUS ZwEnableLastKnownGood();
W32API NTSTATUS NtEnableLastKnownGood();
W32API NTSTATUS ZwQueryIntervalProfile();
W32API NTSTATUS NtQueryIntervalProfile();
W32API NTSTATUS ZwSetIntervalProfile();
W32API NTSTATUS NtSetIntervalProfile();
W32API NTSTATUS ZwSerializeBoot();
W32API NTSTATUS NtSerializeBoot();
W32API NTSTATUS ZwTraceControl();
W32API NTSTATUS NtTraceControl();
W32API NTSTATUS ZwShutdownSystem();
W32API NTSTATUS NtShutdownSystem();
W32API NTSTATUS ZwSinglePhaseReject();
W32API NTSTATUS NtSinglePhaseReject();
W32API NTSTATUS ZwPrePrepareComplete();
W32API NTSTATUS NtPrePrepareComplete();
W32API NTSTATUS ZwPrePrepareEnlistment();
W32API NTSTATUS NtPrePrepareEnlistment();
W32API NTSTATUS ZwPrepareComplete();
W32API NTSTATUS NtPrepareComplete();
W32API NTSTATUS ZwPrepareEnlistment();
W32API NTSTATUS NtPrepareEnlistment();
W32API NTSTATUS ZwPropagationComplete();
W32API NTSTATUS NtPropagationComplete();
W32API NTSTATUS ZwPropagationFailed();
W32API NTSTATUS NtPropagationFailed();
W32API NTSTATUS ZwRollbackComplete();
W32API NTSTATUS NtRollbackComplete();
W32API NTSTATUS ZwRollbackEnlistment();
W32API NTSTATUS NtRollbackEnlistment();
W32API NTSTATUS ZwRollbackRegistryTransaction();
W32API NTSTATUS NtRollbackRegistryTransaction();
W32API NTSTATUS ZwRollbackTransaction();
W32API NTSTATUS NtRollbackTransaction();
W32API NTSTATUS ZwRollforwardTransactionManager();
W32API NTSTATUS NtRollforwardTransactionManager();
W32API NTSTATUS ZwRecoverTransactionManager();
W32API NTSTATUS NtRecoverTransactionManager();
W32API NTSTATUS ZwRenameTransactionManager();
W32API NTSTATUS NtRenameTransactionManager();
W32API NTSTATUS ZwCommitComplete();
W32API NTSTATUS NtCommitComplete();
W32API NTSTATUS ZwCommitEnlistment();
W32API NTSTATUS NtCommitEnlistment();
W32API NTSTATUS ZwCommitRegistryTransaction();
W32API NTSTATUS NtCommitRegistryTransaction();
W32API NTSTATUS ZwCommitTransaction();
W32API NTSTATUS NtCommitTransaction();
W32API NTSTATUS ZwRecoverEnlistment();
W32API NTSTATUS NtRecoverEnlistment();
W32API NTSTATUS ZwRecoverResourceManager();
W32API NTSTATUS NtRecoverResourceManager();
W32API NTSTATUS ZwShutdownWorkerFactory();
W32API NTSTATUS NtShutdownWorkerFactory();
W32API NTSTATUS ZwCreateWorkerFactory();
W32API NTSTATUS NtCreateWorkerFactory();
W32API NTSTATUS ZwQueryInformationWorkerFactory();
W32API NTSTATUS NtQueryInformationWorkerFactory();
W32API NTSTATUS ZwSetInformationWorkerFactory();
W32API NTSTATUS NtSetInformationWorkerFactory();
W32API NTSTATUS ZwReleaseWorkerFactoryWorker();
W32API NTSTATUS NtReleaseWorkerFactoryWorker();
W32API NTSTATUS ZwWorkerFactoryWorkerReady();
W32API NTSTATUS NtWorkerFactoryWorkerReady();
W32API NTSTATUS ZwCreateWaitCompletionPacket();
W32API NTSTATUS NtCreateWaitCompletionPacket();
W32API NTSTATUS ZwCancelWaitCompletionPacket();
W32API NTSTATUS NtCancelWaitCompletionPacket();
W32API NTSTATUS ZwAssociateWaitCompletionPacket();
W32API NTSTATUS NtAssociateWaitCompletionPacket();
W32API NTSTATUS ZwCreateWaitablePort();
W32API NTSTATUS NtCreateWaitablePort();
W32API NTSTATUS ZwCreatePort();
W32API NTSTATUS NtCreatePort();
W32API NTSTATUS ZwConnectPort();
W32API NTSTATUS NtConnectPort();
W32API NTSTATUS ZwAcceptConnectPort();
W32API NTSTATUS NtAcceptConnectPort();
W32API NTSTATUS ZwQueryInformationPort();
W32API NTSTATUS NtQueryInformationPort();
W32API NTSTATUS ZwSetInformationPort();
W32API NTSTATUS ZwListenPort();
W32API NTSTATUS NtListenPort();
W32API NTSTATUS ZwRequestPort();
W32API NTSTATUS NtRequestPort();
W32API NTSTATUS ZwReplyPort();
W32API NTSTATUS NtReplyPort();
W32API NTSTATUS ZwReplyWaitReceivePort();
W32API NTSTATUS NtReplyWaitReceivePort();
W32API NTSTATUS ZwReplyWaitReceivePortEx();
W32API NTSTATUS NtReplyWaitReceivePortEx();
W32API NTSTATUS ZwReplyWaitReplyPort();
W32API NTSTATUS NtReplyWaitReplyPort();
W32API NTSTATUS ZwSendWaitReceivePort();
W32API NTSTATUS ZwSecureConnectPort();
W32API NTSTATUS NtSecureConnectPort();
W32API NTSTATUS ZwCompleteConnectPort();
W32API NTSTATUS NtCompleteConnectPort();
/* ---- batch N: boot / power-transaction / port / worker no-ops ---- */

NTSTATUS ZwAddBootEntry() { return NtAddBootEntry(); }
NTSTATUS NtAddBootEntry() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAddBootEntry, a); }
NTSTATUS ZwAddDriverEntry() { return NtAddDriverEntry(); }
NTSTATUS NtAddDriverEntry() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAddDriverEntry, a); }
NTSTATUS ZwDeleteBootEntry() { return NtDeleteBootEntry(); }
NTSTATUS NtDeleteBootEntry() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDeleteBootEntry, a); }
NTSTATUS ZwDeleteDriverEntry() { return NtDeleteDriverEntry(); }
NTSTATUS NtDeleteDriverEntry() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDeleteDriverEntry, a); }
NTSTATUS ZwEnumerateBootEntries() { return NtEnumerateBootEntries(); }
NTSTATUS NtEnumerateBootEntries() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtEnumerateBootEntries, a); }
NTSTATUS ZwEnumerateDriverEntries() { return NtEnumerateDriverEntries(); }
NTSTATUS NtEnumerateDriverEntries() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtEnumerateDriverEntries, a); }
NTSTATUS ZwQueryBootEntryOrder() { return NtQueryBootEntryOrder(); }
NTSTATUS NtQueryBootEntryOrder() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryBootEntryOrder, a); }
NTSTATUS ZwSetBootEntryOrder() { return NtSetBootEntryOrder(); }
NTSTATUS NtSetBootEntryOrder() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetBootEntryOrder, a); }
NTSTATUS ZwQueryBootOptions() { return NtQueryBootOptions(); }
NTSTATUS NtQueryBootOptions() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryBootOptions, a); }
NTSTATUS ZwSetBootOptions() { return NtSetBootOptions(); }
NTSTATUS NtSetBootOptions() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetBootOptions, a); }
NTSTATUS ZwDisableLastKnownGood() { return NtDisableLastKnownGood(); }
NTSTATUS NtDisableLastKnownGood() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDisableLastKnownGood, a); }
NTSTATUS ZwEnableLastKnownGood() { return NtEnableLastKnownGood(); }
NTSTATUS NtEnableLastKnownGood() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtEnableLastKnownGood, a); }
NTSTATUS ZwQueryIntervalProfile() { return NtQueryIntervalProfile(); }
NTSTATUS NtQueryIntervalProfile() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryIntervalProfile, a); }
NTSTATUS ZwSetIntervalProfile() { return NtSetIntervalProfile(); }
NTSTATUS NtSetIntervalProfile() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetIntervalProfile, a); }
NTSTATUS ZwSerializeBoot() { return NtSerializeBoot(); }
NTSTATUS NtSerializeBoot() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSerializeBoot, a); }
NTSTATUS ZwTraceControl() { return NtTraceControl(); }
NTSTATUS NtTraceControl() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtTraceControl, a); }
NTSTATUS ZwShutdownSystem() { return NtShutdownSystem(); }
NTSTATUS NtShutdownSystem() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtShutdownSystem, a); }
NTSTATUS ZwSinglePhaseReject() { return NtSinglePhaseReject(); }
NTSTATUS NtSinglePhaseReject() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSinglePhaseReject, a); }
NTSTATUS ZwPrePrepareComplete() { return NtPrePrepareComplete(); }
NTSTATUS NtPrePrepareComplete() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtPrePrepareComplete, a); }
NTSTATUS ZwPrePrepareEnlistment() { return NtPrePrepareEnlistment(); }
NTSTATUS NtPrePrepareEnlistment() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtPrePrepareEnlistment, a); }
NTSTATUS ZwPrepareComplete() { return NtPrepareComplete(); }
NTSTATUS NtPrepareComplete() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtPrepareComplete, a); }
NTSTATUS ZwPrepareEnlistment() { return NtPrepareEnlistment(); }
NTSTATUS NtPrepareEnlistment() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtPrepareEnlistment, a); }
NTSTATUS ZwPropagationComplete() { return NtPropagationComplete(); }
NTSTATUS NtPropagationComplete() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtPropagationComplete, a); }
NTSTATUS ZwPropagationFailed() { return NtPropagationFailed(); }
NTSTATUS NtPropagationFailed() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtPropagationFailed, a); }
NTSTATUS ZwRollbackComplete() { return NtRollbackComplete(); }
NTSTATUS NtRollbackComplete() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRollbackComplete, a); }
NTSTATUS ZwRollbackEnlistment() { return NtRollbackEnlistment(); }
NTSTATUS NtRollbackEnlistment() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRollbackEnlistment, a); }
NTSTATUS ZwRollbackRegistryTransaction() { return NtRollbackRegistryTransaction(); }
NTSTATUS NtRollbackRegistryTransaction() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRollbackRegistryTransaction, a); }
NTSTATUS ZwRollbackTransaction() { return NtRollbackTransaction(); }
NTSTATUS NtRollbackTransaction() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRollbackTransaction, a); }
NTSTATUS ZwRollforwardTransactionManager() { return NtRollforwardTransactionManager(); }
NTSTATUS NtRollforwardTransactionManager() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRollforwardTransactionManager, a); }
NTSTATUS ZwRecoverTransactionManager() { return NtRecoverTransactionManager(); }
NTSTATUS NtRecoverTransactionManager() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRecoverTransactionManager, a); }
NTSTATUS ZwRenameTransactionManager() { return NtRenameTransactionManager(); }
NTSTATUS NtRenameTransactionManager() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRenameTransactionManager, a); }
NTSTATUS ZwCommitComplete() { return NtCommitComplete(); }
NTSTATUS NtCommitComplete() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCommitComplete, a); }
NTSTATUS ZwCommitEnlistment() { return NtCommitEnlistment(); }
NTSTATUS NtCommitEnlistment() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCommitEnlistment, a); }
NTSTATUS ZwCommitRegistryTransaction() { return NtCommitRegistryTransaction(); }
NTSTATUS NtCommitRegistryTransaction() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCommitRegistryTransaction, a); }
NTSTATUS ZwCommitTransaction() { return NtCommitTransaction(); }
NTSTATUS NtCommitTransaction() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCommitTransaction, a); }
NTSTATUS ZwRecoverEnlistment() { return NtRecoverEnlistment(); }
NTSTATUS NtRecoverEnlistment() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRecoverEnlistment, a); }
NTSTATUS ZwRecoverResourceManager() { return NtRecoverResourceManager(); }
NTSTATUS NtRecoverResourceManager() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRecoverResourceManager, a); }
NTSTATUS ZwShutdownWorkerFactory() { return NtShutdownWorkerFactory(); }
NTSTATUS NtShutdownWorkerFactory() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtShutdownWorkerFactory, a); }
NTSTATUS ZwCreateWorkerFactory() { return NtCreateWorkerFactory(); }
NTSTATUS NtCreateWorkerFactory() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateWorkerFactory, a); }
NTSTATUS ZwQueryInformationWorkerFactory() { return NtQueryInformationWorkerFactory(); }
NTSTATUS NtQueryInformationWorkerFactory() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryInformationWorkerFactory, a); }
NTSTATUS ZwSetInformationWorkerFactory() { return NtSetInformationWorkerFactory(); }
NTSTATUS NtSetInformationWorkerFactory() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetInformationWorkerFactory, a); }
NTSTATUS ZwReleaseWorkerFactoryWorker() { return NtReleaseWorkerFactoryWorker(); }
NTSTATUS NtReleaseWorkerFactoryWorker() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtReleaseWorkerFactoryWorker, a); }
NTSTATUS ZwWorkerFactoryWorkerReady() { return NtWorkerFactoryWorkerReady(); }
NTSTATUS NtWorkerFactoryWorkerReady() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtWorkerFactoryWorkerReady, a); }
NTSTATUS ZwCreateWaitCompletionPacket() { return NtCreateWaitCompletionPacket(); }
NTSTATUS NtCreateWaitCompletionPacket() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateWaitCompletionPacket, a); }
NTSTATUS ZwCancelWaitCompletionPacket() { return NtCancelWaitCompletionPacket(); }
NTSTATUS NtCancelWaitCompletionPacket() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCancelWaitCompletionPacket, a); }
NTSTATUS ZwAssociateWaitCompletionPacket() { return NtAssociateWaitCompletionPacket(); }
NTSTATUS NtAssociateWaitCompletionPacket() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAssociateWaitCompletionPacket, a); }
NTSTATUS ZwCreateWaitablePort() { return NtCreateWaitablePort(); }
NTSTATUS NtCreateWaitablePort() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateWaitablePort, a); }
NTSTATUS ZwCreatePort() { return NtCreatePort(); }
NTSTATUS NtCreatePort() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreatePort, a); }
NTSTATUS ZwConnectPort() { return NtConnectPort(); }
NTSTATUS NtConnectPort() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtConnectPort, a); }
NTSTATUS ZwAcceptConnectPort() { return NtAcceptConnectPort(); }
NTSTATUS NtAcceptConnectPort() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAcceptConnectPort, a); }
NTSTATUS ZwQueryInformationPort() { return NtQueryInformationPort(); }
NTSTATUS NtQueryInformationPort() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryInformationPort, a); }
NTSTATUS ZwListenPort() { return NtListenPort(); }
NTSTATUS NtListenPort() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtListenPort, a); }
NTSTATUS ZwRequestPort() { return NtRequestPort(); }
NTSTATUS NtRequestPort() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRequestPort, a); }
NTSTATUS ZwReplyPort() { return NtReplyPort(); }
NTSTATUS NtReplyPort() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtReplyPort, a); }
NTSTATUS ZwReplyWaitReceivePort() { return NtReplyWaitReceivePort(); }
NTSTATUS NtReplyWaitReceivePort() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtReplyWaitReceivePort, a); }
NTSTATUS ZwReplyWaitReceivePortEx() { return NtReplyWaitReceivePortEx(); }
NTSTATUS NtReplyWaitReceivePortEx() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtReplyWaitReceivePortEx, a); }
NTSTATUS ZwReplyWaitReplyPort() { return NtReplyWaitReplyPort(); }
NTSTATUS NtReplyWaitReplyPort() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtReplyWaitReplyPort, a); }
NTSTATUS ZwSecureConnectPort() { return NtSecureConnectPort(); }
NTSTATUS NtSecureConnectPort() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSecureConnectPort, a); }
NTSTATUS ZwCompleteConnectPort() { return NtCompleteConnectPort(); }
NTSTATUS NtCompleteConnectPort() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCompleteConnectPort, a); }
W32API NTSTATUS ZwAddBootEntry();
W32API NTSTATUS NtAddBootEntry();
W32API NTSTATUS ZwAddDriverEntry();
W32API NTSTATUS NtAddDriverEntry();
W32API NTSTATUS ZwDeleteBootEntry();
W32API NTSTATUS NtDeleteBootEntry();
W32API NTSTATUS ZwDeleteDriverEntry();
W32API NTSTATUS NtDeleteDriverEntry();
W32API NTSTATUS ZwEnumerateBootEntries();
W32API NTSTATUS NtEnumerateBootEntries();
W32API NTSTATUS ZwEnumerateDriverEntries();
W32API NTSTATUS NtEnumerateDriverEntries();
W32API NTSTATUS ZwQueryBootEntryOrder();
W32API NTSTATUS NtQueryBootEntryOrder();
W32API NTSTATUS ZwSetBootEntryOrder();
W32API NTSTATUS NtSetBootEntryOrder();
W32API NTSTATUS ZwQueryBootOptions();
W32API NTSTATUS NtQueryBootOptions();
W32API NTSTATUS ZwSetBootOptions();
W32API NTSTATUS NtSetBootOptions();
W32API NTSTATUS ZwDisableLastKnownGood();
W32API NTSTATUS NtDisableLastKnownGood();
W32API NTSTATUS ZwEnableLastKnownGood();
W32API NTSTATUS NtEnableLastKnownGood();
W32API NTSTATUS ZwQueryIntervalProfile();
W32API NTSTATUS NtQueryIntervalProfile();
W32API NTSTATUS ZwSetIntervalProfile();
W32API NTSTATUS NtSetIntervalProfile();
W32API NTSTATUS ZwSerializeBoot();
W32API NTSTATUS NtSerializeBoot();
W32API NTSTATUS ZwTraceControl();
W32API NTSTATUS NtTraceControl();
W32API NTSTATUS ZwShutdownSystem();
W32API NTSTATUS NtShutdownSystem();
W32API NTSTATUS ZwSinglePhaseReject();
W32API NTSTATUS NtSinglePhaseReject();
W32API NTSTATUS ZwPrePrepareComplete();
W32API NTSTATUS NtPrePrepareComplete();
W32API NTSTATUS ZwPrePrepareEnlistment();
W32API NTSTATUS NtPrePrepareEnlistment();
W32API NTSTATUS ZwPrepareComplete();
W32API NTSTATUS NtPrepareComplete();
W32API NTSTATUS ZwPrepareEnlistment();
W32API NTSTATUS NtPrepareEnlistment();
W32API NTSTATUS ZwPropagationComplete();
W32API NTSTATUS NtPropagationComplete();
W32API NTSTATUS ZwPropagationFailed();
W32API NTSTATUS NtPropagationFailed();
W32API NTSTATUS ZwRollbackComplete();
W32API NTSTATUS NtRollbackComplete();
W32API NTSTATUS ZwRollbackEnlistment();
W32API NTSTATUS NtRollbackEnlistment();
W32API NTSTATUS ZwRollbackRegistryTransaction();
W32API NTSTATUS NtRollbackRegistryTransaction();
W32API NTSTATUS ZwRollbackTransaction();
W32API NTSTATUS NtRollbackTransaction();
W32API NTSTATUS ZwRollforwardTransactionManager();
W32API NTSTATUS NtRollforwardTransactionManager();
W32API NTSTATUS ZwRecoverTransactionManager();
W32API NTSTATUS NtRecoverTransactionManager();
W32API NTSTATUS ZwRenameTransactionManager();
W32API NTSTATUS NtRenameTransactionManager();
W32API NTSTATUS ZwCommitComplete();
W32API NTSTATUS NtCommitComplete();
W32API NTSTATUS ZwCommitEnlistment();
W32API NTSTATUS NtCommitEnlistment();
W32API NTSTATUS ZwCommitRegistryTransaction();
W32API NTSTATUS NtCommitRegistryTransaction();
W32API NTSTATUS ZwCommitTransaction();
W32API NTSTATUS NtCommitTransaction();
W32API NTSTATUS ZwRecoverEnlistment();
W32API NTSTATUS NtRecoverEnlistment();
W32API NTSTATUS ZwRecoverResourceManager();
W32API NTSTATUS NtRecoverResourceManager();
W32API NTSTATUS ZwShutdownWorkerFactory();
W32API NTSTATUS NtShutdownWorkerFactory();
W32API NTSTATUS ZwCreateWorkerFactory();
W32API NTSTATUS NtCreateWorkerFactory();
W32API NTSTATUS ZwQueryInformationWorkerFactory();
W32API NTSTATUS NtQueryInformationWorkerFactory();
W32API NTSTATUS ZwSetInformationWorkerFactory();
W32API NTSTATUS NtSetInformationWorkerFactory();
W32API NTSTATUS ZwReleaseWorkerFactoryWorker();
W32API NTSTATUS NtReleaseWorkerFactoryWorker();
W32API NTSTATUS ZwWorkerFactoryWorkerReady();
W32API NTSTATUS NtWorkerFactoryWorkerReady();
W32API NTSTATUS ZwCreateWaitCompletionPacket();
W32API NTSTATUS NtCreateWaitCompletionPacket();
W32API NTSTATUS ZwCancelWaitCompletionPacket();
W32API NTSTATUS NtCancelWaitCompletionPacket();
W32API NTSTATUS ZwAssociateWaitCompletionPacket();
W32API NTSTATUS NtAssociateWaitCompletionPacket();
W32API NTSTATUS ZwCreateWaitablePort();
W32API NTSTATUS NtCreateWaitablePort();
W32API NTSTATUS ZwCreatePort();
W32API NTSTATUS NtCreatePort();
W32API NTSTATUS ZwConnectPort();
W32API NTSTATUS NtConnectPort();
W32API NTSTATUS ZwAcceptConnectPort();
W32API NTSTATUS NtAcceptConnectPort();
W32API NTSTATUS ZwQueryInformationPort();
W32API NTSTATUS NtQueryInformationPort();
W32API NTSTATUS ZwSetInformationPort();
W32API NTSTATUS ZwListenPort();
W32API NTSTATUS NtListenPort();
W32API NTSTATUS ZwRequestPort();
W32API NTSTATUS NtRequestPort();
W32API NTSTATUS ZwReplyPort();
W32API NTSTATUS NtReplyPort();
W32API NTSTATUS ZwReplyWaitReceivePort();
W32API NTSTATUS NtReplyWaitReceivePort();
W32API NTSTATUS ZwReplyWaitReceivePortEx();
W32API NTSTATUS NtReplyWaitReceivePortEx();
W32API NTSTATUS ZwReplyWaitReplyPort();
W32API NTSTATUS NtReplyWaitReplyPort();
W32API NTSTATUS ZwSendWaitReceivePort();
W32API NTSTATUS ZwSecureConnectPort();
W32API NTSTATUS NtSecureConnectPort();
W32API NTSTATUS ZwCompleteConnectPort();
W32API NTSTATUS NtCompleteConnectPort();
/* ---- batch M: power / utility / object-state syscalls ---- */

NTSTATUS ZwYieldExecution() { return NtYieldExecution(); }
NTSTATUS NtYieldExecution() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtYieldExecution, a); }

NTSTATUS ZwFlushInstructionCache(HANDLE ImageHandle) { return NtFlushInstructionCache(ImageHandle); }
NTSTATUS NtFlushInstructionCache(HANDLE ImageHandle) { ARGV(1); a[0] = (long)ImageHandle; return (NTSTATUS)nt_dispatch(NR_NtFlushInstructionCache, a); }

NTSTATUS ZwFlushProcessWriteBuffers(HANDLE Process) { return NtFlushProcessWriteBuffers(Process); }
NTSTATUS NtFlushProcessWriteBuffers(HANDLE Process) { ARGV(1); a[0] = (long)Process; return (NTSTATUS)nt_dispatch(NR_NtFlushProcessWriteBuffers, a); }

NTSTATUS ZwGetDevicePowerState(HANDLE Device, W32 *State) { return NtGetDevicePowerState(Device, State); }
NTSTATUS NtGetDevicePowerState(HANDLE Device, W32 *State) { ARGV(2); a[0] = (long)Device; a[1] = (long)State; return (NTSTATUS)nt_dispatch(NR_NtGetDevicePowerState, a); }

NTSTATUS ZwGetWriteWatch(W32 *WatchInfo) { return NtGetWriteWatch(WatchInfo); }
NTSTATUS NtGetWriteWatch(W32 *WatchInfo) { ARGV(1); a[0] = (long)WatchInfo; return (NTSTATUS)nt_dispatch(NR_NtGetWriteWatch, a); }

NTSTATUS ZwGetCachedSigningLevel(W32 *Level) { return NtGetCachedSigningLevel(Level); }
NTSTATUS NtGetCachedSigningLevel(W32 *Level) { ARGV(1); a[0] = (long)Level; return (NTSTATUS)nt_dispatch(NR_NtGetCachedSigningLevel, a); }

NTSTATUS ZwGetNlsSectionPtr(W32 Section, PVOID *Pointer) { return NtGetNlsSectionPtr(Section, Pointer); }
NTSTATUS NtGetNlsSectionPtr(W32 Section, PVOID *Pointer) { ARGV(2); a[0] = (long)Section; a[1] = (long)Pointer; return (NTSTATUS)nt_dispatch(NR_NtGetNlsSectionPtr, a); }

NTSTATUS ZwIsProcessInJob(HANDLE ProcessHandle, HANDLE JobHandle) { return NtIsProcessInJob(ProcessHandle, JobHandle); }
NTSTATUS NtIsProcessInJob(HANDLE ProcessHandle, HANDLE JobHandle) { ARGV(2); a[0] = (long)ProcessHandle; a[1] = (long)JobHandle; return (NTSTATUS)nt_dispatch(NR_NtIsProcessInJob, a); }

NTSTATUS ZwIsSystemResumeAutomatic() { return NtIsSystemResumeAutomatic(); }
NTSTATUS NtIsSystemResumeAutomatic() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtIsSystemResumeAutomatic, a); }

NTSTATUS ZwIsUILanguageComitted(W32 *Committed) { return NtIsUILanguageComitted(Committed); }
NTSTATUS NtIsUILanguageComitted(W32 *Committed) { ARGV(1); a[0] = (long)Committed; return (NTSTATUS)nt_dispatch(NR_NtIsUILanguageComitted, a); }

NTSTATUS ZwInitiatePowerAction() { return NtInitiatePowerAction(); }
NTSTATUS NtInitiatePowerAction() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtInitiatePowerAction, a); }

NTSTATUS ZwLoadDriver() { return NtLoadDriver(); }
NTSTATUS NtLoadDriver() { long a[1]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtLoadDriver, a); }

NTSTATUS ZwLockProductActivationKeys(W32 *Locked) { return NtLockProductActivationKeys(Locked); }
NTSTATUS NtLockProductActivationKeys(W32 *Locked) { ARGV(1); a[0] = (long)Locked; return (NTSTATUS)nt_dispatch(NR_NtLockProductActivationKeys, a); }

NTSTATUS ZwLockRegistryKey(HANDLE Key) { return NtLockRegistryKey(Key); }
NTSTATUS NtLockRegistryKey(HANDLE Key) { ARGV(1); a[0] = (long)Key; return (NTSTATUS)nt_dispatch(NR_NtLockRegistryKey, a); }

NTSTATUS ZwMakeTemporaryObject(HANDLE Object) { return NtMakeTemporaryObject(Object); }
NTSTATUS NtMakeTemporaryObject(HANDLE Object) { ARGV(1); a[0] = (long)Object; return (NTSTATUS)nt_dispatch(NR_NtMakeTemporaryObject, a); }

NTSTATUS ZwMakePermanentObject(HANDLE Object) { return NtMakePermanentObject(Object); }
NTSTATUS NtMakePermanentObject(HANDLE Object) { ARGV(1); a[0] = (long)Object; return (NTSTATUS)nt_dispatch(NR_NtMakePermanentObject, a); }

NTSTATUS ZwManageHotPatch(HANDLE HotPatchObject) { return NtManageHotPatch(HotPatchObject); }
NTSTATUS NtManageHotPatch(HANDLE HotPatchObject) { ARGV(1); a[0] = (long)HotPatchObject; return (NTSTATUS)nt_dispatch(NR_NtManageHotPatch, a); }

NTSTATUS ZwManagePartition(HANDLE Partition) { return NtManagePartition(Partition); }
NTSTATUS NtManagePartition(HANDLE Partition) { ARGV(1); a[0] = (long)Partition; return (NTSTATUS)nt_dispatch(NR_NtManagePartition, a); }

NTSTATUS ZwMapUserPhysicalPages(HANDLE Handle, PVOID *VirtualAddress, W64 Size) { return NtMapUserPhysicalPages(Handle, VirtualAddress, Size); }
NTSTATUS NtMapUserPhysicalPages(HANDLE Handle, PVOID *VirtualAddress, W64 Size) { ARGV(3); a[0] = (long)Handle; a[1] = (long)VirtualAddress; a[2] = (long)Size; return (NTSTATUS)nt_dispatch(NR_NtMapUserPhysicalPages, a); }

NTSTATUS ZwMapUserPhysicalPagesScatter(HANDLE Handle, PVOID *VirtualAddress, W64 Size) { return NtMapUserPhysicalPagesScatter(Handle, VirtualAddress, Size); }
NTSTATUS NtMapUserPhysicalPagesScatter(HANDLE Handle, PVOID *VirtualAddress, W64 Size) { ARGV(3); a[0] = (long)Handle; a[1] = (long)VirtualAddress; a[2] = (long)Size; return (NTSTATUS)nt_dispatch(NR_NtMapUserPhysicalPagesScatter, a); }

NTSTATUS ZwModifyBootEntry(HANDLE BootEntry) { return NtModifyBootEntry(BootEntry); }
NTSTATUS NtModifyBootEntry(HANDLE BootEntry) { ARGV(1); a[0] = (long)BootEntry; return (NTSTATUS)nt_dispatch(NR_NtModifyBootEntry, a); }

NTSTATUS ZwModifyDriverEntry(HANDLE DriverEntry) { return NtModifyDriverEntry(DriverEntry); }
NTSTATUS NtModifyDriverEntry(HANDLE DriverEntry) { ARGV(1); a[0] = (long)DriverEntry; return (NTSTATUS)nt_dispatch(NR_NtModifyDriverEntry, a); }

NTSTATUS ZwNotifyChangeSession(HANDLE Session) { return NtNotifyChangeSession(Session); }
NTSTATUS NtNotifyChangeSession(HANDLE Session) { ARGV(1); a[0] = (long)Session; return (NTSTATUS)nt_dispatch(NR_NtNotifyChangeSession, a); }

NTSTATUS ZwPowerInformation(W32 *Information) { return NtPowerInformation(Information); }
NTSTATUS NtPowerInformation(W32 *Information) { ARGV(1); a[0] = (long)Information; return (NTSTATUS)nt_dispatch(NR_NtPowerInformation, a); }

/* ---------------- registry syscalls: typed wrappers ---------------- */

NTSTATUS ZwCreateKey(HANDLE *KeyHandle, OBJECT_ATTRIBUTES *ObjectAttributes, ULONG DesiredAccess) {
  return NtCreateKey(KeyHandle, ObjectAttributes, DesiredAccess);
}

NTSTATUS NtCreateKey(HANDLE *KeyHandle, OBJECT_ATTRIBUTES *ObjectAttributes, ULONG DesiredAccess) {
  ARGV(3);
  a[0] = (long)KeyHandle; a[1] = (long)ObjectAttributes; a[2] = (long)DesiredAccess;
  return (NTSTATUS)nt_dispatch(NR_NtCreateKey, a);
}

NTSTATUS ZwOpenKey(HANDLE *KeyHandle, ULONG DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes) {
  return NtOpenKey(KeyHandle, DesiredAccess, ObjectAttributes);
}

NTSTATUS NtOpenKey(HANDLE *KeyHandle, ULONG DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes) {
  ARGV(3);
  a[0] = (long)KeyHandle; a[1] = (long)DesiredAccess; a[2] = (long)ObjectAttributes;
  return (NTSTATUS)nt_dispatch(NR_NtOpenKey, a);
}

NTSTATUS ZwQueryKey(HANDLE KeyHandle, W32 InfoClass, PVOID Buffer, W32 BufferLen, W32 *ReturnLength) {
  return NtQueryKey(KeyHandle, InfoClass, Buffer, BufferLen, ReturnLength);
}

NTSTATUS NtQueryKey(HANDLE KeyHandle, W32 InfoClass, PVOID Buffer, W32 BufferLen, W32 *ReturnLength) {
  ARGV(5);
  a[0] = (long)KeyHandle; a[1] = (long)InfoClass; a[2] = (long)Buffer; a[3] = (long)BufferLen;
  a[4] = (long)ReturnLength;
  return (NTSTATUS)nt_dispatch(NR_NtQueryKey, a);
}

NTSTATUS ZwCloseKey(HANDLE KeyHandle) {
  return NtCloseKey(KeyHandle);
}

NTSTATUS NtCloseKey(HANDLE KeyHandle) {
  ARGV(1);
  a[0] = (long)KeyHandle;
  return (NTSTATUS)nt_dispatch(NR_NtCloseKey, a);
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
