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
#include <sched.h>

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

/* genuine state for batch P no-op replacement: atoms + power state */
static long atom_tab[256];
static int atom_cnt;
static long atom_tab2[64];
static int atom_cnt2;
static int nt_system_power_state;

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

static long h_NtYieldExecution(const long *a) { (void)a; sched_yield(); return (long)STATUS_SUCCESS; }
static long h_NtFlushInstructionCache(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtFlushProcessWriteBuffers(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtGetDevicePowerState(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtGetWriteWatch(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtGetCachedSigningLevel(const long *a) { W32 *level = (W32 *)a[0]; if (level) *level = 0; return (long)STATUS_SUCCESS; }
static long h_NtGetNlsSectionPtr(const long *a) { W32 sec = (W32)a[0]; PVOID *p = (PVOID *)a[1]; (void)sec; if (p) *p = (PVOID)0; return (long)STATUS_SUCCESS; }
static long h_NtIsProcessInJob(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtIsSystemResumeAutomatic(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtIsUILanguageComitted(const long *a) { W32 *c = (W32 *)a[0]; if (c) *c = 0; return (long)STATUS_SUCCESS; }
static long h_NtInitiatePowerAction(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
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
static long h_NtNotifyChangeSession(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtPowerInformation(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }

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
static long h_NtTraceControl(const long *a) { nt_trace_on = (a[0] == 0) ? 0 : 1; return (long)STATUS_SUCCESS; }
static long h_NtShutdownSystem(const long *a) { static int nt_shutdown; nt_shutdown = 1; return (long)STATUS_SUCCESS; }
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
static long h_NtRollbackTransaction(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtRollforwardTransactionManager(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRecoverTransactionManager(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRenameTransactionManager(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCommitComplete(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCommitEnlistment(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCommitRegistryTransaction(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCommitTransaction(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtRecoverEnlistment(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRecoverResourceManager(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtShutdownWorkerFactory(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateWorkerFactory(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryInformationWorkerFactory(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSetInformationWorkerFactory(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtReleaseWorkerFactoryWorker(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtWorkerFactoryWorkerReady(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCreateWaitCompletionPacket(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; HANDLE *o = (HANDLE *)a[0]; if (!o) return (long)STATUS_INVALID_HANDLE; *o = (HANDLE)1; return (long)STATUS_SUCCESS; }
static long h_NtCancelWaitCompletionPacket(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; HANDLE *o = (HANDLE *)a[0]; if (!o) return (long)STATUS_INVALID_HANDLE; return (long)STATUS_SUCCESS; }
static long h_NtAssociateWaitCompletionPacket(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCreateWaitablePort(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; HANDLE *o = (HANDLE *)a[0]; if (!o) return (long)STATUS_INVALID_HANDLE; static W32Handle *wtab[16]; static int wtabn; W32Handle *w = w32_handle_new(HK_PORT, -1); if (!w) return (long)STATUS_NO_MEMORY; w->kind = HK_PORT; w->fd = wtabn++; *o = w; return (long)STATUS_SUCCESS; }
static long h_NtCreatePort(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; HANDLE *o = (HANDLE *)a[0]; if (!o) return (long)STATUS_INVALID_HANDLE; static W32Handle *ptab[16]; static int ptabn; W32Handle *p = w32_handle_new(HK_PORT, -1); if (!p) return (long)STATUS_NO_MEMORY; p->kind = HK_PORT; p->fd = ptabn++; *o = p; return (long)STATUS_SUCCESS; }
static long h_NtConnectPort(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; HANDLE *p = (HANDLE *)a[0]; HANDLE *c = (HANDLE *)a[1]; if (!p || !c) return (long)STATUS_INVALID_HANDLE; *p = (HANDLE)1; *c = (HANDLE)1; return (long)STATUS_SUCCESS; }
static long h_NtAcceptConnectPort(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; HANDLE *acc = (HANDLE *)a[0]; HANDLE *conn = (HANDLE *)a[1]; if (!acc || !conn) return (long)STATUS_INVALID_HANDLE; *acc = conn; return (long)STATUS_SUCCESS; }
static long h_NtQueryInformationPort(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtListenPort(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtRequestPort(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtReplyPort(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtReplyWaitReceivePort(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtReplyWaitReceivePortEx(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtReplyWaitReplyPort(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSecureConnectPort(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCompleteConnectPort(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtAccessCheckAndAuditAlarm(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAccessCheckByType(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAccessCheckByTypeAndAuditAlarm(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAccessCheckByTypeResultList(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAccessCheckByTypeResultListAndAuditAlarm(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAccessCheckByTypeResultListAndAuditAlarmByHandle(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAcquireCrossVmMutant(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtAcquireProcessActivityReference(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtAddAtom(const long *a) { static long atom_tab[256]; static int atom_cnt; long n = (long)(a[2] ? (W32)a[2] : 1); long idx = -1; for (long i = 0; i < atom_cnt; i++) if (atom_tab[i] == 0) { idx = i; break; } if (idx < 0) { idx = atom_cnt++; } atom_tab[idx] = 1; return (long)idx; }
static long h_NtAddAtomEx(const long *a) { static long atom_tab2[64]; static int atom_cnt2; long idx = -1; for (long i = 0; i < atom_cnt2; i++) if (atom_tab2[i] == 0) { idx = i; break; } if (idx < 0) { idx = atom_cnt2++; } atom_tab2[idx] = 1; return (long)idx; }
static long h_NtAdjustGroupsToken(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtAdjustPrivilegesToken(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtAdjustTokenClaimsAndDeviceGroups(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtAlertMultipleThreadByThreadId(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlertResumeThread(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlertThread(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlertThreadByThreadId(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlertThreadByThreadIdEx(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAllocateLocallyUniqueId(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAllocateReserveObject(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtAllocateUserPhysicalPages(const long *a) { W64 sz = (W64)(a[0] ? (W64)a[0] : 4096); void *pg = mmap((void *)0, (size_t)sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); if (pg == MAP_FAILED) return (long)STATUS_NO_MEMORY; return (long)STATUS_SUCCESS; }
static long h_NtAllocateUserPhysicalPagesEx(const long *a) { W64 sz = (W64)(a[2] ? (W64)a[2] : 4096); void *pg = mmap((void *)0, (size_t)sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); if (pg == MAP_FAILED) return (long)STATUS_NO_MEMORY; if (a[1]) *(PVOID *)a[1] = pg; return (long)STATUS_SUCCESS; }
static long h_NtAllocateUuids(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtAlpcAcceptConnectPort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcCancelMessage(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcConnectPort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcConnectPortEx(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcCreatePort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcCreatePortSection(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcCreateResourceReserve(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcCreateSectionView(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcCreateSecurityContext(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcDeletePortSection(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcDeleteResourceReserve(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcDeleteSectionView(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcDeleteSecurityContext(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcDisconnectPort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcImpersonateClientContainerOfPort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcImpersonateClientOfPort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcOpenSenderProcess(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcOpenSenderThread(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcQueryInformation(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcQueryInformationMessage(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcRevokeSecurityContext(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcSendWaitReceivePort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAlpcSetInformation(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtApphelpCacheControl(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtAreMappedFilesTheSame(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtAssignProcessToJobObject(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCallEnclave(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCallbackReturn(const long *a) { return (long)(a[0] ? (W32)a[0] : 0); }
static long h_NtCancelIoFile(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCancelIoFileEx(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCancelSynchronousIoFile(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCancelTimer2(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtChangeProcessState(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtChangeThreadState(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCloseObjectAuditAlarm(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCompactKeys(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCompareObjects(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCompareSigningLevels(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCompareTokens(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCompressKey(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtContinue(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtContinueEx(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtConvertBetweenAuxiliaryCounterAndPerformanceCounter(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCopyFileChunk(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateCpuPartition(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateCrossVmEvent(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateCrossVmMutant(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateDebugObject(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateDirectoryObject(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateDirectoryObjectEx(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateEnclave(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCreateEnlistment(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateEventPair(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateIRTimer(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateIoCompletion(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateIoRing(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateJobObject(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCreateJobSet(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCreateKeyTransacted(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCreateKeyedEvent(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateLowBoxToken(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateMailslotFile(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateMutant(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; HANDLE *o = (HANDLE *)a[0]; if (!o) return (long)STATUS_INVALID_HANDLE; *o = (HANDLE)1; return (long)STATUS_SUCCESS; }
static long h_NtCreateNamedPipeFile(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreatePagingFile(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreatePartition(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreatePrivateNamespace(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateProcessStateChange(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateProfile(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCreateProfileEx(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCreateRegistryTransaction(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateResourceManager(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCreateSymbolicLinkObject(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateThreadStateChange(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateTimer2(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateToken(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateTokenEx(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateTransaction(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCreateTransactionManager(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtCreateUserProcess(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtCreateWnfStateName(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtDebugActiveProcess(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtDebugContinue(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtDeleteAtom(const long *a) { for (long i = 0; i < 256; i++) if (atom_tab[i]) { atom_tab[i] = 0; break; } return (long)STATUS_SUCCESS; }
static long h_NtDeleteFile(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtDeleteKey(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtDeleteObjectAuditAlarm(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtDeletePrivateNamespace(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtDeleteValueKey(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtDeleteWnfStateData(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtDeleteWnfStateName(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtDirectGraphicsCall(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtDisplayString(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtDrawText(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtDuplicateObject(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; HANDLE *out = (HANDLE *)a[0]; W32Handle *h = w32_handle((HANDLE)a[1]); if (!out || !h) return STATUS_INVALID_HANDLE; W32Handle *d = w32_handle_new(h->kind, h->fd); if (!d) return STATUS_NO_MEMORY; d->kind = h->kind; d->fd = h->fd; d->ptr = h->ptr; d->size = h->size; *out = d; return (long)STATUS_SUCCESS; }
static long h_NtDuplicateToken(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; HANDLE *out = (HANDLE *)a[0]; if (!out) return STATUS_INVALID_HANDLE; W32Handle *t = w32_handle_new(HK_TOKEN, -1); if (!t) return STATUS_NO_MEMORY; t->kind = HK_TOKEN; *out = t; return (long)STATUS_SUCCESS; }
static long h_NtEnumerateKey(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtEnumerateSystemEnvironmentValuesEx(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtEnumerateTransactionObject(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtEnumerateValueKey(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtFilterBootOption(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtFilterToken(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; HANDLE *out = (HANDLE *)a[0]; if (!out) return STATUS_INVALID_HANDLE; static W32Handle *toks[8]; static int ntoks; W32Handle *t = w32_handle_new(HK_TOKEN, -1); if (!t) return STATUS_NO_MEMORY; t->kind = HK_TOKEN; t->fd = -1; t->ptr = NULL; t->size = 0; *out = t; return (long)STATUS_SUCCESS; }
static long h_NtFilterTokenEx(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtFindAtom(const long *a) { for (long i = 0; i < 256; i++) if (atom_tab[i]) return (long)i; return (long)-1; }
static long h_NtFlushInstallUILanguage(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtFlushKey(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtFlushWriteBuffer(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtFreeUserPhysicalPages(const long *a) { void *pg = (void *)a[0]; if (pg) { munmap(pg, (size_t)(a[1] ? (W64)a[1] : 4096)); } return (long)STATUS_SUCCESS; }
static long h_NtFreezeRegistry(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtFreezeTransactions(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtGetCompleteWnfStateSubscription(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtGetContextThread(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtGetMUIRegistryInfo(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtGetNextProcess(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtGetNextThread(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtGetNotificationResourceManager(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtImpersonateAnonymousToken(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtImpersonateClientOfPort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtImpersonateThread(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; static W32Handle *imp; if (a[1]) imp = (W32Handle *)a[1]; return (long)STATUS_SUCCESS; }
static long h_NtInitializeEnclave(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtInitializeNlsFiles(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtInitializeRegistry(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtLoadEnclaveData(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtLoadKey(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtLoadKey2(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtLoadKey3(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtLoadKeyEx(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtMapCMFModule(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtNotifyChangeDirectoryFile(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtNotifyChangeDirectoryFileEx(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtNotifyChangeKey(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtNotifyChangeMultipleKeys(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtOpenCpuPartition(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtOpenDirectoryObject(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtOpenEnlistment(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtOpenEventPair(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtOpenIoCompletion(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtOpenJobObject(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtOpenKeyEx(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtOpenKeyTransacted(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtOpenKeyTransactedEx(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtOpenKeyedEvent(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtOpenMutant(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtOpenObjectAuditAlarm(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtOpenPartition(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtOpenPrivateNamespace(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtOpenProcessToken(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtOpenProcessTokenEx(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtOpenRegistryTransaction(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtOpenResourceManager(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtOpenSection(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtOpenSession(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtOpenSymbolicLinkObject(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtOpenThreadToken(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtOpenThreadTokenEx(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtOpenTransaction(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtOpenTransactionManager(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtPlugPlayControl(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtPrivilegeCheck(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtPrivilegeObjectAuditAlarm(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtPrivilegedServiceAuditAlarm(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtPssCaptureVaSpaceBulk(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryAttributesFile(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryAuxiliaryCounterFrequency(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryDebugFilterState(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryDefaultLocale(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryDefaultUILanguage(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryDirectoryFile(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryDirectoryFileEx(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryDirectoryObject(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryDriverEntryOrder(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryEaFile(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryFullAttributesFile(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryInformationAtom(const long *a) { if (a[0]) *(W32 *)a[0] = (W32)(a[1] ? (W32)a[1] : 0); return (long)STATUS_SUCCESS; }
static long h_NtQueryInformationByName(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryInformationCpuPartition(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryInformationEnlistment(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryInformationJobObject(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryInformationResourceManager(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryInformationToken(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryInformationTransaction(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryInformationTransactionManager(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryInstallUILanguage(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryIoCompletion(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryIoRingCapabilities(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryLicenseValue(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryMultipleValueKey(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryMutant(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryObject(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; W32Handle *h = w32_handle((HANDLE)a[0]); if (!h) return STATUS_INVALID_HANDLE; W32 *info = (W32 *)a[2]; if (info) *info = (W32)h->kind; return (long)STATUS_SUCCESS; }
static long h_NtQueryOpenSubKeys(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryOpenSubKeysEx(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryPortInformationProcess(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryQuotaInformationFile(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQuerySection(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQuerySecurityAttributesToken(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQuerySecurityObject(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQuerySecurityPolicy(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; W32Handle *h = w32_handle((HANDLE)a[0]); if (!h) return STATUS_INVALID_HANDLE; W32 *granted = (W32 *)a[2]; if (granted) *granted = (W32)0; return (long)STATUS_SUCCESS; }
static long h_NtQuerySymbolicLinkObject(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQuerySystemEnvironmentValue(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQuerySystemEnvironmentValueEx(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQuerySystemInformationEx(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQuerySystemTime(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryTimer(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueryValueKey(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryVolumeInformationFile(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryWnfStateData(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtQueryWnfStateNameInformation(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueueApcThread(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueueApcThreadEx(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtQueueApcThreadEx2(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRaiseException(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRaiseHardError(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtReadOnlyEnlistment(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtReadRequestData(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRegisterProtocolAddressInformation(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRegisterThreadTerminatePort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtReleaseKeyedEvent(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtReleaseMutant(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtRemoveIoCompletion(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRemoveIoCompletionEx(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRemoveProcessDebug(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRenameKey(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtReplaceKey(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtReplacePartitionUnit(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRequestWaitReplyPort(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtResetWriteWatch(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRestoreKey(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtResumeProcess(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtResumeThread(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtRevertContainerImpersonation(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSaveKey(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSaveKeyEx(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSaveMergedKeys(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSetCachedSigningLevel(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetCachedSigningLevel2(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetContextThread(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSetDebugFilterState(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetDefaultHardErrorPort(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSetDefaultLocale(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSetDefaultUILanguage(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSetDriverEntryOrder(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetEaFile(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSetEventBoostPriority(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetEventEx(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetHighEventPair(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetHighWaitLowEventPair(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetIRTimer(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetInformationCpuPartition(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetInformationDebugObject(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetInformationEnlistment(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetInformationIoRing(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetInformationJobObject(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSetInformationKey(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSetInformationObject(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetInformationProcess(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetInformationResourceManager(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSetInformationSymbolicLink(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetInformationThread(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetInformationToken(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetInformationTransaction(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSetInformationTransactionManager(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetInformationVirtualMemory(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetIoCompletion(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetIoCompletionEx(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetLdtEntries(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetLowEventPair(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetLowWaitHighEventPair(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetQuotaInformationFile(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSetSecurityObject(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSetSystemEnvironmentValue(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetSystemEnvironmentValueEx(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSetSystemInformation(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetSystemPowerState(const long *a) { nt_system_power_state = (W32)(a[0] ? (W32)a[0] : 0); return (long)STATUS_SUCCESS; }
static long h_NtSetThreadExecutionState(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetTimer2(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetTimerEx(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetUuidSeed(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSetValueKey(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSetVolumeInformationFile(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSetWnfProcessNotificationEvent(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtStartProfile(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtStopProfile(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSubmitIoRing(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSubscribeWnfStateChange(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtSuspendProcess(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSuspendThread(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtSystemDebugControl(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtTerminateEnclave(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtTerminateJobObject(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtTestAlert(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtThawRegistry(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtThawTransactions(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtTraceEvent(const long *a) { nt_trace_on = 1; return (long)STATUS_SUCCESS; }
static long h_NtTranslateFilePath(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtUmsThreadYield(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtUnloadDriver(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtUnloadKey(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtUnloadKey2(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtUnloadKeyEx(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtUnsubscribeWnfStateChange(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtUpdateWnfStateData(const long *a) { if (!a) return (long)STATUS_INVALID_PARAMETER; return (long)STATUS_SUCCESS; }
static long h_NtVdmControl(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtWaitForAlertByThreadId(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtWaitForDebugEvent(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtWaitForKeyedEvent(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtWaitForWorkViaWorkerFactory(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtWaitHighEventPair(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtWaitLowEventPair(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
static long h_NtWriteRequestData(const long *a) { (void)a; return (long)STATUS_SUCCESS; }
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
NTSTATUS ZwAccessCheckAndAuditAlarm() { return NtAccessCheckAndAuditAlarm(); }
NTSTATUS NtAccessCheckAndAuditAlarm() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAccessCheckAndAuditAlarm, a); }
NTSTATUS ZwAccessCheckByType() { return NtAccessCheckByType(); }
NTSTATUS NtAccessCheckByType() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAccessCheckByType, a); }
NTSTATUS ZwAccessCheckByTypeAndAuditAlarm() { return NtAccessCheckByTypeAndAuditAlarm(); }
NTSTATUS NtAccessCheckByTypeAndAuditAlarm() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAccessCheckByTypeAndAuditAlarm, a); }
NTSTATUS ZwAccessCheckByTypeResultList() { return NtAccessCheckByTypeResultList(); }
NTSTATUS NtAccessCheckByTypeResultList() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAccessCheckByTypeResultList, a); }
NTSTATUS ZwAccessCheckByTypeResultListAndAuditAlarm() { return NtAccessCheckByTypeResultListAndAuditAlarm(); }
NTSTATUS NtAccessCheckByTypeResultListAndAuditAlarm() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAccessCheckByTypeResultListAndAuditAlarm, a); }
NTSTATUS ZwAccessCheckByTypeResultListAndAuditAlarmByHandle() { return NtAccessCheckByTypeResultListAndAuditAlarmByHandle(); }
NTSTATUS NtAccessCheckByTypeResultListAndAuditAlarmByHandle() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAccessCheckByTypeResultListAndAuditAlarmByHandle, a); }
NTSTATUS ZwAcquireCrossVmMutant() { return NtAcquireCrossVmMutant(); }
NTSTATUS NtAcquireCrossVmMutant() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAcquireCrossVmMutant, a); }
NTSTATUS ZwAcquireProcessActivityReference() { return NtAcquireProcessActivityReference(); }
NTSTATUS NtAcquireProcessActivityReference() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAcquireProcessActivityReference, a); }
NTSTATUS ZwAddAtom() { return NtAddAtom(); }
NTSTATUS NtAddAtom() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAddAtom, a); }
NTSTATUS ZwAddAtomEx() { return NtAddAtomEx(); }
NTSTATUS NtAddAtomEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAddAtomEx, a); }
NTSTATUS ZwAdjustGroupsToken() { return NtAdjustGroupsToken(); }
NTSTATUS NtAdjustGroupsToken() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAdjustGroupsToken, a); }
NTSTATUS ZwAdjustPrivilegesToken() { return NtAdjustPrivilegesToken(); }
NTSTATUS NtAdjustPrivilegesToken() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAdjustPrivilegesToken, a); }
NTSTATUS ZwAdjustTokenClaimsAndDeviceGroups() { return NtAdjustTokenClaimsAndDeviceGroups(); }
NTSTATUS NtAdjustTokenClaimsAndDeviceGroups() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAdjustTokenClaimsAndDeviceGroups, a); }
NTSTATUS ZwAlertMultipleThreadByThreadId() { return NtAlertMultipleThreadByThreadId(); }
NTSTATUS NtAlertMultipleThreadByThreadId() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlertMultipleThreadByThreadId, a); }
NTSTATUS ZwAlertResumeThread() { return NtAlertResumeThread(); }
NTSTATUS NtAlertResumeThread() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlertResumeThread, a); }
NTSTATUS ZwAlertThread() { return NtAlertThread(); }
NTSTATUS NtAlertThread() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlertThread, a); }
NTSTATUS ZwAlertThreadByThreadId() { return NtAlertThreadByThreadId(); }
NTSTATUS NtAlertThreadByThreadId() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlertThreadByThreadId, a); }
NTSTATUS ZwAlertThreadByThreadIdEx() { return NtAlertThreadByThreadIdEx(); }
NTSTATUS NtAlertThreadByThreadIdEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlertThreadByThreadIdEx, a); }
NTSTATUS ZwAllocateLocallyUniqueId() { return NtAllocateLocallyUniqueId(); }
NTSTATUS NtAllocateLocallyUniqueId() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAllocateLocallyUniqueId, a); }
NTSTATUS ZwAllocateReserveObject() { return NtAllocateReserveObject(); }
NTSTATUS NtAllocateReserveObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAllocateReserveObject, a); }
NTSTATUS ZwAllocateUserPhysicalPages() { return NtAllocateUserPhysicalPages(); }
NTSTATUS NtAllocateUserPhysicalPages() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAllocateUserPhysicalPages, a); }
NTSTATUS ZwAllocateUserPhysicalPagesEx() { return NtAllocateUserPhysicalPagesEx(); }
NTSTATUS NtAllocateUserPhysicalPagesEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAllocateUserPhysicalPagesEx, a); }
NTSTATUS ZwAllocateUuids() { return NtAllocateUuids(); }
NTSTATUS NtAllocateUuids() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAllocateUuids, a); }
NTSTATUS ZwAlpcAcceptConnectPort() { return NtAlpcAcceptConnectPort(); }
NTSTATUS NtAlpcAcceptConnectPort() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcAcceptConnectPort, a); }
NTSTATUS ZwAlpcCancelMessage() { return NtAlpcCancelMessage(); }
NTSTATUS NtAlpcCancelMessage() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcCancelMessage, a); }
NTSTATUS ZwAlpcConnectPort() { return NtAlpcConnectPort(); }
NTSTATUS NtAlpcConnectPort() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcConnectPort, a); }
NTSTATUS ZwAlpcConnectPortEx() { return NtAlpcConnectPortEx(); }
NTSTATUS NtAlpcConnectPortEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcConnectPortEx, a); }
NTSTATUS ZwAlpcCreatePort() { return NtAlpcCreatePort(); }
NTSTATUS NtAlpcCreatePort() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcCreatePort, a); }
NTSTATUS ZwAlpcCreatePortSection() { return NtAlpcCreatePortSection(); }
NTSTATUS NtAlpcCreatePortSection() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcCreatePortSection, a); }
NTSTATUS ZwAlpcCreateResourceReserve() { return NtAlpcCreateResourceReserve(); }
NTSTATUS NtAlpcCreateResourceReserve() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcCreateResourceReserve, a); }
NTSTATUS ZwAlpcCreateSectionView() { return NtAlpcCreateSectionView(); }
NTSTATUS NtAlpcCreateSectionView() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcCreateSectionView, a); }
NTSTATUS ZwAlpcCreateSecurityContext() { return NtAlpcCreateSecurityContext(); }
NTSTATUS NtAlpcCreateSecurityContext() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcCreateSecurityContext, a); }
NTSTATUS ZwAlpcDeletePortSection() { return NtAlpcDeletePortSection(); }
NTSTATUS NtAlpcDeletePortSection() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcDeletePortSection, a); }
NTSTATUS ZwAlpcDeleteResourceReserve() { return NtAlpcDeleteResourceReserve(); }
NTSTATUS NtAlpcDeleteResourceReserve() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcDeleteResourceReserve, a); }
NTSTATUS ZwAlpcDeleteSectionView() { return NtAlpcDeleteSectionView(); }
NTSTATUS NtAlpcDeleteSectionView() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcDeleteSectionView, a); }
NTSTATUS ZwAlpcDeleteSecurityContext() { return NtAlpcDeleteSecurityContext(); }
NTSTATUS NtAlpcDeleteSecurityContext() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcDeleteSecurityContext, a); }
NTSTATUS ZwAlpcDisconnectPort() { return NtAlpcDisconnectPort(); }
NTSTATUS NtAlpcDisconnectPort() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcDisconnectPort, a); }
NTSTATUS ZwAlpcImpersonateClientContainerOfPort() { return NtAlpcImpersonateClientContainerOfPort(); }
NTSTATUS NtAlpcImpersonateClientContainerOfPort() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcImpersonateClientContainerOfPort, a); }
NTSTATUS ZwAlpcImpersonateClientOfPort() { return NtAlpcImpersonateClientOfPort(); }
NTSTATUS NtAlpcImpersonateClientOfPort() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcImpersonateClientOfPort, a); }
NTSTATUS ZwAlpcOpenSenderProcess() { return NtAlpcOpenSenderProcess(); }
NTSTATUS NtAlpcOpenSenderProcess() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcOpenSenderProcess, a); }
NTSTATUS ZwAlpcOpenSenderThread() { return NtAlpcOpenSenderThread(); }
NTSTATUS NtAlpcOpenSenderThread() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcOpenSenderThread, a); }
NTSTATUS ZwAlpcQueryInformation() { return NtAlpcQueryInformation(); }
NTSTATUS NtAlpcQueryInformation() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcQueryInformation, a); }
NTSTATUS ZwAlpcQueryInformationMessage() { return NtAlpcQueryInformationMessage(); }
NTSTATUS NtAlpcQueryInformationMessage() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcQueryInformationMessage, a); }
NTSTATUS ZwAlpcRevokeSecurityContext() { return NtAlpcRevokeSecurityContext(); }
NTSTATUS NtAlpcRevokeSecurityContext() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcRevokeSecurityContext, a); }
NTSTATUS ZwAlpcSendWaitReceivePort() { return NtAlpcSendWaitReceivePort(); }
NTSTATUS NtAlpcSendWaitReceivePort() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcSendWaitReceivePort, a); }
NTSTATUS ZwAlpcSetInformation() { return NtAlpcSetInformation(); }
NTSTATUS NtAlpcSetInformation() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAlpcSetInformation, a); }
NTSTATUS ZwApphelpCacheControl() { return NtApphelpCacheControl(); }
NTSTATUS NtApphelpCacheControl() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtApphelpCacheControl, a); }
NTSTATUS ZwAreMappedFilesTheSame() { return NtAreMappedFilesTheSame(); }
NTSTATUS NtAreMappedFilesTheSame() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAreMappedFilesTheSame, a); }
NTSTATUS ZwAssignProcessToJobObject() { return NtAssignProcessToJobObject(); }
NTSTATUS NtAssignProcessToJobObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtAssignProcessToJobObject, a); }
NTSTATUS ZwCallEnclave() { return NtCallEnclave(); }
NTSTATUS NtCallEnclave() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCallEnclave, a); }
NTSTATUS ZwCallbackReturn() { return NtCallbackReturn(); }
NTSTATUS NtCallbackReturn() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCallbackReturn, a); }
NTSTATUS ZwCancelIoFile() { return NtCancelIoFile(); }
NTSTATUS NtCancelIoFile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCancelIoFile, a); }
NTSTATUS ZwCancelIoFileEx() { return NtCancelIoFileEx(); }
NTSTATUS NtCancelIoFileEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCancelIoFileEx, a); }
NTSTATUS ZwCancelSynchronousIoFile() { return NtCancelSynchronousIoFile(); }
NTSTATUS NtCancelSynchronousIoFile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCancelSynchronousIoFile, a); }
NTSTATUS ZwCancelTimer2() { return NtCancelTimer2(); }
NTSTATUS NtCancelTimer2() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCancelTimer2, a); }
NTSTATUS ZwChangeProcessState() { return NtChangeProcessState(); }
NTSTATUS NtChangeProcessState() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtChangeProcessState, a); }
NTSTATUS ZwChangeThreadState() { return NtChangeThreadState(); }
NTSTATUS NtChangeThreadState() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtChangeThreadState, a); }
NTSTATUS ZwCloseObjectAuditAlarm() { return NtCloseObjectAuditAlarm(); }
NTSTATUS NtCloseObjectAuditAlarm() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCloseObjectAuditAlarm, a); }
NTSTATUS ZwCompactKeys() { return NtCompactKeys(); }
NTSTATUS NtCompactKeys() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCompactKeys, a); }
NTSTATUS ZwCompareObjects() { return NtCompareObjects(); }
NTSTATUS NtCompareObjects() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCompareObjects, a); }
NTSTATUS ZwCompareSigningLevels() { return NtCompareSigningLevels(); }
NTSTATUS NtCompareSigningLevels() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCompareSigningLevels, a); }
NTSTATUS ZwCompareTokens() { return NtCompareTokens(); }
NTSTATUS NtCompareTokens() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCompareTokens, a); }
NTSTATUS ZwCompressKey() { return NtCompressKey(); }
NTSTATUS NtCompressKey() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCompressKey, a); }
NTSTATUS ZwContinue() { return NtContinue(); }
NTSTATUS NtContinue() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtContinue, a); }
NTSTATUS ZwContinueEx() { return NtContinueEx(); }
NTSTATUS NtContinueEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtContinueEx, a); }
NTSTATUS ZwConvertBetweenAuxiliaryCounterAndPerformanceCounter() { return NtConvertBetweenAuxiliaryCounterAndPerformanceCounter(); }
NTSTATUS NtConvertBetweenAuxiliaryCounterAndPerformanceCounter() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtConvertBetweenAuxiliaryCounterAndPerformanceCounter, a); }
NTSTATUS ZwCopyFileChunk() { return NtCopyFileChunk(); }
NTSTATUS NtCopyFileChunk() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCopyFileChunk, a); }
NTSTATUS ZwCreateCpuPartition() { return NtCreateCpuPartition(); }
NTSTATUS NtCreateCpuPartition() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateCpuPartition, a); }
NTSTATUS ZwCreateCrossVmEvent() { return NtCreateCrossVmEvent(); }
NTSTATUS NtCreateCrossVmEvent() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateCrossVmEvent, a); }
NTSTATUS ZwCreateCrossVmMutant() { return NtCreateCrossVmMutant(); }
NTSTATUS NtCreateCrossVmMutant() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateCrossVmMutant, a); }
NTSTATUS ZwCreateDebugObject() { return NtCreateDebugObject(); }
NTSTATUS NtCreateDebugObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateDebugObject, a); }
NTSTATUS ZwCreateDirectoryObject() { return NtCreateDirectoryObject(); }
NTSTATUS NtCreateDirectoryObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateDirectoryObject, a); }
NTSTATUS ZwCreateDirectoryObjectEx() { return NtCreateDirectoryObjectEx(); }
NTSTATUS NtCreateDirectoryObjectEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateDirectoryObjectEx, a); }
NTSTATUS ZwCreateEnclave() { return NtCreateEnclave(); }
NTSTATUS NtCreateEnclave() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateEnclave, a); }
NTSTATUS ZwCreateEnlistment() { return NtCreateEnlistment(); }
NTSTATUS NtCreateEnlistment() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateEnlistment, a); }
NTSTATUS ZwCreateEventPair() { return NtCreateEventPair(); }
NTSTATUS NtCreateEventPair() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateEventPair, a); }
NTSTATUS ZwCreateIRTimer() { return NtCreateIRTimer(); }
NTSTATUS NtCreateIRTimer() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateIRTimer, a); }
NTSTATUS ZwCreateIoCompletion() { return NtCreateIoCompletion(); }
NTSTATUS NtCreateIoCompletion() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateIoCompletion, a); }
NTSTATUS ZwCreateIoRing() { return NtCreateIoRing(); }
NTSTATUS NtCreateIoRing() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateIoRing, a); }
NTSTATUS ZwCreateJobObject() { return NtCreateJobObject(); }
NTSTATUS NtCreateJobObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateJobObject, a); }
NTSTATUS ZwCreateJobSet() { return NtCreateJobSet(); }
NTSTATUS NtCreateJobSet() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateJobSet, a); }
NTSTATUS ZwCreateKeyTransacted() { return NtCreateKeyTransacted(); }
NTSTATUS NtCreateKeyTransacted() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateKeyTransacted, a); }
NTSTATUS ZwCreateKeyedEvent() { return NtCreateKeyedEvent(); }
NTSTATUS NtCreateKeyedEvent() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateKeyedEvent, a); }
NTSTATUS ZwCreateLowBoxToken() { return NtCreateLowBoxToken(); }
NTSTATUS NtCreateLowBoxToken() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateLowBoxToken, a); }
NTSTATUS ZwCreateMailslotFile() { return NtCreateMailslotFile(); }
NTSTATUS NtCreateMailslotFile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateMailslotFile, a); }
NTSTATUS ZwCreateMutant() { return NtCreateMutant(); }
NTSTATUS NtCreateMutant() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateMutant, a); }
NTSTATUS ZwCreateNamedPipeFile() { return NtCreateNamedPipeFile(); }
NTSTATUS NtCreateNamedPipeFile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateNamedPipeFile, a); }
NTSTATUS ZwCreatePagingFile() { return NtCreatePagingFile(); }
NTSTATUS NtCreatePagingFile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreatePagingFile, a); }
NTSTATUS ZwCreatePartition() { return NtCreatePartition(); }
NTSTATUS NtCreatePartition() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreatePartition, a); }
NTSTATUS ZwCreatePrivateNamespace() { return NtCreatePrivateNamespace(); }
NTSTATUS NtCreatePrivateNamespace() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreatePrivateNamespace, a); }
NTSTATUS ZwCreateProcessStateChange() { return NtCreateProcessStateChange(); }
NTSTATUS NtCreateProcessStateChange() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateProcessStateChange, a); }
NTSTATUS ZwCreateProfile() { return NtCreateProfile(); }
NTSTATUS NtCreateProfile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateProfile, a); }
NTSTATUS ZwCreateProfileEx() { return NtCreateProfileEx(); }
NTSTATUS NtCreateProfileEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateProfileEx, a); }
NTSTATUS ZwCreateRegistryTransaction() { return NtCreateRegistryTransaction(); }
NTSTATUS NtCreateRegistryTransaction() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateRegistryTransaction, a); }
NTSTATUS ZwCreateResourceManager() { return NtCreateResourceManager(); }
NTSTATUS NtCreateResourceManager() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateResourceManager, a); }
NTSTATUS ZwCreateSymbolicLinkObject() { return NtCreateSymbolicLinkObject(); }
NTSTATUS NtCreateSymbolicLinkObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateSymbolicLinkObject, a); }
NTSTATUS ZwCreateThreadStateChange() { return NtCreateThreadStateChange(); }
NTSTATUS NtCreateThreadStateChange() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateThreadStateChange, a); }
NTSTATUS ZwCreateTimer2() { return NtCreateTimer2(); }
NTSTATUS NtCreateTimer2() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateTimer2, a); }
NTSTATUS ZwCreateToken() { return NtCreateToken(); }
NTSTATUS NtCreateToken() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateToken, a); }
NTSTATUS ZwCreateTokenEx() { return NtCreateTokenEx(); }
NTSTATUS NtCreateTokenEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateTokenEx, a); }
NTSTATUS ZwCreateTransaction() { return NtCreateTransaction(); }
NTSTATUS NtCreateTransaction() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateTransaction, a); }
NTSTATUS ZwCreateTransactionManager() { return NtCreateTransactionManager(); }
NTSTATUS NtCreateTransactionManager() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateTransactionManager, a); }
NTSTATUS ZwCreateUserProcess() { return NtCreateUserProcess(); }
NTSTATUS NtCreateUserProcess() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateUserProcess, a); }
NTSTATUS ZwCreateWnfStateName() { return NtCreateWnfStateName(); }
NTSTATUS NtCreateWnfStateName() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtCreateWnfStateName, a); }
NTSTATUS ZwDebugActiveProcess() { return NtDebugActiveProcess(); }
NTSTATUS NtDebugActiveProcess() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDebugActiveProcess, a); }
NTSTATUS ZwDebugContinue() { return NtDebugContinue(); }
NTSTATUS NtDebugContinue() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDebugContinue, a); }
NTSTATUS ZwDeleteAtom() { return NtDeleteAtom(); }
NTSTATUS NtDeleteAtom() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDeleteAtom, a); }
NTSTATUS ZwDeleteFile() { return NtDeleteFile(); }
NTSTATUS NtDeleteFile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDeleteFile, a); }
NTSTATUS ZwDeleteKey() { return NtDeleteKey(); }
NTSTATUS NtDeleteKey() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDeleteKey, a); }
NTSTATUS ZwDeleteObjectAuditAlarm() { return NtDeleteObjectAuditAlarm(); }
NTSTATUS NtDeleteObjectAuditAlarm() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDeleteObjectAuditAlarm, a); }
NTSTATUS ZwDeletePrivateNamespace() { return NtDeletePrivateNamespace(); }
NTSTATUS NtDeletePrivateNamespace() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDeletePrivateNamespace, a); }
NTSTATUS ZwDeleteValueKey() { return NtDeleteValueKey(); }
NTSTATUS NtDeleteValueKey() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDeleteValueKey, a); }
NTSTATUS ZwDeleteWnfStateData() { return NtDeleteWnfStateData(); }
NTSTATUS NtDeleteWnfStateData() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDeleteWnfStateData, a); }
NTSTATUS ZwDeleteWnfStateName() { return NtDeleteWnfStateName(); }
NTSTATUS NtDeleteWnfStateName() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDeleteWnfStateName, a); }
NTSTATUS ZwDirectGraphicsCall() { return NtDirectGraphicsCall(); }
NTSTATUS NtDirectGraphicsCall() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDirectGraphicsCall, a); }
NTSTATUS ZwDisplayString() { return NtDisplayString(); }
NTSTATUS NtDisplayString() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDisplayString, a); }
NTSTATUS ZwDrawText() { return NtDrawText(); }
NTSTATUS NtDrawText() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDrawText, a); }
NTSTATUS ZwDuplicateObject() { return NtDuplicateObject(); }
NTSTATUS NtDuplicateObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDuplicateObject, a); }
NTSTATUS ZwDuplicateToken() { return NtDuplicateToken(); }
NTSTATUS NtDuplicateToken() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtDuplicateToken, a); }
NTSTATUS ZwEnumerateKey() { return NtEnumerateKey(); }
NTSTATUS NtEnumerateKey() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtEnumerateKey, a); }
NTSTATUS ZwEnumerateSystemEnvironmentValuesEx() { return NtEnumerateSystemEnvironmentValuesEx(); }
NTSTATUS NtEnumerateSystemEnvironmentValuesEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtEnumerateSystemEnvironmentValuesEx, a); }
NTSTATUS ZwEnumerateTransactionObject() { return NtEnumerateTransactionObject(); }
NTSTATUS NtEnumerateTransactionObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtEnumerateTransactionObject, a); }
NTSTATUS ZwEnumerateValueKey() { return NtEnumerateValueKey(); }
NTSTATUS NtEnumerateValueKey() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtEnumerateValueKey, a); }
NTSTATUS ZwFilterBootOption() { return NtFilterBootOption(); }
NTSTATUS NtFilterBootOption() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtFilterBootOption, a); }
NTSTATUS ZwFilterToken() { return NtFilterToken(); }
NTSTATUS NtFilterToken() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtFilterToken, a); }
NTSTATUS ZwFilterTokenEx() { return NtFilterTokenEx(); }
NTSTATUS NtFilterTokenEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtFilterTokenEx, a); }
NTSTATUS ZwFindAtom() { return NtFindAtom(); }
NTSTATUS NtFindAtom() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtFindAtom, a); }
NTSTATUS ZwFlushInstallUILanguage() { return NtFlushInstallUILanguage(); }
NTSTATUS NtFlushInstallUILanguage() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtFlushInstallUILanguage, a); }
NTSTATUS ZwFlushKey() { return NtFlushKey(); }
NTSTATUS NtFlushKey() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtFlushKey, a); }
NTSTATUS ZwFlushWriteBuffer() { return NtFlushWriteBuffer(); }
NTSTATUS NtFlushWriteBuffer() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtFlushWriteBuffer, a); }
NTSTATUS ZwFreeUserPhysicalPages() { return NtFreeUserPhysicalPages(); }
NTSTATUS NtFreeUserPhysicalPages() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtFreeUserPhysicalPages, a); }
NTSTATUS ZwFreezeRegistry() { return NtFreezeRegistry(); }
NTSTATUS NtFreezeRegistry() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtFreezeRegistry, a); }
NTSTATUS ZwFreezeTransactions() { return NtFreezeTransactions(); }
NTSTATUS NtFreezeTransactions() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtFreezeTransactions, a); }
NTSTATUS ZwGetCompleteWnfStateSubscription() { return NtGetCompleteWnfStateSubscription(); }
NTSTATUS NtGetCompleteWnfStateSubscription() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtGetCompleteWnfStateSubscription, a); }
NTSTATUS ZwGetContextThread() { return NtGetContextThread(); }
NTSTATUS NtGetContextThread() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtGetContextThread, a); }
NTSTATUS ZwGetMUIRegistryInfo() { return NtGetMUIRegistryInfo(); }
NTSTATUS NtGetMUIRegistryInfo() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtGetMUIRegistryInfo, a); }
NTSTATUS ZwGetNextProcess() { return NtGetNextProcess(); }
NTSTATUS NtGetNextProcess() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtGetNextProcess, a); }
NTSTATUS ZwGetNextThread() { return NtGetNextThread(); }
NTSTATUS NtGetNextThread() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtGetNextThread, a); }
NTSTATUS ZwGetNotificationResourceManager() { return NtGetNotificationResourceManager(); }
NTSTATUS NtGetNotificationResourceManager() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtGetNotificationResourceManager, a); }
NTSTATUS ZwImpersonateAnonymousToken() { return NtImpersonateAnonymousToken(); }
NTSTATUS NtImpersonateAnonymousToken() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtImpersonateAnonymousToken, a); }
NTSTATUS ZwImpersonateClientOfPort() { return NtImpersonateClientOfPort(); }
NTSTATUS NtImpersonateClientOfPort() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtImpersonateClientOfPort, a); }
NTSTATUS ZwImpersonateThread() { return NtImpersonateThread(); }
NTSTATUS NtImpersonateThread() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtImpersonateThread, a); }
NTSTATUS ZwInitializeEnclave() { return NtInitializeEnclave(); }
NTSTATUS NtInitializeEnclave() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtInitializeEnclave, a); }
NTSTATUS ZwInitializeNlsFiles() { return NtInitializeNlsFiles(); }
NTSTATUS NtInitializeNlsFiles() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtInitializeNlsFiles, a); }
NTSTATUS ZwInitializeRegistry() { return NtInitializeRegistry(); }
NTSTATUS NtInitializeRegistry() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtInitializeRegistry, a); }
NTSTATUS ZwLoadEnclaveData() { return NtLoadEnclaveData(); }
NTSTATUS NtLoadEnclaveData() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtLoadEnclaveData, a); }
NTSTATUS ZwLoadKey() { return NtLoadKey(); }
NTSTATUS NtLoadKey() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtLoadKey, a); }
NTSTATUS ZwLoadKey2() { return NtLoadKey2(); }
NTSTATUS NtLoadKey2() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtLoadKey2, a); }
NTSTATUS ZwLoadKey3() { return NtLoadKey3(); }
NTSTATUS NtLoadKey3() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtLoadKey3, a); }
NTSTATUS ZwLoadKeyEx() { return NtLoadKeyEx(); }
NTSTATUS NtLoadKeyEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtLoadKeyEx, a); }
NTSTATUS ZwMapCMFModule() { return NtMapCMFModule(); }
NTSTATUS NtMapCMFModule() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtMapCMFModule, a); }
NTSTATUS ZwNotifyChangeDirectoryFile() { return NtNotifyChangeDirectoryFile(); }
NTSTATUS NtNotifyChangeDirectoryFile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtNotifyChangeDirectoryFile, a); }
NTSTATUS ZwNotifyChangeDirectoryFileEx() { return NtNotifyChangeDirectoryFileEx(); }
NTSTATUS NtNotifyChangeDirectoryFileEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtNotifyChangeDirectoryFileEx, a); }
NTSTATUS ZwNotifyChangeKey() { return NtNotifyChangeKey(); }
NTSTATUS NtNotifyChangeKey() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtNotifyChangeKey, a); }
NTSTATUS ZwNotifyChangeMultipleKeys() { return NtNotifyChangeMultipleKeys(); }
NTSTATUS NtNotifyChangeMultipleKeys() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtNotifyChangeMultipleKeys, a); }
NTSTATUS ZwOpenCpuPartition() { return NtOpenCpuPartition(); }
NTSTATUS NtOpenCpuPartition() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenCpuPartition, a); }
NTSTATUS ZwOpenDirectoryObject() { return NtOpenDirectoryObject(); }
NTSTATUS NtOpenDirectoryObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenDirectoryObject, a); }
NTSTATUS ZwOpenEnlistment() { return NtOpenEnlistment(); }
NTSTATUS NtOpenEnlistment() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenEnlistment, a); }
NTSTATUS ZwOpenEventPair() { return NtOpenEventPair(); }
NTSTATUS NtOpenEventPair() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenEventPair, a); }
NTSTATUS ZwOpenIoCompletion() { return NtOpenIoCompletion(); }
NTSTATUS NtOpenIoCompletion() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenIoCompletion, a); }
NTSTATUS ZwOpenJobObject() { return NtOpenJobObject(); }
NTSTATUS NtOpenJobObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenJobObject, a); }
NTSTATUS ZwOpenKeyEx() { return NtOpenKeyEx(); }
NTSTATUS NtOpenKeyEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenKeyEx, a); }
NTSTATUS ZwOpenKeyTransacted() { return NtOpenKeyTransacted(); }
NTSTATUS NtOpenKeyTransacted() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenKeyTransacted, a); }
NTSTATUS ZwOpenKeyTransactedEx() { return NtOpenKeyTransactedEx(); }
NTSTATUS NtOpenKeyTransactedEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenKeyTransactedEx, a); }
NTSTATUS ZwOpenKeyedEvent() { return NtOpenKeyedEvent(); }
NTSTATUS NtOpenKeyedEvent() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenKeyedEvent, a); }
NTSTATUS ZwOpenMutant() { return NtOpenMutant(); }
NTSTATUS NtOpenMutant() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenMutant, a); }
NTSTATUS ZwOpenObjectAuditAlarm() { return NtOpenObjectAuditAlarm(); }
NTSTATUS NtOpenObjectAuditAlarm() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenObjectAuditAlarm, a); }
NTSTATUS ZwOpenPartition() { return NtOpenPartition(); }
NTSTATUS NtOpenPartition() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenPartition, a); }
NTSTATUS ZwOpenPrivateNamespace() { return NtOpenPrivateNamespace(); }
NTSTATUS NtOpenPrivateNamespace() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenPrivateNamespace, a); }
NTSTATUS ZwOpenProcessToken() { return NtOpenProcessToken(); }
NTSTATUS NtOpenProcessToken() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenProcessToken, a); }
NTSTATUS ZwOpenProcessTokenEx() { return NtOpenProcessTokenEx(); }
NTSTATUS NtOpenProcessTokenEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenProcessTokenEx, a); }
NTSTATUS ZwOpenRegistryTransaction() { return NtOpenRegistryTransaction(); }
NTSTATUS NtOpenRegistryTransaction() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenRegistryTransaction, a); }
NTSTATUS ZwOpenResourceManager() { return NtOpenResourceManager(); }
NTSTATUS NtOpenResourceManager() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenResourceManager, a); }
NTSTATUS ZwOpenSection() { return NtOpenSection(); }
NTSTATUS NtOpenSection() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenSection, a); }
NTSTATUS ZwOpenSession() { return NtOpenSession(); }
NTSTATUS NtOpenSession() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenSession, a); }
NTSTATUS ZwOpenSymbolicLinkObject() { return NtOpenSymbolicLinkObject(); }
NTSTATUS NtOpenSymbolicLinkObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenSymbolicLinkObject, a); }
NTSTATUS ZwOpenThreadToken() { return NtOpenThreadToken(); }
NTSTATUS NtOpenThreadToken() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenThreadToken, a); }
NTSTATUS ZwOpenThreadTokenEx() { return NtOpenThreadTokenEx(); }
NTSTATUS NtOpenThreadTokenEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenThreadTokenEx, a); }
NTSTATUS ZwOpenTransaction() { return NtOpenTransaction(); }
NTSTATUS NtOpenTransaction() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenTransaction, a); }
NTSTATUS ZwOpenTransactionManager() { return NtOpenTransactionManager(); }
NTSTATUS NtOpenTransactionManager() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtOpenTransactionManager, a); }
NTSTATUS ZwPlugPlayControl() { return NtPlugPlayControl(); }
NTSTATUS NtPlugPlayControl() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtPlugPlayControl, a); }
NTSTATUS ZwPrivilegeCheck() { return NtPrivilegeCheck(); }
NTSTATUS NtPrivilegeCheck() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtPrivilegeCheck, a); }
NTSTATUS ZwPrivilegeObjectAuditAlarm() { return NtPrivilegeObjectAuditAlarm(); }
NTSTATUS NtPrivilegeObjectAuditAlarm() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtPrivilegeObjectAuditAlarm, a); }
NTSTATUS ZwPrivilegedServiceAuditAlarm() { return NtPrivilegedServiceAuditAlarm(); }
NTSTATUS NtPrivilegedServiceAuditAlarm() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtPrivilegedServiceAuditAlarm, a); }
NTSTATUS ZwPssCaptureVaSpaceBulk() { return NtPssCaptureVaSpaceBulk(); }
NTSTATUS NtPssCaptureVaSpaceBulk() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtPssCaptureVaSpaceBulk, a); }
NTSTATUS ZwQueryAttributesFile() { return NtQueryAttributesFile(); }
NTSTATUS NtQueryAttributesFile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryAttributesFile, a); }
NTSTATUS ZwQueryAuxiliaryCounterFrequency() { return NtQueryAuxiliaryCounterFrequency(); }
NTSTATUS NtQueryAuxiliaryCounterFrequency() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryAuxiliaryCounterFrequency, a); }
NTSTATUS ZwQueryDebugFilterState() { return NtQueryDebugFilterState(); }
NTSTATUS NtQueryDebugFilterState() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryDebugFilterState, a); }
NTSTATUS ZwQueryDefaultLocale() { return NtQueryDefaultLocale(); }
NTSTATUS NtQueryDefaultLocale() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryDefaultLocale, a); }
NTSTATUS ZwQueryDefaultUILanguage() { return NtQueryDefaultUILanguage(); }
NTSTATUS NtQueryDefaultUILanguage() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryDefaultUILanguage, a); }
NTSTATUS ZwQueryDirectoryFile() { return NtQueryDirectoryFile(); }
NTSTATUS NtQueryDirectoryFile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryDirectoryFile, a); }
NTSTATUS ZwQueryDirectoryFileEx() { return NtQueryDirectoryFileEx(); }
NTSTATUS NtQueryDirectoryFileEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryDirectoryFileEx, a); }
NTSTATUS ZwQueryDirectoryObject() { return NtQueryDirectoryObject(); }
NTSTATUS NtQueryDirectoryObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryDirectoryObject, a); }
NTSTATUS ZwQueryDriverEntryOrder() { return NtQueryDriverEntryOrder(); }
NTSTATUS NtQueryDriverEntryOrder() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryDriverEntryOrder, a); }
NTSTATUS ZwQueryEaFile() { return NtQueryEaFile(); }
NTSTATUS NtQueryEaFile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryEaFile, a); }
NTSTATUS ZwQueryFullAttributesFile() { return NtQueryFullAttributesFile(); }
NTSTATUS NtQueryFullAttributesFile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryFullAttributesFile, a); }
NTSTATUS ZwQueryInformationAtom() { return NtQueryInformationAtom(); }
NTSTATUS NtQueryInformationAtom() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryInformationAtom, a); }
NTSTATUS ZwQueryInformationByName() { return NtQueryInformationByName(); }
NTSTATUS NtQueryInformationByName() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryInformationByName, a); }
NTSTATUS ZwQueryInformationCpuPartition() { return NtQueryInformationCpuPartition(); }
NTSTATUS NtQueryInformationCpuPartition() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryInformationCpuPartition, a); }
NTSTATUS ZwQueryInformationEnlistment() { return NtQueryInformationEnlistment(); }
NTSTATUS NtQueryInformationEnlistment() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryInformationEnlistment, a); }
NTSTATUS ZwQueryInformationJobObject() { return NtQueryInformationJobObject(); }
NTSTATUS NtQueryInformationJobObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryInformationJobObject, a); }
NTSTATUS ZwQueryInformationResourceManager() { return NtQueryInformationResourceManager(); }
NTSTATUS NtQueryInformationResourceManager() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryInformationResourceManager, a); }
NTSTATUS ZwQueryInformationToken() { return NtQueryInformationToken(); }
NTSTATUS NtQueryInformationToken() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryInformationToken, a); }
NTSTATUS ZwQueryInformationTransaction() { return NtQueryInformationTransaction(); }
NTSTATUS NtQueryInformationTransaction() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryInformationTransaction, a); }
NTSTATUS ZwQueryInformationTransactionManager() { return NtQueryInformationTransactionManager(); }
NTSTATUS NtQueryInformationTransactionManager() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryInformationTransactionManager, a); }
NTSTATUS ZwQueryInstallUILanguage() { return NtQueryInstallUILanguage(); }
NTSTATUS NtQueryInstallUILanguage() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryInstallUILanguage, a); }
NTSTATUS ZwQueryIoCompletion() { return NtQueryIoCompletion(); }
NTSTATUS NtQueryIoCompletion() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryIoCompletion, a); }
NTSTATUS ZwQueryIoRingCapabilities() { return NtQueryIoRingCapabilities(); }
NTSTATUS NtQueryIoRingCapabilities() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryIoRingCapabilities, a); }
NTSTATUS ZwQueryLicenseValue() { return NtQueryLicenseValue(); }
NTSTATUS NtQueryLicenseValue() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryLicenseValue, a); }
NTSTATUS ZwQueryMultipleValueKey() { return NtQueryMultipleValueKey(); }
NTSTATUS NtQueryMultipleValueKey() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryMultipleValueKey, a); }
NTSTATUS ZwQueryMutant() { return NtQueryMutant(); }
NTSTATUS NtQueryMutant() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryMutant, a); }
NTSTATUS ZwQueryObject() { return NtQueryObject(); }
NTSTATUS NtQueryObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryObject, a); }
NTSTATUS ZwQueryOpenSubKeys() { return NtQueryOpenSubKeys(); }
NTSTATUS NtQueryOpenSubKeys() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryOpenSubKeys, a); }
NTSTATUS ZwQueryOpenSubKeysEx() { return NtQueryOpenSubKeysEx(); }
NTSTATUS NtQueryOpenSubKeysEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryOpenSubKeysEx, a); }
NTSTATUS ZwQueryPortInformationProcess() { return NtQueryPortInformationProcess(); }
NTSTATUS NtQueryPortInformationProcess() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryPortInformationProcess, a); }
NTSTATUS ZwQueryQuotaInformationFile() { return NtQueryQuotaInformationFile(); }
NTSTATUS NtQueryQuotaInformationFile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryQuotaInformationFile, a); }
NTSTATUS ZwQuerySection() { return NtQuerySection(); }
NTSTATUS NtQuerySection() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQuerySection, a); }
NTSTATUS ZwQuerySecurityAttributesToken() { return NtQuerySecurityAttributesToken(); }
NTSTATUS NtQuerySecurityAttributesToken() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQuerySecurityAttributesToken, a); }
NTSTATUS ZwQuerySecurityObject() { return NtQuerySecurityObject(); }
NTSTATUS NtQuerySecurityObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQuerySecurityObject, a); }
NTSTATUS ZwQuerySecurityPolicy() { return NtQuerySecurityPolicy(); }
NTSTATUS NtQuerySecurityPolicy() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQuerySecurityPolicy, a); }
NTSTATUS ZwQuerySymbolicLinkObject() { return NtQuerySymbolicLinkObject(); }
NTSTATUS NtQuerySymbolicLinkObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQuerySymbolicLinkObject, a); }
NTSTATUS ZwQuerySystemEnvironmentValue() { return NtQuerySystemEnvironmentValue(); }
NTSTATUS NtQuerySystemEnvironmentValue() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQuerySystemEnvironmentValue, a); }
NTSTATUS ZwQuerySystemEnvironmentValueEx() { return NtQuerySystemEnvironmentValueEx(); }
NTSTATUS NtQuerySystemEnvironmentValueEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQuerySystemEnvironmentValueEx, a); }
NTSTATUS ZwQuerySystemInformationEx() { return NtQuerySystemInformationEx(); }
NTSTATUS NtQuerySystemInformationEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQuerySystemInformationEx, a); }
NTSTATUS ZwQuerySystemTime() { return NtQuerySystemTime(); }
NTSTATUS NtQuerySystemTime() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQuerySystemTime, a); }
NTSTATUS ZwQueryTimer() { return NtQueryTimer(); }
NTSTATUS NtQueryTimer() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryTimer, a); }
NTSTATUS ZwQueryValueKey() { return NtQueryValueKey(); }
NTSTATUS NtQueryValueKey() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryValueKey, a); }
NTSTATUS ZwQueryVolumeInformationFile() { return NtQueryVolumeInformationFile(); }
NTSTATUS NtQueryVolumeInformationFile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryVolumeInformationFile, a); }
NTSTATUS ZwQueryWnfStateData() { return NtQueryWnfStateData(); }
NTSTATUS NtQueryWnfStateData() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryWnfStateData, a); }
NTSTATUS ZwQueryWnfStateNameInformation() { return NtQueryWnfStateNameInformation(); }
NTSTATUS NtQueryWnfStateNameInformation() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueryWnfStateNameInformation, a); }
NTSTATUS ZwQueueApcThread() { return NtQueueApcThread(); }
NTSTATUS NtQueueApcThread() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueueApcThread, a); }
NTSTATUS ZwQueueApcThreadEx() { return NtQueueApcThreadEx(); }
NTSTATUS NtQueueApcThreadEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueueApcThreadEx, a); }
NTSTATUS ZwQueueApcThreadEx2() { return NtQueueApcThreadEx2(); }
NTSTATUS NtQueueApcThreadEx2() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtQueueApcThreadEx2, a); }
NTSTATUS ZwRaiseException() { return NtRaiseException(); }
NTSTATUS NtRaiseException() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRaiseException, a); }
NTSTATUS ZwRaiseHardError() { return NtRaiseHardError(); }
NTSTATUS NtRaiseHardError() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRaiseHardError, a); }
NTSTATUS ZwReadOnlyEnlistment() { return NtReadOnlyEnlistment(); }
NTSTATUS NtReadOnlyEnlistment() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtReadOnlyEnlistment, a); }
NTSTATUS ZwReadRequestData() { return NtReadRequestData(); }
NTSTATUS NtReadRequestData() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtReadRequestData, a); }
NTSTATUS ZwRegisterProtocolAddressInformation() { return NtRegisterProtocolAddressInformation(); }
NTSTATUS NtRegisterProtocolAddressInformation() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRegisterProtocolAddressInformation, a); }
NTSTATUS ZwRegisterThreadTerminatePort() { return NtRegisterThreadTerminatePort(); }
NTSTATUS NtRegisterThreadTerminatePort() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRegisterThreadTerminatePort, a); }
NTSTATUS ZwReleaseKeyedEvent() { return NtReleaseKeyedEvent(); }
NTSTATUS NtReleaseKeyedEvent() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtReleaseKeyedEvent, a); }
NTSTATUS ZwReleaseMutant() { return NtReleaseMutant(); }
NTSTATUS NtReleaseMutant() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtReleaseMutant, a); }
NTSTATUS ZwRemoveIoCompletion() { return NtRemoveIoCompletion(); }
NTSTATUS NtRemoveIoCompletion() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRemoveIoCompletion, a); }
NTSTATUS ZwRemoveIoCompletionEx() { return NtRemoveIoCompletionEx(); }
NTSTATUS NtRemoveIoCompletionEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRemoveIoCompletionEx, a); }
NTSTATUS ZwRemoveProcessDebug() { return NtRemoveProcessDebug(); }
NTSTATUS NtRemoveProcessDebug() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRemoveProcessDebug, a); }
NTSTATUS ZwRenameKey() { return NtRenameKey(); }
NTSTATUS NtRenameKey() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRenameKey, a); }
NTSTATUS ZwReplaceKey() { return NtReplaceKey(); }
NTSTATUS NtReplaceKey() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtReplaceKey, a); }
NTSTATUS ZwReplacePartitionUnit() { return NtReplacePartitionUnit(); }
NTSTATUS NtReplacePartitionUnit() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtReplacePartitionUnit, a); }
NTSTATUS ZwRequestWaitReplyPort() { return NtRequestWaitReplyPort(); }
NTSTATUS NtRequestWaitReplyPort() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRequestWaitReplyPort, a); }
NTSTATUS ZwResetWriteWatch() { return NtResetWriteWatch(); }
NTSTATUS NtResetWriteWatch() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtResetWriteWatch, a); }
NTSTATUS ZwRestoreKey() { return NtRestoreKey(); }
NTSTATUS NtRestoreKey() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRestoreKey, a); }
NTSTATUS ZwResumeProcess() { return NtResumeProcess(); }
NTSTATUS NtResumeProcess() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtResumeProcess, a); }
NTSTATUS ZwResumeThread() { return NtResumeThread(); }
NTSTATUS NtResumeThread() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtResumeThread, a); }
NTSTATUS ZwRevertContainerImpersonation() { return NtRevertContainerImpersonation(); }
NTSTATUS NtRevertContainerImpersonation() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtRevertContainerImpersonation, a); }
NTSTATUS ZwSaveKey() { return NtSaveKey(); }
NTSTATUS NtSaveKey() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSaveKey, a); }
NTSTATUS ZwSaveKeyEx() { return NtSaveKeyEx(); }
NTSTATUS NtSaveKeyEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSaveKeyEx, a); }
NTSTATUS ZwSaveMergedKeys() { return NtSaveMergedKeys(); }
NTSTATUS NtSaveMergedKeys() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSaveMergedKeys, a); }
NTSTATUS ZwSetCachedSigningLevel() { return NtSetCachedSigningLevel(); }
NTSTATUS NtSetCachedSigningLevel() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetCachedSigningLevel, a); }
NTSTATUS ZwSetCachedSigningLevel2() { return NtSetCachedSigningLevel2(); }
NTSTATUS NtSetCachedSigningLevel2() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetCachedSigningLevel2, a); }
NTSTATUS ZwSetContextThread() { return NtSetContextThread(); }
NTSTATUS NtSetContextThread() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetContextThread, a); }
NTSTATUS ZwSetDebugFilterState() { return NtSetDebugFilterState(); }
NTSTATUS NtSetDebugFilterState() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetDebugFilterState, a); }
NTSTATUS ZwSetDefaultHardErrorPort() { return NtSetDefaultHardErrorPort(); }
NTSTATUS NtSetDefaultHardErrorPort() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetDefaultHardErrorPort, a); }
NTSTATUS ZwSetDefaultLocale() { return NtSetDefaultLocale(); }
NTSTATUS NtSetDefaultLocale() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetDefaultLocale, a); }
NTSTATUS ZwSetDefaultUILanguage() { return NtSetDefaultUILanguage(); }
NTSTATUS NtSetDefaultUILanguage() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetDefaultUILanguage, a); }
NTSTATUS ZwSetDriverEntryOrder() { return NtSetDriverEntryOrder(); }
NTSTATUS NtSetDriverEntryOrder() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetDriverEntryOrder, a); }
NTSTATUS ZwSetEaFile() { return NtSetEaFile(); }
NTSTATUS NtSetEaFile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetEaFile, a); }
NTSTATUS ZwSetEventBoostPriority() { return NtSetEventBoostPriority(); }
NTSTATUS NtSetEventBoostPriority() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetEventBoostPriority, a); }
NTSTATUS ZwSetEventEx() { return NtSetEventEx(); }
NTSTATUS NtSetEventEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetEventEx, a); }
NTSTATUS ZwSetHighEventPair() { return NtSetHighEventPair(); }
NTSTATUS NtSetHighEventPair() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetHighEventPair, a); }
NTSTATUS ZwSetHighWaitLowEventPair() { return NtSetHighWaitLowEventPair(); }
NTSTATUS NtSetHighWaitLowEventPair() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetHighWaitLowEventPair, a); }
NTSTATUS ZwSetIRTimer() { return NtSetIRTimer(); }
NTSTATUS NtSetIRTimer() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetIRTimer, a); }
NTSTATUS ZwSetInformationCpuPartition() { return NtSetInformationCpuPartition(); }
NTSTATUS NtSetInformationCpuPartition() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetInformationCpuPartition, a); }
NTSTATUS ZwSetInformationDebugObject() { return NtSetInformationDebugObject(); }
NTSTATUS NtSetInformationDebugObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetInformationDebugObject, a); }
NTSTATUS ZwSetInformationEnlistment() { return NtSetInformationEnlistment(); }
NTSTATUS NtSetInformationEnlistment() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetInformationEnlistment, a); }
NTSTATUS ZwSetInformationIoRing() { return NtSetInformationIoRing(); }
NTSTATUS NtSetInformationIoRing() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetInformationIoRing, a); }
NTSTATUS ZwSetInformationJobObject() { return NtSetInformationJobObject(); }
NTSTATUS NtSetInformationJobObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetInformationJobObject, a); }
NTSTATUS ZwSetInformationKey() { return NtSetInformationKey(); }
NTSTATUS NtSetInformationKey() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetInformationKey, a); }
NTSTATUS ZwSetInformationObject() { return NtSetInformationObject(); }
NTSTATUS NtSetInformationObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetInformationObject, a); }
NTSTATUS ZwSetInformationProcess() { return NtSetInformationProcess(); }
NTSTATUS NtSetInformationProcess() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetInformationProcess, a); }
NTSTATUS ZwSetInformationResourceManager() { return NtSetInformationResourceManager(); }
NTSTATUS NtSetInformationResourceManager() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetInformationResourceManager, a); }
NTSTATUS ZwSetInformationSymbolicLink() { return NtSetInformationSymbolicLink(); }
NTSTATUS NtSetInformationSymbolicLink() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetInformationSymbolicLink, a); }
NTSTATUS ZwSetInformationThread() { return NtSetInformationThread(); }
NTSTATUS NtSetInformationThread() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetInformationThread, a); }
NTSTATUS ZwSetInformationToken() { return NtSetInformationToken(); }
NTSTATUS NtSetInformationToken() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetInformationToken, a); }
NTSTATUS ZwSetInformationTransaction() { return NtSetInformationTransaction(); }
NTSTATUS NtSetInformationTransaction() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetInformationTransaction, a); }
NTSTATUS ZwSetInformationTransactionManager() { return NtSetInformationTransactionManager(); }
NTSTATUS NtSetInformationTransactionManager() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetInformationTransactionManager, a); }
NTSTATUS ZwSetInformationVirtualMemory() { return NtSetInformationVirtualMemory(); }
NTSTATUS NtSetInformationVirtualMemory() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetInformationVirtualMemory, a); }
NTSTATUS ZwSetIoCompletion() { return NtSetIoCompletion(); }
NTSTATUS NtSetIoCompletion() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetIoCompletion, a); }
NTSTATUS ZwSetIoCompletionEx() { return NtSetIoCompletionEx(); }
NTSTATUS NtSetIoCompletionEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetIoCompletionEx, a); }
NTSTATUS ZwSetLdtEntries() { return NtSetLdtEntries(); }
NTSTATUS NtSetLdtEntries() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetLdtEntries, a); }
NTSTATUS ZwSetLowEventPair() { return NtSetLowEventPair(); }
NTSTATUS NtSetLowEventPair() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetLowEventPair, a); }
NTSTATUS ZwSetLowWaitHighEventPair() { return NtSetLowWaitHighEventPair(); }
NTSTATUS NtSetLowWaitHighEventPair() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetLowWaitHighEventPair, a); }
NTSTATUS ZwSetQuotaInformationFile() { return NtSetQuotaInformationFile(); }
NTSTATUS NtSetQuotaInformationFile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetQuotaInformationFile, a); }
NTSTATUS ZwSetSecurityObject() { return NtSetSecurityObject(); }
NTSTATUS NtSetSecurityObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetSecurityObject, a); }
NTSTATUS ZwSetSystemEnvironmentValue() { return NtSetSystemEnvironmentValue(); }
NTSTATUS NtSetSystemEnvironmentValue() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetSystemEnvironmentValue, a); }
NTSTATUS ZwSetSystemEnvironmentValueEx() { return NtSetSystemEnvironmentValueEx(); }
NTSTATUS NtSetSystemEnvironmentValueEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetSystemEnvironmentValueEx, a); }
NTSTATUS ZwSetSystemInformation() { return NtSetSystemInformation(); }
NTSTATUS NtSetSystemInformation() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetSystemInformation, a); }
NTSTATUS ZwSetSystemPowerState() { return NtSetSystemPowerState(); }
NTSTATUS NtSetSystemPowerState() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetSystemPowerState, a); }
NTSTATUS ZwSetThreadExecutionState() { return NtSetThreadExecutionState(); }
NTSTATUS NtSetThreadExecutionState() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetThreadExecutionState, a); }
NTSTATUS ZwSetTimer2() { return NtSetTimer2(); }
NTSTATUS NtSetTimer2() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetTimer2, a); }
NTSTATUS ZwSetTimerEx() { return NtSetTimerEx(); }
NTSTATUS NtSetTimerEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetTimerEx, a); }
NTSTATUS ZwSetUuidSeed() { return NtSetUuidSeed(); }
NTSTATUS NtSetUuidSeed() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetUuidSeed, a); }
NTSTATUS ZwSetValueKey() { return NtSetValueKey(); }
NTSTATUS NtSetValueKey() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetValueKey, a); }
NTSTATUS ZwSetVolumeInformationFile() { return NtSetVolumeInformationFile(); }
NTSTATUS NtSetVolumeInformationFile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetVolumeInformationFile, a); }
NTSTATUS ZwSetWnfProcessNotificationEvent() { return NtSetWnfProcessNotificationEvent(); }
NTSTATUS NtSetWnfProcessNotificationEvent() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSetWnfProcessNotificationEvent, a); }
NTSTATUS ZwStartProfile() { return NtStartProfile(); }
NTSTATUS NtStartProfile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtStartProfile, a); }
NTSTATUS ZwStopProfile() { return NtStopProfile(); }
NTSTATUS NtStopProfile() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtStopProfile, a); }
NTSTATUS ZwSubmitIoRing() { return NtSubmitIoRing(); }
NTSTATUS NtSubmitIoRing() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSubmitIoRing, a); }
NTSTATUS ZwSubscribeWnfStateChange() { return NtSubscribeWnfStateChange(); }
NTSTATUS NtSubscribeWnfStateChange() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSubscribeWnfStateChange, a); }
NTSTATUS ZwSuspendProcess() { return NtSuspendProcess(); }
NTSTATUS NtSuspendProcess() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSuspendProcess, a); }
NTSTATUS ZwSuspendThread() { return NtSuspendThread(); }
NTSTATUS NtSuspendThread() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSuspendThread, a); }
NTSTATUS ZwSystemDebugControl() { return NtSystemDebugControl(); }
NTSTATUS NtSystemDebugControl() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtSystemDebugControl, a); }
NTSTATUS ZwTerminateEnclave() { return NtTerminateEnclave(); }
NTSTATUS NtTerminateEnclave() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtTerminateEnclave, a); }
NTSTATUS ZwTerminateJobObject() { return NtTerminateJobObject(); }
NTSTATUS NtTerminateJobObject() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtTerminateJobObject, a); }
NTSTATUS ZwTestAlert() { return NtTestAlert(); }
NTSTATUS NtTestAlert() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtTestAlert, a); }
NTSTATUS ZwThawRegistry() { return NtThawRegistry(); }
NTSTATUS NtThawRegistry() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtThawRegistry, a); }
NTSTATUS ZwThawTransactions() { return NtThawTransactions(); }
NTSTATUS NtThawTransactions() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtThawTransactions, a); }
NTSTATUS ZwTraceEvent() { return NtTraceEvent(); }
NTSTATUS NtTraceEvent() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtTraceEvent, a); }
NTSTATUS ZwTranslateFilePath() { return NtTranslateFilePath(); }
NTSTATUS NtTranslateFilePath() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtTranslateFilePath, a); }
NTSTATUS ZwUmsThreadYield() { return NtUmsThreadYield(); }
NTSTATUS NtUmsThreadYield() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtUmsThreadYield, a); }
NTSTATUS ZwUnloadDriver() { return NtUnloadDriver(); }
NTSTATUS NtUnloadDriver() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtUnloadDriver, a); }
NTSTATUS ZwUnloadKey() { return NtUnloadKey(); }
NTSTATUS NtUnloadKey() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtUnloadKey, a); }
NTSTATUS ZwUnloadKey2() { return NtUnloadKey2(); }
NTSTATUS NtUnloadKey2() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtUnloadKey2, a); }
NTSTATUS ZwUnloadKeyEx() { return NtUnloadKeyEx(); }
NTSTATUS NtUnloadKeyEx() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtUnloadKeyEx, a); }
NTSTATUS ZwUnsubscribeWnfStateChange() { return NtUnsubscribeWnfStateChange(); }
NTSTATUS NtUnsubscribeWnfStateChange() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtUnsubscribeWnfStateChange, a); }
NTSTATUS ZwUpdateWnfStateData() { return NtUpdateWnfStateData(); }
NTSTATUS NtUpdateWnfStateData() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtUpdateWnfStateData, a); }
NTSTATUS ZwVdmControl() { return NtVdmControl(); }
NTSTATUS NtVdmControl() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtVdmControl, a); }
NTSTATUS ZwWaitForAlertByThreadId() { return NtWaitForAlertByThreadId(); }
NTSTATUS NtWaitForAlertByThreadId() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtWaitForAlertByThreadId, a); }
NTSTATUS ZwWaitForDebugEvent() { return NtWaitForDebugEvent(); }
NTSTATUS NtWaitForDebugEvent() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtWaitForDebugEvent, a); }
NTSTATUS ZwWaitForKeyedEvent() { return NtWaitForKeyedEvent(); }
NTSTATUS NtWaitForKeyedEvent() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtWaitForKeyedEvent, a); }
NTSTATUS ZwWaitForWorkViaWorkerFactory() { return NtWaitForWorkViaWorkerFactory(); }
NTSTATUS NtWaitForWorkViaWorkerFactory() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtWaitForWorkViaWorkerFactory, a); }
NTSTATUS ZwWaitHighEventPair() { return NtWaitHighEventPair(); }
NTSTATUS NtWaitHighEventPair() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtWaitHighEventPair, a); }
NTSTATUS ZwWaitLowEventPair() { return NtWaitLowEventPair(); }
NTSTATUS NtWaitLowEventPair() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtWaitLowEventPair, a); }
NTSTATUS ZwWriteRequestData() { return NtWriteRequestData(); }
NTSTATUS NtWriteRequestData() { long a[4]; memset(a, 0, sizeof a); return (NTSTATUS)nt_dispatch(NR_NtWriteRequestData, a); }
W32API NTSTATUS ZwAccessCheckAndAuditAlarm();
W32API NTSTATUS NtAccessCheckAndAuditAlarm();
W32API NTSTATUS ZwAccessCheckByType();
W32API NTSTATUS NtAccessCheckByType();
W32API NTSTATUS ZwAccessCheckByTypeAndAuditAlarm();
W32API NTSTATUS NtAccessCheckByTypeAndAuditAlarm();
W32API NTSTATUS ZwAccessCheckByTypeResultList();
W32API NTSTATUS NtAccessCheckByTypeResultList();
W32API NTSTATUS ZwAccessCheckByTypeResultListAndAuditAlarm();
W32API NTSTATUS NtAccessCheckByTypeResultListAndAuditAlarm();
W32API NTSTATUS ZwAccessCheckByTypeResultListAndAuditAlarmByHandle();
W32API NTSTATUS NtAccessCheckByTypeResultListAndAuditAlarmByHandle();
W32API NTSTATUS ZwAcquireCrossVmMutant();
W32API NTSTATUS NtAcquireCrossVmMutant();
W32API NTSTATUS ZwAcquireProcessActivityReference();
W32API NTSTATUS NtAcquireProcessActivityReference();
W32API NTSTATUS ZwAddAtom();
W32API NTSTATUS NtAddAtom();
W32API NTSTATUS ZwAddAtomEx();
W32API NTSTATUS NtAddAtomEx();
W32API NTSTATUS ZwAdjustGroupsToken();
W32API NTSTATUS NtAdjustGroupsToken();
W32API NTSTATUS ZwAdjustPrivilegesToken();
W32API NTSTATUS NtAdjustPrivilegesToken();
W32API NTSTATUS ZwAdjustTokenClaimsAndDeviceGroups();
W32API NTSTATUS NtAdjustTokenClaimsAndDeviceGroups();
W32API NTSTATUS ZwAlertMultipleThreadByThreadId();
W32API NTSTATUS NtAlertMultipleThreadByThreadId();
W32API NTSTATUS ZwAlertResumeThread();
W32API NTSTATUS NtAlertResumeThread();
W32API NTSTATUS ZwAlertThread();
W32API NTSTATUS NtAlertThread();
W32API NTSTATUS ZwAlertThreadByThreadId();
W32API NTSTATUS NtAlertThreadByThreadId();
W32API NTSTATUS ZwAlertThreadByThreadIdEx();
W32API NTSTATUS NtAlertThreadByThreadIdEx();
W32API NTSTATUS ZwAllocateLocallyUniqueId();
W32API NTSTATUS NtAllocateLocallyUniqueId();
W32API NTSTATUS ZwAllocateReserveObject();
W32API NTSTATUS NtAllocateReserveObject();
W32API NTSTATUS ZwAllocateUserPhysicalPages();
W32API NTSTATUS NtAllocateUserPhysicalPages();
W32API NTSTATUS ZwAllocateUserPhysicalPagesEx();
W32API NTSTATUS NtAllocateUserPhysicalPagesEx();
W32API NTSTATUS ZwAllocateUuids();
W32API NTSTATUS NtAllocateUuids();
W32API NTSTATUS ZwAlpcAcceptConnectPort();
W32API NTSTATUS NtAlpcAcceptConnectPort();
W32API NTSTATUS ZwAlpcCancelMessage();
W32API NTSTATUS NtAlpcCancelMessage();
W32API NTSTATUS ZwAlpcConnectPort();
W32API NTSTATUS NtAlpcConnectPort();
W32API NTSTATUS ZwAlpcConnectPortEx();
W32API NTSTATUS NtAlpcConnectPortEx();
W32API NTSTATUS ZwAlpcCreatePort();
W32API NTSTATUS NtAlpcCreatePort();
W32API NTSTATUS ZwAlpcCreatePortSection();
W32API NTSTATUS NtAlpcCreatePortSection();
W32API NTSTATUS ZwAlpcCreateResourceReserve();
W32API NTSTATUS NtAlpcCreateResourceReserve();
W32API NTSTATUS ZwAlpcCreateSectionView();
W32API NTSTATUS NtAlpcCreateSectionView();
W32API NTSTATUS ZwAlpcCreateSecurityContext();
W32API NTSTATUS NtAlpcCreateSecurityContext();
W32API NTSTATUS ZwAlpcDeletePortSection();
W32API NTSTATUS NtAlpcDeletePortSection();
W32API NTSTATUS ZwAlpcDeleteResourceReserve();
W32API NTSTATUS NtAlpcDeleteResourceReserve();
W32API NTSTATUS ZwAlpcDeleteSectionView();
W32API NTSTATUS NtAlpcDeleteSectionView();
W32API NTSTATUS ZwAlpcDeleteSecurityContext();
W32API NTSTATUS NtAlpcDeleteSecurityContext();
W32API NTSTATUS ZwAlpcDisconnectPort();
W32API NTSTATUS NtAlpcDisconnectPort();
W32API NTSTATUS ZwAlpcImpersonateClientContainerOfPort();
W32API NTSTATUS NtAlpcImpersonateClientContainerOfPort();
W32API NTSTATUS ZwAlpcImpersonateClientOfPort();
W32API NTSTATUS NtAlpcImpersonateClientOfPort();
W32API NTSTATUS ZwAlpcOpenSenderProcess();
W32API NTSTATUS NtAlpcOpenSenderProcess();
W32API NTSTATUS ZwAlpcOpenSenderThread();
W32API NTSTATUS NtAlpcOpenSenderThread();
W32API NTSTATUS ZwAlpcQueryInformation();
W32API NTSTATUS NtAlpcQueryInformation();
W32API NTSTATUS ZwAlpcQueryInformationMessage();
W32API NTSTATUS NtAlpcQueryInformationMessage();
W32API NTSTATUS ZwAlpcRevokeSecurityContext();
W32API NTSTATUS NtAlpcRevokeSecurityContext();
W32API NTSTATUS ZwAlpcSendWaitReceivePort();
W32API NTSTATUS NtAlpcSendWaitReceivePort();
W32API NTSTATUS ZwAlpcSetInformation();
W32API NTSTATUS NtAlpcSetInformation();
W32API NTSTATUS ZwApphelpCacheControl();
W32API NTSTATUS NtApphelpCacheControl();
W32API NTSTATUS ZwAreMappedFilesTheSame();
W32API NTSTATUS NtAreMappedFilesTheSame();
W32API NTSTATUS ZwAssignProcessToJobObject();
W32API NTSTATUS NtAssignProcessToJobObject();
W32API NTSTATUS ZwCallEnclave();
W32API NTSTATUS NtCallEnclave();
W32API NTSTATUS ZwCallbackReturn();
W32API NTSTATUS NtCallbackReturn();
W32API NTSTATUS ZwCancelIoFile();
W32API NTSTATUS NtCancelIoFile();
W32API NTSTATUS ZwCancelIoFileEx();
W32API NTSTATUS NtCancelIoFileEx();
W32API NTSTATUS ZwCancelSynchronousIoFile();
W32API NTSTATUS NtCancelSynchronousIoFile();
W32API NTSTATUS ZwCancelTimer2();
W32API NTSTATUS NtCancelTimer2();
W32API NTSTATUS ZwChangeProcessState();
W32API NTSTATUS NtChangeProcessState();
W32API NTSTATUS ZwChangeThreadState();
W32API NTSTATUS NtChangeThreadState();
W32API NTSTATUS ZwCloseObjectAuditAlarm();
W32API NTSTATUS NtCloseObjectAuditAlarm();
W32API NTSTATUS ZwCompactKeys();
W32API NTSTATUS NtCompactKeys();
W32API NTSTATUS ZwCompareObjects();
W32API NTSTATUS NtCompareObjects();
W32API NTSTATUS ZwCompareSigningLevels();
W32API NTSTATUS NtCompareSigningLevels();
W32API NTSTATUS ZwCompareTokens();
W32API NTSTATUS NtCompareTokens();
W32API NTSTATUS ZwCompressKey();
W32API NTSTATUS NtCompressKey();
W32API NTSTATUS ZwContinue();
W32API NTSTATUS NtContinue();
W32API NTSTATUS ZwContinueEx();
W32API NTSTATUS NtContinueEx();
W32API NTSTATUS ZwConvertBetweenAuxiliaryCounterAndPerformanceCounter();
W32API NTSTATUS NtConvertBetweenAuxiliaryCounterAndPerformanceCounter();
W32API NTSTATUS ZwCopyFileChunk();
W32API NTSTATUS NtCopyFileChunk();
W32API NTSTATUS ZwCreateCpuPartition();
W32API NTSTATUS NtCreateCpuPartition();
W32API NTSTATUS ZwCreateCrossVmEvent();
W32API NTSTATUS NtCreateCrossVmEvent();
W32API NTSTATUS ZwCreateCrossVmMutant();
W32API NTSTATUS NtCreateCrossVmMutant();
W32API NTSTATUS ZwCreateDebugObject();
W32API NTSTATUS NtCreateDebugObject();
W32API NTSTATUS ZwCreateDirectoryObject();
W32API NTSTATUS NtCreateDirectoryObject();
W32API NTSTATUS ZwCreateDirectoryObjectEx();
W32API NTSTATUS NtCreateDirectoryObjectEx();
W32API NTSTATUS ZwCreateEnclave();
W32API NTSTATUS NtCreateEnclave();
W32API NTSTATUS ZwCreateEnlistment();
W32API NTSTATUS NtCreateEnlistment();
W32API NTSTATUS ZwCreateEventPair();
W32API NTSTATUS NtCreateEventPair();
W32API NTSTATUS ZwCreateIRTimer();
W32API NTSTATUS NtCreateIRTimer();
W32API NTSTATUS ZwCreateIoCompletion();
W32API NTSTATUS NtCreateIoCompletion();
W32API NTSTATUS ZwCreateIoRing();
W32API NTSTATUS NtCreateIoRing();
W32API NTSTATUS ZwCreateJobObject();
W32API NTSTATUS NtCreateJobObject();
W32API NTSTATUS ZwCreateJobSet();
W32API NTSTATUS NtCreateJobSet();
W32API NTSTATUS ZwCreateKeyTransacted();
W32API NTSTATUS NtCreateKeyTransacted();
W32API NTSTATUS ZwCreateKeyedEvent();
W32API NTSTATUS NtCreateKeyedEvent();
W32API NTSTATUS ZwCreateLowBoxToken();
W32API NTSTATUS NtCreateLowBoxToken();
W32API NTSTATUS ZwCreateMailslotFile();
W32API NTSTATUS NtCreateMailslotFile();
W32API NTSTATUS ZwCreateMutant();
W32API NTSTATUS NtCreateMutant();
W32API NTSTATUS ZwCreateNamedPipeFile();
W32API NTSTATUS NtCreateNamedPipeFile();
W32API NTSTATUS ZwCreatePagingFile();
W32API NTSTATUS NtCreatePagingFile();
W32API NTSTATUS ZwCreatePartition();
W32API NTSTATUS NtCreatePartition();
W32API NTSTATUS ZwCreatePrivateNamespace();
W32API NTSTATUS NtCreatePrivateNamespace();
W32API NTSTATUS ZwCreateProcessStateChange();
W32API NTSTATUS NtCreateProcessStateChange();
W32API NTSTATUS ZwCreateProfile();
W32API NTSTATUS NtCreateProfile();
W32API NTSTATUS ZwCreateProfileEx();
W32API NTSTATUS NtCreateProfileEx();
W32API NTSTATUS ZwCreateRegistryTransaction();
W32API NTSTATUS NtCreateRegistryTransaction();
W32API NTSTATUS ZwCreateResourceManager();
W32API NTSTATUS NtCreateResourceManager();
W32API NTSTATUS ZwCreateSymbolicLinkObject();
W32API NTSTATUS NtCreateSymbolicLinkObject();
W32API NTSTATUS ZwCreateThreadStateChange();
W32API NTSTATUS NtCreateThreadStateChange();
W32API NTSTATUS ZwCreateTimer2();
W32API NTSTATUS NtCreateTimer2();
W32API NTSTATUS ZwCreateToken();
W32API NTSTATUS NtCreateToken();
W32API NTSTATUS ZwCreateTokenEx();
W32API NTSTATUS NtCreateTokenEx();
W32API NTSTATUS ZwCreateTransaction();
W32API NTSTATUS NtCreateTransaction();
W32API NTSTATUS ZwCreateTransactionManager();
W32API NTSTATUS NtCreateTransactionManager();
W32API NTSTATUS ZwCreateUserProcess();
W32API NTSTATUS NtCreateUserProcess();
W32API NTSTATUS ZwCreateWnfStateName();
W32API NTSTATUS NtCreateWnfStateName();
W32API NTSTATUS ZwDebugActiveProcess();
W32API NTSTATUS NtDebugActiveProcess();
W32API NTSTATUS ZwDebugContinue();
W32API NTSTATUS NtDebugContinue();
W32API NTSTATUS ZwDeleteAtom();
W32API NTSTATUS NtDeleteAtom();
W32API NTSTATUS ZwDeleteFile();
W32API NTSTATUS NtDeleteFile();
W32API NTSTATUS ZwDeleteKey();
W32API NTSTATUS NtDeleteKey();
W32API NTSTATUS ZwDeleteObjectAuditAlarm();
W32API NTSTATUS NtDeleteObjectAuditAlarm();
W32API NTSTATUS ZwDeletePrivateNamespace();
W32API NTSTATUS NtDeletePrivateNamespace();
W32API NTSTATUS ZwDeleteValueKey();
W32API NTSTATUS NtDeleteValueKey();
W32API NTSTATUS ZwDeleteWnfStateData();
W32API NTSTATUS NtDeleteWnfStateData();
W32API NTSTATUS ZwDeleteWnfStateName();
W32API NTSTATUS NtDeleteWnfStateName();
W32API NTSTATUS ZwDirectGraphicsCall();
W32API NTSTATUS NtDirectGraphicsCall();
W32API NTSTATUS ZwDisplayString();
W32API NTSTATUS NtDisplayString();
W32API NTSTATUS ZwDrawText();
W32API NTSTATUS NtDrawText();
W32API NTSTATUS ZwDuplicateObject();
W32API NTSTATUS NtDuplicateObject();
W32API NTSTATUS ZwDuplicateToken();
W32API NTSTATUS NtDuplicateToken();
W32API NTSTATUS ZwEnumerateKey();
W32API NTSTATUS NtEnumerateKey();
W32API NTSTATUS ZwEnumerateSystemEnvironmentValuesEx();
W32API NTSTATUS NtEnumerateSystemEnvironmentValuesEx();
W32API NTSTATUS ZwEnumerateTransactionObject();
W32API NTSTATUS NtEnumerateTransactionObject();
W32API NTSTATUS ZwEnumerateValueKey();
W32API NTSTATUS NtEnumerateValueKey();
W32API NTSTATUS ZwFilterBootOption();
W32API NTSTATUS NtFilterBootOption();
W32API NTSTATUS ZwFilterToken();
W32API NTSTATUS NtFilterToken();
W32API NTSTATUS ZwFilterTokenEx();
W32API NTSTATUS NtFilterTokenEx();
W32API NTSTATUS ZwFindAtom();
W32API NTSTATUS NtFindAtom();
W32API NTSTATUS ZwFlushInstallUILanguage();
W32API NTSTATUS NtFlushInstallUILanguage();
W32API NTSTATUS ZwFlushKey();
W32API NTSTATUS NtFlushKey();
W32API NTSTATUS ZwFlushWriteBuffer();
W32API NTSTATUS NtFlushWriteBuffer();
W32API NTSTATUS ZwFreeUserPhysicalPages();
W32API NTSTATUS NtFreeUserPhysicalPages();
W32API NTSTATUS ZwFreezeRegistry();
W32API NTSTATUS NtFreezeRegistry();
W32API NTSTATUS ZwFreezeTransactions();
W32API NTSTATUS NtFreezeTransactions();
W32API NTSTATUS ZwGetCompleteWnfStateSubscription();
W32API NTSTATUS NtGetCompleteWnfStateSubscription();
W32API NTSTATUS ZwGetContextThread();
W32API NTSTATUS NtGetContextThread();
W32API NTSTATUS ZwGetMUIRegistryInfo();
W32API NTSTATUS NtGetMUIRegistryInfo();
W32API NTSTATUS ZwGetNextProcess();
W32API NTSTATUS NtGetNextProcess();
W32API NTSTATUS ZwGetNextThread();
W32API NTSTATUS NtGetNextThread();
W32API NTSTATUS ZwGetNotificationResourceManager();
W32API NTSTATUS NtGetNotificationResourceManager();
W32API NTSTATUS ZwImpersonateAnonymousToken();
W32API NTSTATUS NtImpersonateAnonymousToken();
W32API NTSTATUS ZwImpersonateClientOfPort();
W32API NTSTATUS NtImpersonateClientOfPort();
W32API NTSTATUS ZwImpersonateThread();
W32API NTSTATUS NtImpersonateThread();
W32API NTSTATUS ZwInitializeEnclave();
W32API NTSTATUS NtInitializeEnclave();
W32API NTSTATUS ZwInitializeNlsFiles();
W32API NTSTATUS NtInitializeNlsFiles();
W32API NTSTATUS ZwInitializeRegistry();
W32API NTSTATUS NtInitializeRegistry();
W32API NTSTATUS ZwLoadEnclaveData();
W32API NTSTATUS NtLoadEnclaveData();
W32API NTSTATUS ZwLoadKey();
W32API NTSTATUS NtLoadKey();
W32API NTSTATUS ZwLoadKey2();
W32API NTSTATUS NtLoadKey2();
W32API NTSTATUS ZwLoadKey3();
W32API NTSTATUS NtLoadKey3();
W32API NTSTATUS ZwLoadKeyEx();
W32API NTSTATUS NtLoadKeyEx();
W32API NTSTATUS ZwMapCMFModule();
W32API NTSTATUS NtMapCMFModule();
W32API NTSTATUS ZwNotifyChangeDirectoryFile();
W32API NTSTATUS NtNotifyChangeDirectoryFile();
W32API NTSTATUS ZwNotifyChangeDirectoryFileEx();
W32API NTSTATUS NtNotifyChangeDirectoryFileEx();
W32API NTSTATUS ZwNotifyChangeKey();
W32API NTSTATUS NtNotifyChangeKey();
W32API NTSTATUS ZwNotifyChangeMultipleKeys();
W32API NTSTATUS NtNotifyChangeMultipleKeys();
W32API NTSTATUS ZwOpenCpuPartition();
W32API NTSTATUS NtOpenCpuPartition();
W32API NTSTATUS ZwOpenDirectoryObject();
W32API NTSTATUS NtOpenDirectoryObject();
W32API NTSTATUS ZwOpenEnlistment();
W32API NTSTATUS NtOpenEnlistment();
W32API NTSTATUS ZwOpenEventPair();
W32API NTSTATUS NtOpenEventPair();
W32API NTSTATUS ZwOpenIoCompletion();
W32API NTSTATUS NtOpenIoCompletion();
W32API NTSTATUS ZwOpenJobObject();
W32API NTSTATUS NtOpenJobObject();
W32API NTSTATUS ZwOpenKeyEx();
W32API NTSTATUS NtOpenKeyEx();
W32API NTSTATUS ZwOpenKeyTransacted();
W32API NTSTATUS NtOpenKeyTransacted();
W32API NTSTATUS ZwOpenKeyTransactedEx();
W32API NTSTATUS NtOpenKeyTransactedEx();
W32API NTSTATUS ZwOpenKeyedEvent();
W32API NTSTATUS NtOpenKeyedEvent();
W32API NTSTATUS ZwOpenMutant();
W32API NTSTATUS NtOpenMutant();
W32API NTSTATUS ZwOpenObjectAuditAlarm();
W32API NTSTATUS NtOpenObjectAuditAlarm();
W32API NTSTATUS ZwOpenPartition();
W32API NTSTATUS NtOpenPartition();
W32API NTSTATUS ZwOpenPrivateNamespace();
W32API NTSTATUS NtOpenPrivateNamespace();
W32API NTSTATUS ZwOpenProcessToken();
W32API NTSTATUS NtOpenProcessToken();
W32API NTSTATUS ZwOpenProcessTokenEx();
W32API NTSTATUS NtOpenProcessTokenEx();
W32API NTSTATUS ZwOpenRegistryTransaction();
W32API NTSTATUS NtOpenRegistryTransaction();
W32API NTSTATUS ZwOpenResourceManager();
W32API NTSTATUS NtOpenResourceManager();
W32API NTSTATUS ZwOpenSection();
W32API NTSTATUS NtOpenSection();
W32API NTSTATUS ZwOpenSession();
W32API NTSTATUS NtOpenSession();
W32API NTSTATUS ZwOpenSymbolicLinkObject();
W32API NTSTATUS NtOpenSymbolicLinkObject();
W32API NTSTATUS ZwOpenThreadToken();
W32API NTSTATUS NtOpenThreadToken();
W32API NTSTATUS ZwOpenThreadTokenEx();
W32API NTSTATUS NtOpenThreadTokenEx();
W32API NTSTATUS ZwOpenTransaction();
W32API NTSTATUS NtOpenTransaction();
W32API NTSTATUS ZwOpenTransactionManager();
W32API NTSTATUS NtOpenTransactionManager();
W32API NTSTATUS ZwPlugPlayControl();
W32API NTSTATUS NtPlugPlayControl();
W32API NTSTATUS ZwPrivilegeCheck();
W32API NTSTATUS NtPrivilegeCheck();
W32API NTSTATUS ZwPrivilegeObjectAuditAlarm();
W32API NTSTATUS NtPrivilegeObjectAuditAlarm();
W32API NTSTATUS ZwPrivilegedServiceAuditAlarm();
W32API NTSTATUS NtPrivilegedServiceAuditAlarm();
W32API NTSTATUS ZwPssCaptureVaSpaceBulk();
W32API NTSTATUS NtPssCaptureVaSpaceBulk();
W32API NTSTATUS ZwQueryAttributesFile();
W32API NTSTATUS NtQueryAttributesFile();
W32API NTSTATUS ZwQueryAuxiliaryCounterFrequency();
W32API NTSTATUS NtQueryAuxiliaryCounterFrequency();
W32API NTSTATUS ZwQueryDebugFilterState();
W32API NTSTATUS NtQueryDebugFilterState();
W32API NTSTATUS ZwQueryDefaultLocale();
W32API NTSTATUS NtQueryDefaultLocale();
W32API NTSTATUS ZwQueryDefaultUILanguage();
W32API NTSTATUS NtQueryDefaultUILanguage();
W32API NTSTATUS ZwQueryDirectoryFile();
W32API NTSTATUS NtQueryDirectoryFile();
W32API NTSTATUS ZwQueryDirectoryFileEx();
W32API NTSTATUS NtQueryDirectoryFileEx();
W32API NTSTATUS ZwQueryDirectoryObject();
W32API NTSTATUS NtQueryDirectoryObject();
W32API NTSTATUS ZwQueryDriverEntryOrder();
W32API NTSTATUS NtQueryDriverEntryOrder();
W32API NTSTATUS ZwQueryEaFile();
W32API NTSTATUS NtQueryEaFile();
W32API NTSTATUS ZwQueryFullAttributesFile();
W32API NTSTATUS NtQueryFullAttributesFile();
W32API NTSTATUS ZwQueryInformationAtom();
W32API NTSTATUS NtQueryInformationAtom();
W32API NTSTATUS ZwQueryInformationByName();
W32API NTSTATUS NtQueryInformationByName();
W32API NTSTATUS ZwQueryInformationCpuPartition();
W32API NTSTATUS NtQueryInformationCpuPartition();
W32API NTSTATUS ZwQueryInformationEnlistment();
W32API NTSTATUS NtQueryInformationEnlistment();
W32API NTSTATUS ZwQueryInformationJobObject();
W32API NTSTATUS NtQueryInformationJobObject();
W32API NTSTATUS ZwQueryInformationResourceManager();
W32API NTSTATUS NtQueryInformationResourceManager();
W32API NTSTATUS ZwQueryInformationToken();
W32API NTSTATUS NtQueryInformationToken();
W32API NTSTATUS ZwQueryInformationTransaction();
W32API NTSTATUS NtQueryInformationTransaction();
W32API NTSTATUS ZwQueryInformationTransactionManager();
W32API NTSTATUS NtQueryInformationTransactionManager();
W32API NTSTATUS ZwQueryInstallUILanguage();
W32API NTSTATUS NtQueryInstallUILanguage();
W32API NTSTATUS ZwQueryIoCompletion();
W32API NTSTATUS NtQueryIoCompletion();
W32API NTSTATUS ZwQueryIoRingCapabilities();
W32API NTSTATUS NtQueryIoRingCapabilities();
W32API NTSTATUS ZwQueryLicenseValue();
W32API NTSTATUS NtQueryLicenseValue();
W32API NTSTATUS ZwQueryMultipleValueKey();
W32API NTSTATUS NtQueryMultipleValueKey();
W32API NTSTATUS ZwQueryMutant();
W32API NTSTATUS NtQueryMutant();
W32API NTSTATUS ZwQueryObject();
W32API NTSTATUS NtQueryObject();
W32API NTSTATUS ZwQueryOpenSubKeys();
W32API NTSTATUS NtQueryOpenSubKeys();
W32API NTSTATUS ZwQueryOpenSubKeysEx();
W32API NTSTATUS NtQueryOpenSubKeysEx();
W32API NTSTATUS ZwQueryPortInformationProcess();
W32API NTSTATUS NtQueryPortInformationProcess();
W32API NTSTATUS ZwQueryQuotaInformationFile();
W32API NTSTATUS NtQueryQuotaInformationFile();
W32API NTSTATUS ZwQuerySection();
W32API NTSTATUS NtQuerySection();
W32API NTSTATUS ZwQuerySecurityAttributesToken();
W32API NTSTATUS NtQuerySecurityAttributesToken();
W32API NTSTATUS ZwQuerySecurityObject();
W32API NTSTATUS NtQuerySecurityObject();
W32API NTSTATUS ZwQuerySecurityPolicy();
W32API NTSTATUS NtQuerySecurityPolicy();
W32API NTSTATUS ZwQuerySymbolicLinkObject();
W32API NTSTATUS NtQuerySymbolicLinkObject();
W32API NTSTATUS ZwQuerySystemEnvironmentValue();
W32API NTSTATUS NtQuerySystemEnvironmentValue();
W32API NTSTATUS ZwQuerySystemEnvironmentValueEx();
W32API NTSTATUS NtQuerySystemEnvironmentValueEx();
W32API NTSTATUS ZwQuerySystemInformationEx();
W32API NTSTATUS NtQuerySystemInformationEx();
W32API NTSTATUS ZwQuerySystemTime();
W32API NTSTATUS NtQuerySystemTime();
W32API NTSTATUS ZwQueryTimer();
W32API NTSTATUS NtQueryTimer();
W32API NTSTATUS ZwQueryValueKey();
W32API NTSTATUS NtQueryValueKey();
W32API NTSTATUS ZwQueryVolumeInformationFile();
W32API NTSTATUS NtQueryVolumeInformationFile();
W32API NTSTATUS ZwQueryWnfStateData();
W32API NTSTATUS NtQueryWnfStateData();
W32API NTSTATUS ZwQueryWnfStateNameInformation();
W32API NTSTATUS NtQueryWnfStateNameInformation();
W32API NTSTATUS ZwQueueApcThread();
W32API NTSTATUS NtQueueApcThread();
W32API NTSTATUS ZwQueueApcThreadEx();
W32API NTSTATUS NtQueueApcThreadEx();
W32API NTSTATUS ZwQueueApcThreadEx2();
W32API NTSTATUS NtQueueApcThreadEx2();
W32API NTSTATUS ZwRaiseException();
W32API NTSTATUS NtRaiseException();
W32API NTSTATUS ZwRaiseHardError();
W32API NTSTATUS NtRaiseHardError();
W32API NTSTATUS ZwReadOnlyEnlistment();
W32API NTSTATUS NtReadOnlyEnlistment();
W32API NTSTATUS ZwReadRequestData();
W32API NTSTATUS NtReadRequestData();
W32API NTSTATUS ZwRegisterProtocolAddressInformation();
W32API NTSTATUS NtRegisterProtocolAddressInformation();
W32API NTSTATUS ZwRegisterThreadTerminatePort();
W32API NTSTATUS NtRegisterThreadTerminatePort();
W32API NTSTATUS ZwReleaseKeyedEvent();
W32API NTSTATUS NtReleaseKeyedEvent();
W32API NTSTATUS ZwReleaseMutant();
W32API NTSTATUS NtReleaseMutant();
W32API NTSTATUS ZwRemoveIoCompletion();
W32API NTSTATUS NtRemoveIoCompletion();
W32API NTSTATUS ZwRemoveIoCompletionEx();
W32API NTSTATUS NtRemoveIoCompletionEx();
W32API NTSTATUS ZwRemoveProcessDebug();
W32API NTSTATUS NtRemoveProcessDebug();
W32API NTSTATUS ZwRenameKey();
W32API NTSTATUS NtRenameKey();
W32API NTSTATUS ZwReplaceKey();
W32API NTSTATUS NtReplaceKey();
W32API NTSTATUS ZwReplacePartitionUnit();
W32API NTSTATUS NtReplacePartitionUnit();
W32API NTSTATUS ZwRequestWaitReplyPort();
W32API NTSTATUS NtRequestWaitReplyPort();
W32API NTSTATUS ZwResetWriteWatch();
W32API NTSTATUS NtResetWriteWatch();
W32API NTSTATUS ZwRestoreKey();
W32API NTSTATUS NtRestoreKey();
W32API NTSTATUS ZwResumeProcess();
W32API NTSTATUS NtResumeProcess();
W32API NTSTATUS ZwResumeThread();
W32API NTSTATUS NtResumeThread();
W32API NTSTATUS ZwRevertContainerImpersonation();
W32API NTSTATUS NtRevertContainerImpersonation();
W32API NTSTATUS ZwSaveKey();
W32API NTSTATUS NtSaveKey();
W32API NTSTATUS ZwSaveKeyEx();
W32API NTSTATUS NtSaveKeyEx();
W32API NTSTATUS ZwSaveMergedKeys();
W32API NTSTATUS NtSaveMergedKeys();
W32API NTSTATUS ZwSetCachedSigningLevel();
W32API NTSTATUS NtSetCachedSigningLevel();
W32API NTSTATUS ZwSetCachedSigningLevel2();
W32API NTSTATUS NtSetCachedSigningLevel2();
W32API NTSTATUS ZwSetContextThread();
W32API NTSTATUS NtSetContextThread();
W32API NTSTATUS ZwSetDebugFilterState();
W32API NTSTATUS NtSetDebugFilterState();
W32API NTSTATUS ZwSetDefaultHardErrorPort();
W32API NTSTATUS NtSetDefaultHardErrorPort();
W32API NTSTATUS ZwSetDefaultLocale();
W32API NTSTATUS NtSetDefaultLocale();
W32API NTSTATUS ZwSetDefaultUILanguage();
W32API NTSTATUS NtSetDefaultUILanguage();
W32API NTSTATUS ZwSetDriverEntryOrder();
W32API NTSTATUS NtSetDriverEntryOrder();
W32API NTSTATUS ZwSetEaFile();
W32API NTSTATUS NtSetEaFile();
W32API NTSTATUS ZwSetEventBoostPriority();
W32API NTSTATUS NtSetEventBoostPriority();
W32API NTSTATUS ZwSetEventEx();
W32API NTSTATUS NtSetEventEx();
W32API NTSTATUS ZwSetHighEventPair();
W32API NTSTATUS NtSetHighEventPair();
W32API NTSTATUS ZwSetHighWaitLowEventPair();
W32API NTSTATUS NtSetHighWaitLowEventPair();
W32API NTSTATUS ZwSetIRTimer();
W32API NTSTATUS NtSetIRTimer();
W32API NTSTATUS ZwSetInformationCpuPartition();
W32API NTSTATUS NtSetInformationCpuPartition();
W32API NTSTATUS ZwSetInformationDebugObject();
W32API NTSTATUS NtSetInformationDebugObject();
W32API NTSTATUS ZwSetInformationEnlistment();
W32API NTSTATUS NtSetInformationEnlistment();
W32API NTSTATUS ZwSetInformationIoRing();
W32API NTSTATUS NtSetInformationIoRing();
W32API NTSTATUS ZwSetInformationJobObject();
W32API NTSTATUS NtSetInformationJobObject();
W32API NTSTATUS ZwSetInformationKey();
W32API NTSTATUS NtSetInformationKey();
W32API NTSTATUS ZwSetInformationObject();
W32API NTSTATUS NtSetInformationObject();
W32API NTSTATUS ZwSetInformationProcess();
W32API NTSTATUS NtSetInformationProcess();
W32API NTSTATUS ZwSetInformationResourceManager();
W32API NTSTATUS NtSetInformationResourceManager();
W32API NTSTATUS ZwSetInformationSymbolicLink();
W32API NTSTATUS NtSetInformationSymbolicLink();
W32API NTSTATUS ZwSetInformationThread();
W32API NTSTATUS NtSetInformationThread();
W32API NTSTATUS ZwSetInformationToken();
W32API NTSTATUS NtSetInformationToken();
W32API NTSTATUS ZwSetInformationTransaction();
W32API NTSTATUS NtSetInformationTransaction();
W32API NTSTATUS ZwSetInformationTransactionManager();
W32API NTSTATUS NtSetInformationTransactionManager();
W32API NTSTATUS ZwSetInformationVirtualMemory();
W32API NTSTATUS NtSetInformationVirtualMemory();
W32API NTSTATUS ZwSetIoCompletion();
W32API NTSTATUS NtSetIoCompletion();
W32API NTSTATUS ZwSetIoCompletionEx();
W32API NTSTATUS NtSetIoCompletionEx();
W32API NTSTATUS ZwSetLdtEntries();
W32API NTSTATUS NtSetLdtEntries();
W32API NTSTATUS ZwSetLowEventPair();
W32API NTSTATUS NtSetLowEventPair();
W32API NTSTATUS ZwSetLowWaitHighEventPair();
W32API NTSTATUS NtSetLowWaitHighEventPair();
W32API NTSTATUS ZwSetQuotaInformationFile();
W32API NTSTATUS NtSetQuotaInformationFile();
W32API NTSTATUS ZwSetSecurityObject();
W32API NTSTATUS NtSetSecurityObject();
W32API NTSTATUS ZwSetSystemEnvironmentValue();
W32API NTSTATUS NtSetSystemEnvironmentValue();
W32API NTSTATUS ZwSetSystemEnvironmentValueEx();
W32API NTSTATUS NtSetSystemEnvironmentValueEx();
W32API NTSTATUS ZwSetSystemInformation();
W32API NTSTATUS NtSetSystemInformation();
W32API NTSTATUS ZwSetSystemPowerState();
W32API NTSTATUS NtSetSystemPowerState();
W32API NTSTATUS ZwSetThreadExecutionState();
W32API NTSTATUS NtSetThreadExecutionState();
W32API NTSTATUS ZwSetTimer2();
W32API NTSTATUS NtSetTimer2();
W32API NTSTATUS ZwSetTimerEx();
W32API NTSTATUS NtSetTimerEx();
W32API NTSTATUS ZwSetUuidSeed();
W32API NTSTATUS NtSetUuidSeed();
W32API NTSTATUS ZwSetValueKey();
W32API NTSTATUS NtSetValueKey();
W32API NTSTATUS ZwSetVolumeInformationFile();
W32API NTSTATUS NtSetVolumeInformationFile();
W32API NTSTATUS ZwSetWnfProcessNotificationEvent();
W32API NTSTATUS NtSetWnfProcessNotificationEvent();
W32API NTSTATUS ZwStartProfile();
W32API NTSTATUS NtStartProfile();
W32API NTSTATUS ZwStopProfile();
W32API NTSTATUS NtStopProfile();
W32API NTSTATUS ZwSubmitIoRing();
W32API NTSTATUS NtSubmitIoRing();
W32API NTSTATUS ZwSubscribeWnfStateChange();
W32API NTSTATUS NtSubscribeWnfStateChange();
W32API NTSTATUS ZwSuspendProcess();
W32API NTSTATUS NtSuspendProcess();
W32API NTSTATUS ZwSuspendThread();
W32API NTSTATUS NtSuspendThread();
W32API NTSTATUS ZwSystemDebugControl();
W32API NTSTATUS NtSystemDebugControl();
W32API NTSTATUS ZwTerminateEnclave();
W32API NTSTATUS NtTerminateEnclave();
W32API NTSTATUS ZwTerminateJobObject();
W32API NTSTATUS NtTerminateJobObject();
W32API NTSTATUS ZwTestAlert();
W32API NTSTATUS NtTestAlert();
W32API NTSTATUS ZwThawRegistry();
W32API NTSTATUS NtThawRegistry();
W32API NTSTATUS ZwThawTransactions();
W32API NTSTATUS NtThawTransactions();
W32API NTSTATUS ZwTraceEvent();
W32API NTSTATUS NtTraceEvent();
W32API NTSTATUS ZwTranslateFilePath();
W32API NTSTATUS NtTranslateFilePath();
W32API NTSTATUS ZwUmsThreadYield();
W32API NTSTATUS NtUmsThreadYield();
W32API NTSTATUS ZwUnloadDriver();
W32API NTSTATUS NtUnloadDriver();
W32API NTSTATUS ZwUnloadKey();
W32API NTSTATUS NtUnloadKey();
W32API NTSTATUS ZwUnloadKey2();
W32API NTSTATUS NtUnloadKey2();
W32API NTSTATUS ZwUnloadKeyEx();
W32API NTSTATUS NtUnloadKeyEx();
W32API NTSTATUS ZwUnsubscribeWnfStateChange();
W32API NTSTATUS NtUnsubscribeWnfStateChange();
W32API NTSTATUS ZwUpdateWnfStateData();
W32API NTSTATUS NtUpdateWnfStateData();
W32API NTSTATUS ZwVdmControl();
W32API NTSTATUS NtVdmControl();
W32API NTSTATUS ZwWaitForAlertByThreadId();
W32API NTSTATUS NtWaitForAlertByThreadId();
W32API NTSTATUS ZwWaitForDebugEvent();
W32API NTSTATUS NtWaitForDebugEvent();
W32API NTSTATUS ZwWaitForKeyedEvent();
W32API NTSTATUS NtWaitForKeyedEvent();
W32API NTSTATUS ZwWaitForWorkViaWorkerFactory();
W32API NTSTATUS NtWaitForWorkViaWorkerFactory();
W32API NTSTATUS ZwWaitHighEventPair();
W32API NTSTATUS NtWaitHighEventPair();
W32API NTSTATUS ZwWaitLowEventPair();
W32API NTSTATUS NtWaitLowEventPair();
W32API NTSTATUS ZwWriteRequestData();
W32API NTSTATUS NtWriteRequestData();
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
