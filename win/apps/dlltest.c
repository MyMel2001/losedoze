/* dlltest.c — exercises PE DLLs on disk in the losedoze Win32 subsystem:
 * a static import (dllmath.dll, resolved at load time from disk) plus
 * LoadLibraryA / GetProcAddress / FreeLibrary / GetModuleHandleA. Original. */
#include "w32.h"

int dll_add(int, int);   /* static import: dllmath.dll */

typedef int (*AddFn)(int, int);
typedef const char *(*VerFn)(void);
typedef int (*CountFn)(void);

static void print(const char *s) {
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD n;
  WriteFile(out, s, (DWORD)lstrlenA(s), &n, 0);
}

void dlltest_entry(void) {
  int static_ok = dll_add(2, 3) == 5;

  HMODULE h = LoadLibraryA("dllmath.dll");
  int dyn_ok = 0;
  if (h) {
    AddFn mul = (AddFn)GetProcAddress(h, "dll_mul");
    VerFn ver = (VerFn)GetProcAddress(h, "dll_version");
    dyn_ok = mul && mul(6, 7) == 42 && ver && ver()[0] == 'd';
    FreeLibrary(h);   /* the static import keeps the refcount at 1 */
  }

  /* A module that is unloaded and mapped again starts with fresh .data: the
   * DllMain attach counter coming back to 1 proves the first one was gone. */
  HMODULE s = LoadLibraryA("dllstate.dll");
  int unload_ok = 0;
  if (s) {
    CountFn cf = (CountFn)GetProcAddress(s, "dllstate_attach_count");
    int a1 = cf ? cf() : -1;
    FreeLibrary(s);
    s = LoadLibraryA("dllstate.dll");
    cf = s ? (CountFn)GetProcAddress(s, "dllstate_attach_count") : NULL;
    int a2 = cf ? cf() : -1;
    unload_ok = a1 == 1 && a2 == 1;
    FreeLibrary(s);
  }

  int handle_ok = GetModuleHandleA("dllmath.dll") != NULL;
  int missing_ok = LoadLibraryA("no-such-library.dll") == NULL;

  char line[160];
  wsprintfA(line,
            "LOSEDOZE DLL static=%d dynamic=%d unload=%d handle=%d missing=%d\r\n",
            static_ok, dyn_ok, unload_ok, handle_ok, missing_ok);
  print(line);

  int ok = static_ok && dyn_ok && unload_ok && handle_ok && missing_ok;
  print(ok ? "LOSEDOZE DLL OK\r\n" : "LOSEDOZE DLL FAIL\r\n");
  if (ok) {
    HANDLE f = CreateFileA("/tmp/w32dllok", GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, 0);
    if (f && f != INVALID_HANDLE_VALUE) {
      DWORD n;
      WriteFile(f, "DLL\n", 4, &n, 0);
      CloseHandle(f);
    }
    ExitProcess(0);
  }
  ExitProcess(1);
}
