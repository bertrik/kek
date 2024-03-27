// (C) 2018-2023 by Folkert van Heusden
// Released under MIT license

#include "bus.h"
#include "console.h"

int disassemble(cpu *const c, console *const cnsl, const uint16_t pc, const bool instruction_only);
void debugger(console *const cnsl, bus *const b, std::atomic_uint32_t *const stop_event, const bool tracing);
void run_bic(console *const cnsl, bus *const b, std::atomic_uint32_t *const stop_event, const bool tracing, const uint16_t bic_start);
