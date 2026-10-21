/* w32host.c — host process for the homemade Win32 subsystem.
 * Loads a PE32+ executable, binds its imports to builtin ntdll/kernel32/
 * user32/gdi32 (our own implementations over Linux), connects to the
 * loseshell Wayland compositor, and runs the app's entry point.
 * Original code. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

#include "host.h"

static PeImage *main_img;

void host_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "w32host: ");
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

HMODULE pe_module_main(void) { return main_img ? pe_module(main_img) : NULL; }

/* ---------------- builtin dll tables ---------------- */
#define NT_LIST(X) \
  X(NtCreateFile) X(NtOpenFile) X(NtReadFile) X(NtWriteFile) X(NtClose) \
  X(NtAllocateVirtualMemory) X(NtFreeVirtualMemory) X(NtDelayExecution) \
  X(NtWaitForSingleObject) X(NtQueryInformationFile) X(NtSetInformationFile) \
  X(nt_name)

#define K32_LIST(X) \
  X(GetModuleHandleA) X(GetModuleHandleW) X(GetProcAddress) X(LoadLibraryA) \
  X(LoadLibraryW) X(FreeLibrary) X(CreateFileW) X(CreateFileA) X(ReadFile) \
  X(WriteFile) X(CloseHandle) X(GetStdHandle) X(ExitProcess) \
  X(GetLastError) X(SetLastError) X(GetProcessHeap) X(HeapAlloc) \
  X(HeapFree) X(GetCurrentProcess) X(GetTickCount) X(Sleep) \
  X(GetCommandLineW) X(GetCommandLineA) X(WideCharToMultiByte) \
  X(MultiByteToWideChar) X(VirtualAlloc) X(VirtualFree) X(lstrlenW) \
  X(lstrlenA) X(lstrcpyW) X(lstrcpyA)

#define U32_LIST(X) \
  X(RegisterClassExW) X(RegisterClassA) X(CreateWindowExW) X(CreateWindowExA) \
  X(ShowWindow) X(UpdateWindow) X(DefWindowProcW) X(DefWindowProcA) \
  X(GetMessageW) X(GetMessageA) X(PeekMessageW) X(TranslateMessage) \
  X(DispatchMessageW) X(DispatchMessageA) X(PostQuitMessage) X(PostMessageW) \
  X(SendMessageW) X(BeginPaint) X(EndPaint) X(GetDC) X(ReleaseDC) \
  X(FillRect) X(InvalidateRect) X(ValidateRect) X(SetWindowTextW) \
  X(GetWindowTextW) X(GetClientRect) X(GetWindowRect) X(MessageBoxW) \
  X(MessageBoxA) X(wsprintfW) X(wsprintfA) X(LoadCursorW) X(LoadIconW) \
  X(IsWindow)

#define G32_LIST(X) \
  X(TextOutW) X(TextOutA) X(Rectangle) X(SetTextColor) X(SetBkColor) \
  X(SetBkMode) X(CreateSolidBrush) X(CreatePen) X(DeleteObject) \
  X(SelectObject) X(GetStockObject) X(MoveToEx) X(LineTo)

#define XN(n) #n,
static const char *const nt_names_[] = { NT_LIST(XN) NULL };
static const char *const k32_names_[] = { K32_LIST(XN) NULL };
static const char *const u32_names_[] = { U32_LIST(XN) NULL };
static const char *const g32_names_[] = { G32_LIST(XN) NULL };
#undef XN
#define XF(n) (void *)n,
static void *nt_fns[] = { NT_LIST(XF) NULL };
static void *k32_fns[] = { K32_LIST(XF) NULL };
static void *u32_fns[] = { U32_LIST(XF) NULL };
static void *g32_fns[] = { G32_LIST(XF) NULL };
#undef XF

/* name -> handle (both "kernel32.dll" and "kernel32" spellings) */
typedef struct { const char *name; HMODULE h; } DllAlias;
static DllAlias aliases[8];
static int nalias;

static void reg(const char *dll, const char *const *names, void **fns) {
  pe_register_dll(dll, names, fns);
  aliases[nalias].name = dll;
  aliases[nalias].h = pe_dll_handle(dll);
  nalias++;
}

HMODULE w32host_dll(LPCSTR name) {
  for (int i = 0; i < nalias; i++)
    if (!strcasecmp(aliases[i].name, name)) return aliases[i].h;
  /* try "name.dll" spelling */
  char buf[80];
  snprintf(buf, sizeof buf, "%s", name);
  size_t l = strlen(buf);
  if (l < 4 || strcasecmp(buf + l - 4, ".dll") != 0) {
    strncat(buf, ".dll", sizeof buf - strlen(buf) - 1);
    for (int i = 0; i < nalias; i++)
      if (!strcasecmp(aliases[i].name, buf)) return aliases[i].h;
  }
  return NULL;
}

/* ---------------- wayland glue ---------------- */
static void cb_configure(void *ud, WlcWin *w, int cw, int ch) {
  (void)ud;
  user32_configure_event(w, cw, ch);
}
static void cb_close(void *ud, WlcWin *w) {
  (void)ud;
  user32_close_event(w);
}
static void cb_key(void *ud, int code, int down, unsigned mods) {
  (void)ud;
  user32_key_event(code, down, mods);
}
static void cb_motion(void *ud, int x, int y) {
  (void)ud;
  user32_motion_event(x, y);
}
static void cb_button(void *ud, int button, int pressed, int x, int y) {
  (void)ud;
  user32_button_event(button, pressed, x, y);
}
static void cb_release(void *ud, WlcWin *w) {
  (void)ud;
  user32_release_event(w);
}

int main(int argc, char **argv) {
  int argi = 1;
  if (argi < argc && !strcmp(argv[argi], "-v")) {
    setenv("NT_TRACE", "1", 1);
    argi++;
  }
  if (argi >= argc) {
    fprintf(stderr, "usage: w32host [-v] <app.exe> [args...]\n");
    return 2;
  }

  nt_init();
  k32_init_stdio();
  k32_set_command_line(argc - argi, argv + argi);

  reg("ntdll.dll", nt_names_, nt_fns);
  reg("kernel32.dll", k32_names_, k32_fns);
  reg("user32.dll", u32_names_, u32_fns);
  reg("gdi32.dll", g32_names_, g32_fns);

  if (wlc_connect() == 0) {
    struct WlcCbs cb = {
        .configure = cb_configure,
        .close_req = cb_close,
        .key = cb_key,
        .motion = cb_motion,
        .button = cb_button,
        .buf_release = cb_release,
    };
    wlc_set_cbs(&cb);
    host_log("compositor connected");
  } else {
    host_log("no compositor: console-only run");
  }

  const char *err = NULL;
  main_img = pe_load(argv[argi], &err);
  if (!main_img) {
    host_log("load %s: %s", argv[argi], err ? err : "?");
    return 1;
  }
  host_log("loaded %s", pe_image_name(main_img));

  void (*entry)(void) = (void (*)(void))pe_entry(main_img);
  entry();
  host_log("entry returned");
  return 0;
}
