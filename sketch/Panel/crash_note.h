// Crash recorder shared state. See crash_note.cpp.
#pragma once
#include <stdint.h>

struct CrashNote {
  uint32_t magic;      // CRASH_NOTE_MAGIC when a panic has been recorded
  uint32_t pc;         // address that faulted
  uint32_t ra;         // its caller (return address)
  int      core;
  char     reason[40]; // e.g. LoadProhibited, StoreProhibited
  char     desc[88];   // e.g. "abort() was called at PC ... on core 1"
};
extern CrashNote crashNote;
static const uint32_t CRASH_NOTE_MAGIC = 0xC0FFEE42;
