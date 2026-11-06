// Crash recorder.
//
// RTC slow memory survives a panic reset (not a power cut), so the boot that
// follows a crash can put the reason and the faulting address into the
// activity log (reportCrashNote() in the sketch). build.yml enables it:
//   compiler.cpp.extra_flags   = -DPANEL_CRASH_NOTE
//   compiler.c.elf.extra_flags = -Wl,--wrap=esp_panic_handler
// With the --wrap flag the system calls __wrap_esp_panic_handler first; it
// notes the facts and hands over to the real handler. Built without the
// flags, this file only provides the (never written) note.
//
// Kept out of the .ino on purpose: an extern "C" function definition in the
// sketch file confuses the Arduino prototype generator.
#include <Arduino.h>
#include "crash_note.h"

RTC_NOINIT_ATTR CrashNote crashNote;

#ifdef PANEL_CRASH_NOTE
// Leading fields of the system's panic_info_t (ESP-IDF 4.4); only these are read.
struct PanelPanicInfo {
  int core; int exception; const char *reason; const char *description;
  void *details; void *state; const void *addr; const void *frame; bool pseudo_excause;
};

extern "C" void __real_esp_panic_handler(PanelPanicInfo *info);

static void crashCopy(char *dst, const char *src, size_t n) {
  size_t i = 0;
  if (src) for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
  dst[i] = '\0';
}

extern "C" void __wrap_esp_panic_handler(PanelPanicInfo *info) {
  crashNote.magic = CRASH_NOTE_MAGIC;
  crashNote.core  = info->core;
  crashNote.pc    = (uint32_t)info->addr;
  // Exception frame layout (xtensa_context.h): exit, pc, ps, a0 -- a0 is the
  // return address, i.e. the caller of the function that faulted.
  crashNote.ra    = info->frame ? ((const uint32_t *)info->frame)[3] : 0;
  crashCopy(crashNote.reason, info->reason, sizeof(crashNote.reason));
  crashCopy(crashNote.desc, info->description, sizeof(crashNote.desc));
  __real_esp_panic_handler(info);
}
#endif
