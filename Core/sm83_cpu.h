#ifndef sm83_cpu_h
#define sm83_cpu_h
#include "gb_struct_def.h"
#include <stdint.h>

void GB_cpu_disassemble(GB_gameboy_t *gb, uint16_t pc, uint16_t count);
#ifdef GB_INTERNAL
void GB_cpu_run(GB_gameboy_t *gb);
void GB_emuka_cpu_run_stealth(GB_gameboy_t *gb, uint16_t jump_location);
#endif

#endif /* sm83_cpu_h */
