/* WrestleFest native port — shared definitions.
 *
 * Every constant here was confirmed against the running machine or taken from
 * MAME's src/mame/technos/ddragon3.cpp. Nothing is assumed.
 */
#ifndef WF_H
#define WF_H

#include <stdint.h>
#include <stddef.h>

/* Address map, from wwfwfest_state::main_map. */
#define WF_ROM_BASE      0x000000
#define WF_ROM_SIZE      0x080000
#define WF_FGRAM_BASE    0x080000
#define WF_FGRAM_SIZE    0x001000
#define WF_BGRAM_BASE    0x082000
#define WF_BGRAM_SIZE    0x001000
#define WF_FG0RAM_BASE   0x0C0000
#define WF_FG0RAM_SIZE   0x002000
#define WF_SPRRAM_BASE   0x0C2000
#define WF_SPRRAM_SIZE   0x004000  /* 2x the hardware 0x2000: AI-ingested
                                      clone poses emit ~27 grid records
                                      (stock ~16) — crowded scenes ran out
                                      and dropped record tails (in-game
                                      missing chunks). Extra space renders
                                      as inactive records for stock. */
#define WF_SCROLL_BASE   0x100000
#define WF_FLIP_ADDR     0x10000A
#define WF_IRQACK_BASE   0x140000
#define WF_SPRBUF_ADDR   0x140008
#define WF_SOUND_ADDR    0x14000C
#define WF_PRIORITY_ADDR 0x140011
#define WF_INPUT_BASE    0x140020
#define WF_PALETTE_BASE  0x180000
#define WF_PALETTE_SIZE  0x010000
#define WF_WORKRAM_BASE  0x1C0000
#define WF_WORKRAM_SIZE  0x004000

/* Timing, from the screen raw params 28MHz/4, 448, 0, 320, 272, 8, 248 and the
 * M68000 clock of 24MHz/2.
 *
 * Refresh is 7000000 / (448 * 272) = 57.44 Hz, NOT 60. A 60 Hz assumption runs
 * the game ~4.5% fast and cannot hold the oracle.
 */
#define WF_CPU_CLOCK       12000000
#define WF_PIXEL_CLOCK     7000000
#define WF_HTOTAL          448
#define WF_VTOTAL          272
#define WF_VISIBLE_START   8    /* first visible scanline; source of the
                                 * text layer's one-tile offset */
#define WF_VISIBLE_END     248
#define WF_SCREEN_WIDTH    320
#define WF_SCREEN_HEIGHT   240
/* Cycles per scanline = CPU_CLOCK * HTOTAL / PIXEL_CLOCK
 *                      = 12000000 * 448 / 7000000 = 768 exactly.
 * No fractional drift, so any residual timing error is within-line, not a
 * per-line budget error. */
#define WF_CYCLES_PER_LINE 768
#define WF_CYCLES_PER_FRAME (WF_CYCLES_PER_LINE * WF_VTOTAL)

/* Sample after the last scanline of the frame.
 *
 * An earlier sweep appeared to show line 248 (vblank start) was far better,
 * but that was measured against every byte, a metric dominated by dead stack
 * residue. Re-swept against LIVE state only, the ordering reverses: line 272
 * gives 241/251 identical frames and 3103 divergent bytes, while line 248
 * gives 214/251 and 81497. The lesson is that the noisy metric pointed the
 * wrong way — sample at the end of the frame. */
#define WF_SAMPLE_LINE WF_VTOTAL

/* Interrupt levels are driver-specific. WrestleFest uses IRQ2 for the raster
 * interrupt (every 16 scanlines) and IRQ3 for vblank (scanline 248) — NOT the
 * IRQ6/IRQ5 that ddragon3 uses on the same hardware. Confirmed two ways: the
 * vector table has real handlers only at IRQ2 (0x434) and IRQ3 (0x834) with a
 * bare RTE at 0x91A for IRQ5/IRQ6, and wwfwfest_irq_ack_w clears level 3 for
 * offset 0 and level 2 otherwise. */
#define WF_IRQ_RASTER       2
#define WF_IRQ_VBLANK       3
#define WF_IRQ_PERIOD_LINES 16
#define WF_VBLANK_LINE      248

typedef struct {
    uint8_t rom[WF_ROM_SIZE];
    uint8_t fg_videoram[WF_FGRAM_SIZE];
    uint8_t bg_videoram[WF_BGRAM_SIZE];
    uint8_t fg0_videoram[WF_FG0RAM_SIZE];
    uint8_t spriteram[WF_SPRRAM_SIZE];
    uint8_t spriteram_buffered[WF_SPRRAM_SIZE];
    uint8_t palette[WF_PALETTE_SIZE];
    uint8_t work_ram[WF_WORKRAM_SIZE];

    uint16_t scroll[4];
    uint16_t inputs[4];
    uint8_t  priority;
    uint8_t  flipscreen;
    uint16_t sound_latch;

    int irq_pending[8]; /* per-level assertion state; the CPU sees the highest */
    long irq_taken;     /* interrupts acknowledged by the CPU */
    long irq_acks;      /* writes to the ack registers = handlers that ran */
    long irq_asserts;   /* times a level was asserted */
    int cycle_carry;    /* cycles executed beyond the last request */
    long total_cycles;  /* cycles executed since reset */
    int scanline;
    long frame;
} wf_machine;

extern wf_machine wf;

/* Cycle-driven timing. Line and frame are functions of total_cycles,
 * not of wf_run_lines' iteration index. origin is RESET_CYCLES (40). */
extern long wf_cycle_origin;
int  wf_line_of(long cyc);
int  wf_scanline_has_irq(int line);
int  wf_irq_in_window(int cost);
void wf_sprbuf_digest_close(void);
/* Cycles the CPU owes for a natively-replaced routine, drained per scanline. */
extern long wf_cpu_stall;

int  wf_load_roms(const char *dir);
int  wf_video_verify_gfx(const char *rom_dir);
int  wf_video_pack_gfx(const char *pak_path);      /* --pack: PNG tile sets -> build/gfx.pak */   /* --verify-gfx: chips vs data/gfx-edit PNGs */

/* Rebuild the character-select tables with two extra positions (the Legion of
 * Doom pair). Patches the loaded ROM image, so it must run after
 * wf_load_roms and before wf_reset. Opt-in: every gate runs the stock
 * roster. See src/roster.c. */
int  wf_roster_extend(void);

/* Native character-select screen — a hard takeover of the 68k select routine.
 * On for --port and --mods. --68k or --rom-select turns the intercept
 * into a no-op so the
 * arcade path runs byte-for-byte (what `make exact` proves). See
 * src/select.c and docs/select-hook.md for the measured seam. */
/* C owns the whole select screen. Intercept at init (0x58FE) *before*
 * jsr $26e66 loads the stock tilemap — that load-then-overpaint is what
 * flickered. Resume at the stock routine's own jmp $ac0. */
#define WF_SELECT_ENTRY_PC  0x58FE
/* Rumble skips 0x58FE (btst #0,$1c0161 / bne $592a) and inits here. */
#define WF_SELECT_ENTRY_RUMBLE_PC  0x592A
#define WF_SELECT_RESUME_PC 0xAC0
/* C-owned main-frame loop on --port/--mods. Intercept 0xF9A (even/odd
 * subsystem list) and the 0x10E8 vblank spin. --68k is a no-op. */
#define WF_FRAME_ENTRY_PC   0xF9A
#define WF_FRAME_SPIN_PC    0x10E8
int  wf_frame_intercept(unsigned pc);
void wf_frame_report(void);
/* Stock GAME SELECT. 0x5324 is after jsr $26e66 / jsr $1f9e so the
 * two cards are already painted. 0x534A is the seated-player loop
 * (fallback if start is latched after the load). --68k and
 * exact scenarios leave the 68k menu alone. */
#define WF_MENU_ENTRY_PC    0x5324
#define WF_MENU_LOOP_PC     0x534A
/* Stock confirm tail: sets 0x1C0160 flags, wires opponent/slot
 * state (jsr $51d2 / $527e), then falls into 0x58FE / 0x592A. */
#define WF_MENU_CONFIRM_PC  0x5474
void wf_video_fg0_write(unsigned code, const uint8_t *in);
int  wf_video_load_indexed_png(const char *path, uint8_t **out, int *w, int *h);
int  wf_video_load_rgba_png(const char *path, uint8_t **rgba, int *w, int *h);
/* Paint a cell PNG at this spriteram offset (68k list order / z). */
/* Leftover cell blits with no list slot. Called after the sprite walk. */
int  wf_cell_frames_gameplay_active(void);

extern int wf_select_enabled;
int  wf_select_active(void);
void wf_select_begin(void);                 /* hook: take over, freeze the 68k */
void wf_select_run_frame(long target);      /* one native frame + cycle advance */
/* Undo a Hogan overwrite of seated +0x02. Does not write +0x22
 * in-match (that word is anim after confirm). */
void wf_select_reapply_ids(void);

/* C-owned clone id for human-row slot 0-3, or -1 if C did not seat it. */
int wf_select_owned_clone(int slot);
/* Seated or snapshotted clone for 5DAC skip. Ignores $1C007C so
 * continue keeps identity while the match flag is clear. */
int wf_select_remembered_clone(int slot);
/* Learn live CPU clone ids so a continue bout can restore them. */
void wf_select_note_live_ids(void);
/* C picker / continue: remember the CPU pair (clone 0-11). */
void wf_select_set_cpu_team(int a, int b);
/* Match over / attract: 68k owns object identity again. */
void wf_select_release_ids(void);

/* 68k is writing object +0x02. Keep walkout flags; if the identity
 * nibble/byte would become a different 0-11 clone (esp. Hogan 0),
 * force seated clone. Unowned slots: return value unchanged. */
uint16_t wf_select_filter_id_write(int slot, uint16_t value);

/* 68k is writing object +0x56. Keep high-byte control (apron/CPU);
 * force low byte to seated clone. Unowned: return value unchanged. */
uint16_t wf_select_filter_ctrl_write(int slot, uint16_t value);

extern int wf_menu_enabled;
int  wf_menu_active(void);
int  wf_menu_offer(unsigned pc);
void wf_menu_begin(void);
void wf_menu_run_frame(long target);

/* Native C attract scenes. --port/--mods; --rom-select pins 68k attract.
 * Credit+start -> GAME SELECT (0x534A / C menu), not 0x58FE. */
extern int wf_attract_enabled;
int  wf_attract_active(void);
int  wf_attract_offer(unsigned pc);
void wf_attract_begin(void);
void wf_attract_run_frame(long target);
int  wf_intro_active(void);
int  wf_intro_offer(unsigned pc);
int  wf_intro_at_spin(unsigned pc);
void wf_intro_begin(void);
void wf_intro_run_frame(long target);

/* Walk-in / stage-select aisle (ROM 0x7B70). Default off (WF_WALKIN=1).
 * Scene $1C007E = 4. Park $1C006F spin at 0x7CDE. Resume 0xC08 so
 * 68k still jsr $a654 at 0xC66. --68k is a no-op. */
#define WF_WALKIN_ENTRY_PC         0x7B70
#define WF_WALKIN_RESUME_PC        0xC08
#define WF_WALKIN_SPIN_PC          0x7CDE
#define WF_WALKIN_SPIN2_PC         0x7DFC
#define WF_WALKIN_SPIN_RUMBLE_PC   0x7E5E
extern int wf_walkin_enabled;
int  wf_walkin_active(void);
int  wf_walkin_offer(unsigned pc);
int  wf_walkin_at_spin(unsigned pc);
void wf_walkin_begin(void);
void wf_walkin_run_frame(long target);

/* --native C match. C+SDL; Musashi idle after reset. Not arcade-exact. */
int  wf_native_active(void);
void wf_native_boot(void);                  /* skip 68k attract; C select */
void wf_native_begin(void);
void wf_native_run_frame(long target);
void wf_native_emit_sprites(void);
/* Advance the raster with IRQ handlers, no 68k. */
void wf_c_run_to(long target);
/* Run 68k until PC hits a, b, or c (0 = unused), or `target` cycles. */
int  wf_c_run_cpu_until(unsigned a, unsigned b, unsigned c, long target);

void wf_match_tick(void);
void wf_match_on_pc(unsigned pc);

/* jsr $26e66 in the match-setup tail. Scene is already in $1C007F. */
#define WF_CAGE_TILEMAP_PC  0xCD4

int  wf_video_init(const char *rom_dir);
void wf_video_draw(uint32_t *pixels, int pitch);
void wf_video_latch(void);
void wf_video_sprite_log_dump(void);        /* WF_SPRITE_LOG, no-op if unset */
void wf_video_inject_sprite_tiles(unsigned base, unsigned count, const uint8_t *pixels);
uint32_t wf_video_palette_rgb(unsigned index);

/* 0xD1FC enter: queue a cell blit; a future native cycle model may skip 68k. */
int wf_sprite_emit_fs(void);
int wf_sprite_d1fc_enter(void);
/* WF_STREAM_VALIDATE summary: C decoder vs 68k compiler. No-op when unset. */
void wf_sprite_stream_validate_dump(void);
void wf_sprite_native_stream_dump(void);
/* WF_D1FC_COST=1: what the ROM sprite compiler costs per call. */
void wf_sprite_d1fc_cost_dump(void);
/* 0xD1FC RTS: close the A2 range. SPRBUF A2 is not the compiler cursor. */
void wf_sprite_d1fc_exit_a2(uint32_t a2);
void wf_sprite_d1fc_exit(void);
/* 00D212 hook: start the cost window past the 0xDCA0 prologue call. */
void wf_sprite_d1fc_body_start(void);
/* WF_D1FC_TRACE: record one compile's PC sequence. */
void wf_sprite_d1fc_trace_pc(uint32_t pc);
/* 0xDCA0 measured separately: D1FC's prologue calls it and a skip replaces it. */
void wf_sprite_dca0_enter(uint32_t return_pc);
void wf_sprite_dca0_maybe_exit(uint32_t pc);
void wf_sprite_dca0_dump(void);
/* WF_NS_FULL_CHECK=1: score ns_write_spr_full() against the 68k's record. */
void wf_sprite_ns_full_report(void);
/* The C decoder replaces 0xD1FC by default; WF_D1FC_SKIP=0 disables it. The cycle
 * cost of the call that was skipped. */
extern long wf_d1fc_skip_cost;
/* PORTED[] row for 0xD1FC: prepare() returns the exact cost or 0 to decline,
 * commit() lays down the records and advances A2. Going through PORTED[] is
 * what makes wf_recomp_call pull the new A2 back into a generated caller. */
int wf_sprite_d1fc_prepare(void);
/* 1 while a resumable 0xD1FC body is between quanta (WF_D1FC_RESUME=1). */
int wf_sprite_d1fc_in_progress(void);
/* WF_STALL_IRQ=1: service interrupts inside a stalling native body. */
int wf_stall_irq_enabled(void);
/* Release a stalling body's output as its cycles drain. */
void wf_stall_tick(int cycles);
extern long wf_idle_stall;
int wf_sprite_d1fc_commit(void);
void wf_sprite_d1fc_skip_report(void);
/* Rewrite this frame's spriteram: stickman body, keep paired sprites. */
void wf_sprite_compose_fs(void);
void wf_sprite_set_68k_fallback(int enabled);

/* Live visual diagnostic. F1 toggles what C draws, F2 toggles what the 68k
 * draws. C on, 68k off: the 68k's output is suppressed wherever C replaced
 * it, so F2 reveals the 68k's version rather than hiding anything. Surfaces
 * the 68k still solely authors stay visible in every state — that is how the
 * remaining un-converted surfaces are found. */
/* Scene tilemap composer (C form of ROM 0x26E66) — src/scene_map.c. */
void wf_scene_compose_fg(uint8_t *out, unsigned outsize);
void wf_scene_compose_bg(uint8_t *out, unsigned outsize);
/* Palette loading — C form of ROM 0x2A06 (src/pal_load.c). */
void wf_palette_load(void);
void wf_palette_latch(void);
int  wf_pal_2ad0(void);
/* Palette fade kernel — C form of ROM 0x26642 (src/fade.c).
 * 81408 cycles / 106 scanlines: not a live PORTED row. */
void wf_fade_step(uint32_t src, uint32_t dst, unsigned level);
int  wf_fade_26642(void);
int  wf_fade_26642_cost(void);
/* Faithful replacement for ROM 0x26E66, including its work-RAM residue. */
void wf_scene_run(void);
/* 0x26E1A / 0x26DAC — also the vblank jsr targets. */
void wf_scene_write_scroll_regs(void);
void wf_scene_flush_row(void);

/* IRQ3 vblank (src/vblank.c). Wrapper returns 0 (rte / PC already set);
 * the eight jsr bodies return 1 (rts). */
int wf_vblank_irq3(void);
int wf_vblank_irq3_cost(void);
int wf_vblank_b4b8(void);
int wf_vblank_b53a(void);
int wf_vblank_27012(void);
int wf_vblank_27066(void);
int wf_vblank_26e1a(void);
int wf_vblank_26dac(void);
int wf_vblank_2946a(void);
int wf_vblank_264aa(void);
int wf_vblank_b4b8_cost(void);
int wf_vblank_b53a_cost(void);
int wf_vblank_27012_cost(void);
int wf_vblank_27066_cost(void);
int wf_vblank_26e1a_cost(void);
int wf_vblank_26dac_cost(void);
int wf_vblank_2946a_cost(void);
int wf_vblank_264aa_cost(void);
/* Idle arms of IRQ3's other jsrs. Work paths decline (cost 0). */
int wf_attract_978(void);
int wf_attract_978_cost(void);
int wf_post_wake_hud(void);
int wf_post_wake_hud_cost(void);
/* IRQ2 raster (src/irq2.c). Wrapper returns 0 (rte / PC already set). */
int wf_irq2_raster(void);
int wf_irq2_cost(void);
int wf_obj_timer_tick(void);
int wf_obj_timer_tick_cost(void);
int wf_pack_attr_d4(void);
int wf_pack_attr_d4_cost(void);
int wf_cam_copy(void);
int wf_cam_copy_cost(void);
/* ROM 0x28608 overlay ID filter (src/overlay.c). Decode only, not PORTED. */
void wf_overlay_28608(void);
int wf_overlay_28608_cost(void);
int wf_overlay_2884e_run(void);
int wf_overlay_28808_run(void);
/* Body and cost in one pass: these three cannot be costed ahead of time.
 * See the comment block in src/overlay.c. Each performs its writes and
 * returns the exact cycles for the path it took. */
int wf_overlay_28b46_run(void);
int wf_overlay_288f0_run(void);
int wf_overlay_28896_run(void);
void wf_overlay_copy_182a(void);
int wf_overlay_copy_182a_cost(void);
void wf_overlay_285da(void);       /* reference decode, diagnostics only */
int wf_overlay_285da_prepare(void); /* PORTED cost slot: does the work */
int wf_overlay_285da_run(void);     /* PORTED impl slot: commits */
int wf_credit_55a(void);
int wf_credit_55a_cost(void);
/* Short 0xF9A callees (src/even_frame.c). Each returns 1 (rts). */
int wf_even_2983c(void);
int wf_even_2983c_cost(void);
int wf_even_298ec(void);
int wf_even_298ec_cost(void);
int wf_even_514e(void);
int wf_even_514e_cost(void);
int wf_even_26936(void);
int wf_even_26936_cost(void);
int wf_even_8b3a(void);
int wf_even_8b3a_cost(void);
int wf_even_10e6a(void);
int wf_even_10e6a_cost(void);
int wf_frame_21b4(void);
int wf_frame_21b4_cost(void);
int wf_even_bb14(void);
int wf_even_bb14_cost(void);
int wf_even_8ecc(void);
int wf_even_8ecc_cost(void);
int wf_even_24e58(void);
int wf_even_24e58_cost(void);
int wf_odd_c04a(void);
int wf_odd_c04a_cost(void);
int wf_even_fdee(void);
int wf_even_fdee_cost(void);
int wf_even_8cf8(void);
int wf_even_8cf8_cost(void);
int wf_even_7366(void);
int wf_even_7366_cost(void);
int wf_even_899c(void);
int wf_even_899c_cost(void);
int wf_even_9052(void);
int wf_even_9052_cost(void);
int wf_even_18c4(void);
int wf_even_18c4_cost(void);
int wf_even_7a00(void);
int wf_even_7a00_cost(void);
int wf_even_8710(void);
int wf_even_8710_cost(void);
int wf_even_2088e(void);
int wf_even_2088e_cost(void);
int wf_even_88a2(void);
int wf_even_88a2_cost(void);
int wf_even_262d2(void);
int wf_even_262d2_cost(void);
int wf_even_24062(void);
int wf_even_24062_cost(void);
int wf_even_f560(void);
int wf_even_f560_cost(void);
int wf_even_20958(void);
int wf_even_20958_cost(void);
int wf_even_1f914(void);
int wf_even_1f914_cost(void);
int wf_even_10120(void);
int wf_even_10120_cost(void);
int wf_even_f18a(void);
int wf_even_f18a_cost(void);
/* DE86 DF3A helper (src/frame_e002.c). Not a list PC.
 * ccr_io is SR low 5 (XNZVC) in/out. 1 = all-13 miss ($DFF8). 0 = HIT. */
int wf_de86_e002_would_hit(uint32_t a0);
int wf_de86_e002_hit11_ff(uint32_t a0);
int wf_de86_e002_hit11_ok(uint32_t a0);
int wf_de86_e002_chain(int *n, uint32_t a0, unsigned *ccr_io, int apply);
int wf_de86_e002(void);
int wf_de86_e002_cost(void);
/* $E958 AABB leaf (src/frame_e958.c). Not a list PC. *c = CCR C. */
int wf_e958(void);
int wf_e958_cost(void);
int wf_e958_run(int apply, uint32_t *d0, uint32_t a0, uint32_t a1, unsigned *c);
/* ROM 0x1C93E AF=0 + $1E2F6 $1E332 RTS (src/frame_1c93e.c). Not a list PC. */
int wf_even_1c93e(void);
int wf_even_1c93e_cost(void);
int wf_1dfe2_e2f6(void);
int wf_1dfe2_e2f6_cost(void);
int wf_1de66(void);
int wf_1de66_cost(void);
int wf_floor_2818e(void);
int wf_floor_2818e_cost(void);
int wf_floor_28288(void);
int wf_floor_28288_cost(void);
int wf_floor_2831c(void);
int wf_floor_2831c_cost(void);
/* ROM 0x1C150 both-list object tick (src/frame_1c150.c). */
int wf_even_1c150(void);
int wf_even_1c150_cost(void);
void wf_1c150_miss_report(void);
void wf_1dfe2_miss_report(void);
void wf_f18a_miss_report(void);
/* Odd-frame-only 0xF9A callees (src/odd_frame.c). Each returns 1 (rts). */
int wf_odd_26a42(void);
int wf_odd_26a42_cost(void);
int wf_odd_7548(void);
int wf_odd_7548_cost(void);
int wf_odd_bcfe(void);
int wf_odd_bcfe_cost(void);

/* Tile/grid address calculators feeding the scene composer's copy pass.
 * Both leak their result in an address register: 0x26CA8 in A3, 0x26D56 in
 * A4 -- that leak IS the return value, not a bug. */
int wf_re_026ca8_decode(void);
int wf_re_026ca8_cost(int mode_bit7_set);
int wf_tile_addr_26ca8_cost(void);
int wf_grid_addr_26d56(void);
int wf_grid_addr_26d56_cost(void);
/* Palette VRAM write-out (src/vram_writeout_26752.c). 16 x 8 longs with a
 * 0x80 row stride. No movem at all: D1/A0/A1 all leak. */
int wf_vram_writeout_26752(void);
int wf_vram_writeout_26752_cost(void);
/* Priority sort (src/prio_sort_2948.c). Swaps what it rescans, so body and
 * cost are one pass and the row is prepare/commit. */
int wf_2948_run(int *cost_out);
int wf_prio_sort_2948(void);
int wf_prio_sort_2948_cost(void);

/* ROM layer wipes (src/wipe.c). Each returns 1 (rts). */
int wf_wipe_1f6c(void);  /* returns 1 */
int wf_wipe_1fde(void);
int wf_wipe_1f9e(void);
int wf_wipe_1f38(void);      /* 0x1F38: clears $1C05AC..$1C1CD4 */
int wf_wipe_1f38_cost(void);
int wf_wipe_1f6c_cost(void);
int wf_wipe_1f9e_cost(void);
int wf_wipe_1fde_cost(void);
/* C-owned tilemap shadow: renderer-side ownership of BG/FG. */
int  wf_tilemap_shadow_active(void);
void wf_tilemap_shadow_write(unsigned address, unsigned value, int is_word);
void wf_tilemap_shadow_latch(uint8_t *fg_out, uint8_t *bg_out);
void wf_tilemap_shadow_adopt(void);   /* engine: VRAM as just composed -> shadow */

extern int wf_render_c_enabled;
extern int wf_render_68k_enabled;

/* C-side display id for an object slot (0-999). -1 if stock / empty. */
void wf_ext_set_slot(int slot, int display_id);
void wf_ext_clear_slots(void);
int  wf_ext_slot_display(int slot);

/* Extended wrestler sprite registration (called after video init) */
void wf_extended_flush_sprites(void);
void wf_extended_register_sprites(int wrestler_id, unsigned tile_base, unsigned count);
int  wf_video_write_ppm(const char *path);
void wf_reset(void);
void wf_run_frame(void);
void wf_run_lines(int from, int to);
void wf_set_irq(int level, int asserted);

/* Function-at-a-time port hooks. */
void wf_instruction_hook(unsigned int pc);
void wf_ported_report(void);

/* wf_ported_dispatch outcomes. Callers must distinguish "no row here" (the
 * generated table may claim the address) from "a row exists but declined"
 * (an IRQ would land inside the atomic body, so it must be interpreted). */
#define WF_PORTED_NONE      0   /* no live row; try the generated table */
#define WF_PORTED_RETURNED  1   /* ran; popped the return address, PC = ret */
#define WF_PORTED_SET_PC    2   /* ran; set PC itself (tail jump) */
#define WF_PORTED_DECLINED  3   /* row exists but straddles an IRQ; interpret */
/* Ran a bounded quantum and is NOT finished. PC stays at the routine entry and
 * the return address stays on the stack, so the next hook re-enters the body
 * where it left off. The slice ends, which lets wf_run_lines cross the line
 * edge and service interrupts *inside* the routine — the thing an atomic row
 * cannot do, and the reason a 765k-cycle body can never be one. */
#define WF_PORTED_YIELD     4

int wf_ported_dispatch(unsigned int pc);
void wf_recomp_report(void);
void wf_profile_add(unsigned int address);
void wf_profile_report(void);
/* Histogram of per-call costs gathered in --measure-ported mode. */
void wf_measure_report(void);
/* Write-stream log. n>0 filters to those addresses; n<0 logs every write.
 * Each line is `c=<cycles> pc=<ppc> wr<n> @<addr> =<val>` so only/skip
 * runs can be cmp'd including timestamps. */
void wf_trace_writes_open(const char *path, const unsigned int *addrs, int n);
void wf_trace_writes_close(void);
/* Cycles executed since reset, including the in-flight timeslice. */
long wf_cycles_now(void);

/* ROM read tracer: env-gated bitmap of ROM addresses accessed.
 * Used for differential analysis to find per-wrestler tables.
 * Bitmap is 64KB (512KB ROM / 8 bits). Dump and clear between runs. */
void wf_rom_trace_enable(int enable);
void wf_rom_trace_dump(const char *path);
void wf_rom_trace_clear(void);
void wf_romtap_enable(const char *path);
void wf_romtap_dump(void);

/* Filesystem roster: 3-digit ids 000-999. ROM still has 12 behaviour
 * rows (0-11); C tables are 1000 long and clone_of picks the row. */
#define WF_MAX_WRESTLERS 1000
#define WF_EXTENDED_ID_START 12

void wf_extended_tables_init(void);
uint16_t wf_extended_energy_read(int wrestler_id);
void wf_extended_energy_write(int wrestler_id, uint16_t value);

extern int wf_ported_enabled;
extern int wf_ported_measure;
extern unsigned int wf_measure_address;

/* Scenarios are shared with MAME: tools/scenarios/NAME.scn is parsed by both
 * sides so inputs are driven on identical frames. */
/* 256 was enough while every scenario pressed five buttons and coasted. A
 * scenario that actually contests a grapple has to mash, which is hundreds of
 * discrete presses -- tools/scenarios/grapple.scn alone needs ~440. */
#define WF_MAX_EVENTS 2048

typedef struct {
    long frame;
    long hold;
    int  button;
} wf_event;

typedef struct {
    char     name[64];
    long     frames;
    wf_event events[WF_MAX_EVENTS];
    size_t   event_count;
} wf_scenario;

int      wf_scenario_load(wf_scenario *scenario, const char *path);
uint32_t wf_scenario_apply(const wf_scenario *scenario, long frame);

/* Shared by scenario playback and the SDL front end so one button/mask table
 * serves both. Ports are active low: idle sets every bit, pressing clears. */
void     wf_input_idle(void);
int      wf_button_index(const char *name);
void     wf_button_press(int button);

/* Oracle trace output — byte-compatible with tools/oracle.lua so that
 * tools/trace_tool.py can diff a port trace against a MAME trace. */
int  wf_trace_open(const char *path, const char *scenario);
void wf_trace_frame(long frame, uint32_t input_bits, uint32_t sp, uint32_t usp);
void wf_trace_close(void);

#endif /* WF_H */
