/* dllstate.c — second PE DLL, only used by dlltest's unload/reload proof: its
 * DllMain counters live in .data, so a module that was really unloaded and
 * re-mapped starts over from zero. Original code. */
#include "w32.h"

#define DLL_PROCESS_ATTACH_ 1
#define DLL_PROCESS_DETACH_ 0

static int attaches;

int dllstate_attach_count(void) { return attaches; }

BOOL dllstate_dllmain(void *hinst, W32 reason, void *reserved) {
  (void)hinst; (void)reserved;
  if (reason == DLL_PROCESS_ATTACH_) attaches++;
  else if (reason == DLL_PROCESS_DETACH_) { /* nothing to undo */ }
  return W32_TRUE;
}
