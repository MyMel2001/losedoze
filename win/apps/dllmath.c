/* dllmath.c — a PE DLL loaded from disk by the losedoze Win32 subsystem.
 * Exports a couple of functions plus a version string, and counts DllMain
 * attach/detach calls so dlltest can prove load/unload really happened.
 * Original code. */
#include "w32.h"

#define DLL_PROCESS_ATTACH_ 1
#define DLL_PROCESS_DETACH_ 0

static int attaches, detaches;

int dll_add(int a, int b) { return a + b; }
int dll_mul(int a, int b) { return a * b; }
const char *dll_version(void) { static const char v[] = "dllmath 1.0"; return v; }

int dll_attach_count(void) { return attaches; }
int dll_detach_count(void) { return detaches; }

BOOL dllmath_dllmain(void *hinst, W32 reason, void *reserved) {
  (void)hinst; (void)reserved;
  if (reason == DLL_PROCESS_ATTACH_) attaches++;
  else if (reason == DLL_PROCESS_DETACH_) detaches++;
  return W32_TRUE;
}
