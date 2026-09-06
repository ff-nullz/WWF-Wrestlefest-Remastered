/* No-op stubs for hooks the reused rasteriser/composer files call into
 * the port's mod/parity machinery. The engine has no 68k, no mods layer
 * and no select takeover, so these are inert. Every stub returns the
 * "feature off" value. */
#include <stdint.h>
#include "wf.h"
#include "tbl.h"

int  wf_layer_arcade(void) { return 0; }
int  wf_menu_active(void) { return 0; }
int  wf_select_active(void) { return 0; }
int  wf_attract_active(void) { return 0; }
void wf_asset_log(const char *kind, const char *detail) { (void)kind; (void)detail; }
void wf_ext_apply_palettes(void) {}
void wf_extended_flush_sprites(void) {}
void wf_ext_fix_banners(void) {}
void wf_ext_fix_hud(void) {}
void wf_hud_redraw_native(void) {}
void wf_hud_shadow_latch(void) {}

/* Minimal memory map. scene_map.c/pal_load.c move data through the
 * m68k_{read,write}_memory accessors because inside the port they ARE the
 * bus; here they address the engine-owned wf struct directly. Only the
 * regions the reused files touch are mapped. */
static uint8_t *map(unsigned a, unsigned *lim)
{
    a &= 0xFFFFFFu;
    if (a < WF_ROM_SIZE)                        { *lim = WF_ROM_SIZE; return wf.rom + a; }
    if (a - WF_FGRAM_BASE < WF_FGRAM_SIZE)      { *lim = 0; return wf.fg_videoram + (a - WF_FGRAM_BASE); }
    if (a - WF_BGRAM_BASE < WF_BGRAM_SIZE)      { *lim = 0; return wf.bg_videoram + (a - WF_BGRAM_BASE); }
    if (a - WF_FG0RAM_BASE < WF_FG0RAM_SIZE)    { *lim = 0; return wf.fg0_videoram + (a - WF_FG0RAM_BASE); }
    if (a - WF_SPRRAM_BASE < WF_SPRRAM_SIZE)    { *lim = 0; return wf.spriteram + (a - WF_SPRRAM_BASE); }
    if (a - WF_PALETTE_BASE < WF_PALETTE_SIZE)  { *lim = 0; return wf.palette + (a - WF_PALETTE_BASE); }
    if (a - WF_WORKRAM_BASE < WF_WORKRAM_SIZE)  { *lim = 0; return wf.work_ram + (a - WF_WORKRAM_BASE); }
    return 0;
}

unsigned int m68k_read_memory_16(unsigned int a)
{
    unsigned lim;
    const uint8_t *p;
    if ((a & 0xFFFFFFu) < WF_ROM_SIZE) return tbl_ra16(a & 0xFFFFFFu);   /* data layer, not wf.rom */
    p = map(a, &lim);
    if (p) return ((unsigned)p[0] << 8) | p[1];
    if ((a & 0xFFFFF8u) == 0x100000u) return wf.scroll[(a & 7) / 2];
    return 0;
}

unsigned int m68k_read_memory_32(unsigned int a)
{
    return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2);
}

void m68k_write_memory_16(unsigned int a, unsigned int v)
{
    unsigned lim;
    uint8_t *p;
    a &= 0xFFFFFFu;
    if ((a & 0xFFFFF8u) == 0x100000u) { wf.scroll[(a & 7) / 2] = (uint16_t)v; return; }
    if (a == 0x140010u) { wf.priority = (uint8_t)v; return; }
    p = map(a, &lim);
    if (p && !lim) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
}

void m68k_write_memory_32(unsigned int a, unsigned int v)
{
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16(a + 2, v & 0xFFFF);
}

/* Render-layer switches: the engine always draws the C layer. */
int wf_render_c_enabled = 1;
int wf_render_68k_enabled = 0;
int wf_menu_enabled = 0;
int wf_intro_active(void) { return 0; }
int wf_walkin_active(void) { return 0; }
int wf_native_active(void) { return 0; }
