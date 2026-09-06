/* The engine's own memory map (src/stubs.c). These carry the names the
 * reused decomp files (scene_map.c, pal_load.c, video.c) call — the 68k
 * bus API of the old port — but there is no CPU emulator here: the
 * functions write straight into the `wf` video state. */
#ifndef ENG_BUS_H
#define ENG_BUS_H
unsigned int m68k_read_memory_8(unsigned int a);
unsigned int m68k_read_memory_16(unsigned int a);
unsigned int m68k_read_memory_32(unsigned int a);
void m68k_write_memory_8(unsigned int a, unsigned int v);
void m68k_write_memory_16(unsigned int a, unsigned int v);
void m68k_write_memory_32(unsigned int a, unsigned int v);
#endif
