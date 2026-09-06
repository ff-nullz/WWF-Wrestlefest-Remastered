/* C-owned appearance path for filesystem wrestlers (ID 12+).
 *
 * Stock IDs 0-11 still run through the 68k and the ROM. Extended IDs load
 * every visible asset from data/wrestlers/ and this file draws them:
 *   - sprites.bin  → in-ring / walkout body (video.c calls wf_ext_draw_sprites)
 *   - hud.png      → energy-gauge mugshot (24x24 fg0 tiles)
 *   - sprite_palette → private 16-word palette served at a fake ROM address
 *
 * Behaviour (moves, AI, hitboxes) still clones a stock wrestler via
 * wf_rom_id(); that is the 68k bootloader half, not the look.
 */
#ifndef EXT_WRESTLER_H
#define EXT_WRESTLER_H

#include <stdint.h>

/* Fake ROM window. Palette words for wrestler N live at
 * WF_EXT_PAL_BASE + N * 32. The 68k copy loop at 0x9AAE reads them after
 * the pointer table at 0x9E66 is intercepted. */
#define WF_EXT_PAL_BASE  0x00F00000u
#define WF_EXT_PAL_STRIDE 32u

/* ROM wrestler the 68k should pretend this ID is, for tables we have not
 * ported yet. Stock IDs map to themselves. */
int wf_rom_id(int id);

/* True when this ID has filesystem sprite tiles. */
int wf_ext_has_body(int id);

/* 0xD1FC replacement: skip the ROM sprite compiler for a filesystem body.
 * Returns 1 if it handled the call (caller must RTS). */
int wf_ext_skip_rom_sprites(uint32_t object);

/* 0x9908 replacement: draw the wrestler's name into fg0 at A3 with bank D7.
 * Returns 1 if it handled the call. */
int wf_ext_draw_banner(int id, uint32_t vram, uint16_t bank);

/* Rasteriser hook: paint a procedural stick body for every live FS wrestler. */
void wf_ext_draw_sprites(uint32_t *pix, int pitch);

/* Rewrite walkout name banners for FS wrestlers. Called from the rasteriser
 * so it does not depend on a 68k PC the generated walk-in may skip. */
void wf_ext_fix_banners(void);
void wf_ext_fix_hud(void);
void wf_hud_redraw_native(void);
void wf_hud_check(void);
int wf_hud_blocks_68k_write(unsigned address);
int wf_hud_shadow_write(unsigned address, unsigned value, int is_word);
void wf_hud_shadow_latch(void);
void wf_ext_apply_palettes(void);

/* Load unpacked sprite PNGs for extras (id >= 12) into spr_pen[].
 * Stock IDs keep the gfx-edit sheets so exact stays green. */
void wf_ext_load_folder_tiles(void);

/* How many times we suppressed the ROM sprite compiler. */
extern long wf_ext_skip_count;

/* Serve one palette word from the fake ROM window, or 0xFFFFFFFF if the
 * address is not ours. */
unsigned int wf_ext_palette_read16(unsigned int address);

#endif /* EXT_WRESTLER_H */
