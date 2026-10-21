/* kernel32.c — homemade kernel32 for the losedoze Win32 subsystem.
 * Public API semantics over Linux, via the ntdll handle layer for files.
 * Original code. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include "host.h"

static DWORD last_error;

DWORD GetLastError(void) { return last_error; }
void SetLastError(DWORD e) { last_error = e; }

/* ---------------- modules / imports ---------------- */
HMODULE GetModuleHandleA(LPCSTR n) {
  if (!n) return pe_module_main();
  return (HMODULE)0; /* PE-side modules: none (builtins via LoadLibrary) */
}
HMODULE GetModuleHandleW(LPCWSTR n) {
  if (!n) return pe_module_main();
  return (HMODULE)0;
}

/* builtin DLLs are registered by w32host; LoadLibrary hands out their handles */
HMODULE LoadLibraryA(LPCSTR n) { return w32host_dll(n); }
HMODULE LoadLibraryW(LPCWSTR n) {
  char u8[128];
  w16_to_utf8(n, lstrlenW(n), u8, sizeof u8);
  return w32host_dll(u8);
}
BOOL FreeLibrary(HMODULE h) { (void)h; return W32_TRUE; }

FARPROC GetProcAddress(HMODULE h, LPCSTR n) { return pe_get_export(h, n); }

/* ---------------- console ---------------- */
static W32Handle *std_out, *std_err, *std_in;

void k32_init_stdio(void) {
  if (!std_out) std_out = w32_handle_new(HK_STDOUT, 1);
  if (!std_err) std_err = w32_handle_new(HK_STDERR, 2);
  if (!std_in) std_in = w32_handle_new(HK_STDIN, 0);
}

HANDLE GetStdHandle(DWORD which) {
  k32_init_stdio();
  if (which == STD_OUTPUT_HANDLE) return (HANDLE)std_out;
  if (which == STD_ERROR_HANDLE) return (HANDLE)std_err;
  if (which == STD_INPUT_HANDLE) return (HANDLE)std_in;
  return INVALID_HANDLE_VALUE;
}

/* ---------------- files (thin layer over Nt* dispatch) ---------------- */
static void utf16_to_objname(LPCWSTR w, W16 *storage, size_t cap,
                             OBJECT_ATTRIBUTES *oa, UNICODE_STRING *us) {
  size_t n = (size_t)lstrlenW(w);
  if (n > cap - 1) n = cap - 1;
  memcpy(storage, w, n * 2);
  storage[n] = 0;
  us->Buffer = storage;
  us->Length = (W16)(n * 2);
  us->MaximumLength = (W16)(cap * 2);
  memset(oa, 0, sizeof *oa);
  oa->ObjectName = us;
  oa->Attributes = 0x40; /* OBJ_KERNEL_HANDLE-free; OBJ_CASE_INSENSITIVE */
}

HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share, void *sa,
                   DWORD disp, DWORD flags, HANDLE tmpl) {
  (void)sa; (void)share; (void)flags; (void)tmpl;
  W16 storage[600];
  OBJECT_ATTRIBUTES oa;
  UNICODE_STRING us;
  IO_STATUS_BLOCK iosb;
  HANDLE h = INVALID_HANDLE_VALUE;
  utf16_to_objname(name, storage, sizeof storage / 2, &oa, &us);
  NTSTATUS st = NtCreateFile(&h, access, &oa, &iosb, NULL,
                             FILE_ATTRIBUTE_NORMAL, share, disp, 0, NULL, 0);
  if (!NT_SUCCESS(st)) {
    SetLastError(2 /*ERROR_FILE_NOT_FOUND*/);
    return INVALID_HANDLE_VALUE;
  }
  return h;
}

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, void *sa,
                   DWORD disp, DWORD flags, HANDLE tmpl) {
  W16 w[600];
  int i = 0;
  for (; name[i] && i < 599; i++) w[i] = (W8)name[i];
  w[i] = 0;
  return CreateFileW(w, access, share, sa, disp, flags, tmpl);
}

BOOL ReadFile(HANDLE h, void *buf, DWORD len, DWORD *read, void *ov) {
  (void)ov;
  IO_STATUS_BLOCK iosb;
  NTSTATUS st = NtReadFile(h, NULL, NULL, NULL, &iosb, buf, len, NULL, NULL);
  if (read) *read = (DWORD)iosb.Information;
  return NT_SUCCESS(st);
}

BOOL WriteFile(HANDLE h, const void *buf, DWORD len, DWORD *written, void *ov) {
  (void)ov;
  IO_STATUS_BLOCK iosb;
  NTSTATUS st = NtWriteFile(h, NULL, NULL, NULL, &iosb, (void *)buf, len,
                            NULL, NULL);
  if (written) *written = (DWORD)iosb.Information;
  return NT_SUCCESS(st);
}

BOOL CloseHandle(HANDLE h) {
  if (!h || h == INVALID_HANDLE_VALUE) return W32_FALSE;
  return NT_SUCCESS(NtClose(h));
}

void ExitProcess(DWORD code) { exit((int)code); }

/* ---------------- heap ---------------- */
static W32Handle *the_heap;

HANDLE GetProcessHeap(void) {
  if (!the_heap) the_heap = w32_handle_new(HK_HEAP, -1);
  return (HANDLE)the_heap;
}

void *HeapAlloc(HANDLE heap, DWORD flags, W64 size) {
  (void)heap; (void)flags;
  void *p = malloc((size_t)size ? (size_t)size : 1);
  if (p) memset(p, 0, (size_t)size);
  return p;
}
BOOL HeapFree(HANDLE heap, DWORD flags, void *p) {
  (void)heap; (void)flags;
  free(p);
  return W32_TRUE;
}

/* ---------------- time ---------------- */
DWORD GetTickCount(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (DWORD)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
void Sleep(DWORD ms) {
  W64S iv = -(W64S)ms * 10000;
  NtDelayExecution(0, &iv);
}

/* ---------------- virtual memory ---------------- */
LPVOID VirtualAlloc(LPVOID addr, W64 size, DWORD type, DWORD prot) {
  PVOID p = addr;
  ULONG_PTR sz = (ULONG_PTR)size;
  if (!NT_SUCCESS(NtAllocateVirtualMemory(GetCurrentProcess(), &p, 0, &sz,
                                          type ? type : (MEM_COMMIT | MEM_RESERVE),
                                          prot ? prot : PAGE_READWRITE)))
    return NULL;
  return p;
}
BOOL VirtualFree(LPVOID addr, W64 size, DWORD type) {
  PVOID p = addr;
  ULONG_PTR sz = (ULONG_PTR)size;
  return NT_SUCCESS(NtFreeVirtualMemory(GetCurrentProcess(), &p, &sz, type));
}
HANDLE GetCurrentProcess(void) { return (HANDLE)(W64S)-1; }

/* ---------------- strings ---------------- */
static W16 cmd_w[1024];
static char cmd_a[1024];

void k32_set_command_line(int argc, char **argv) {
  int o = 0;
  cmd_a[0] = 0;
  for (int i = 0; i < argc; i++) {
    if (o) {
      cmd_w[o++] = ' ';
      strncat(cmd_a, " ", sizeof cmd_a - strlen(cmd_a) - 1);
    }
    for (char *s = argv[i]; *s && o < 1022; s++) cmd_w[o++] = (W8)*s;
    strncat(cmd_a, argv[i], sizeof cmd_a - strlen(cmd_a) - 1);
  }
  cmd_w[o] = 0;
}
LPWSTR GetCommandLineW(void) { return cmd_w; }
LPSTR GetCommandLineA(void) { return cmd_a; }

int lstrlenW(LPCWSTR s) {
  int n = 0;
  if (!s) return 0;
  while (s[n]) n++;
  return n;
}
int lstrlenA(LPCSTR s) { return s ? (int)strlen(s) : 0; }
LPWSTR lstrcpyW(LPWSTR d, LPCWSTR s) {
  LPWSTR o = d;
  while ((*d++ = *s++) != 0) {}
  return o;
}
LPSTR lstrcpyA(LPSTR d, LPCSTR s) { return strcpy(d, s); }

int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR w, int wn, LPSTR out,
                        int outn, LPCSTR def, BOOL *used) {
  (void)cp; (void)flags; (void)def;
  if (used) *used = W32_FALSE;
  if (wn < 0) wn = lstrlenW(w) + 1;
  if (!out || !outn) return wn; /* size query */
  int o = 0;
  for (int i = 0; i < wn && o < outn; i++) {
    unsigned c = w[i];
    if (c < 0x100) out[o++] = (char)c;
    else { out[o++] = '?'; }
  }
  return o;
}

int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s, int sn, LPWSTR out,
                        int outn) {
  (void)cp; (void)flags;
  if (sn < 0) sn = (int)strlen(s) + 1;
  if (!out || !outn) return sn;
  int o = 0;
  for (int i = 0; i < sn && o < outn; i++) out[o++] = (W8)s[i];
  return o;
}
