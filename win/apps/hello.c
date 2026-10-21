/* hello.c — freestanding Win32 console test app for the losedoze homemade
 * subsystem. Original code; proves loader + kernel32 file/console path. */
#include "w32.h"

static void print(const char *s) {
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD n;
  WriteFile(out, s, (DWORD)lstrlenA(s), &n, 0);
}

void hello_entry(void) {
  print("LOSEDOZE WIN32 APP OK\r\n");
  HANDLE f = CreateFileA("/tmp/w32hello.txt", GENERIC_WRITE, 0, 0,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  if (f && f != INVALID_HANDLE_VALUE) {
    DWORD n;
    WriteFile(f, "hello from PE via NtCreateFile\n", 31, &n, 0);
    CloseHandle(f);
  }
  /* small NT-layer exercise: delay via NtDelayExecution */
  W64S iv = -100000; /* 10ms */
  NtDelayExecution(0, &iv);
  ExitProcess(0);
}
