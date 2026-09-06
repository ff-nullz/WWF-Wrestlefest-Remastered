/* Engine sprite path: object -> spriteram records, via the ported 0xD1FC
 * stream decoder (src/stream_decode.c, self-contained "thinker" build).
 *
 * ROM tables used, all read out of wf.rom (see docs/rom-tables.md,
 * mv_sprite_stream_ptrs / sprite_meta — 81 rows, wrestlers are 0..11):
 *   off_tab  = r16(0x38F14 + row*2)              per-row pose table offset
 *   pose_off = r16(0x38F14 + off_tab + pose*2)   0xFFFE = no such pose
 *   base     = r32(0x38FB8 + row*4)              stream base
 *
 * Spriteram record layout is the one draw_sprites() consumes
 * (src/video.c): +1 y.lo, +2..3 w1 (bit0 enable, bit1 y8, bit2 x8,
 * bit3 flipy, bit4 flipx, bits5..7 chain), +5 tile.lo, +7 tile.hi,
 * +9 bank, +11 x.lo.
 *
 * Palettes: one 16-colour body palette per wrestler at ROM 0x2F22+id*32,
 * installed at sprite palette RAM bank id (PAL_SPRITE + id*0x80); every
 * record for that wrestler uses bank id. Same scheme as the real machine
 * after 0x2AEA runs (native.c's install_body_pals).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wf.h"
#include "engine.h"
int wf_view_w(void); int wf_view_h(void);
#include "tbl.h"
extern int eng_dbgsel;
#include "stream_decode.h"

#define TAB_META    0x38F14u
#define TAB_STREAM  0x38FB8u
#define ROM_BODY    0x2F22u
#define PAL_SPRITE  0x00182000u

/* Sprite data (docs/adr-001, group base/gfx). 0x38F14 is one contiguous
 * block: 81 row words (pose-table offsets relative to 0x38F14, 0xD256),
 * the 81 stream base longs (0x38FB8, 0xD272), then the per-row pose
 * tables (0x390FC..0x3C99C, 74 distinct, 0xFFFE = no such pose, each
 * table ends on 0xFFFF). The streams run from row 0's base (0x3C99C):
 * every stream sub-pointer is either inline (kind 0) or a 16-bit offset
 * from 0x6B0B0 (kinds 2/3, +0x8000 for kind 3, 0xD46A/0xD596/0xD862), so
 * the asm gives no end — the bound is measured: WF_STREAM_SCAN=1 decodes
 * every pose of every row with every wrestler partner (128,440 decodes)
 * and the highest byte read is 0x7ED37; 0x7ED38..0x80000 is a byte-
 * doubled blob of a different kind that no stream reaches (the AI's
 * null-object pointer $7F000 lands in it). No code above 0x3C99C.
 * Body palettes: 0x2AEA copies 0x2F22 + id*32 (id = the stream's bank
 * byte, andi.l #$ff) into sprite palette RAM; 80 rows up to the fg tile
 * palettes at 0x3922. Rows 0..11 are the wrestlers (ADR rule 7: sliced
 * into data/wrestlers/NN later, hence group "wrestler"). */
static const tbl_def sprite_tables[] = {
    { "ws_body_palettes",             "wrestler", 0x2F22, 80 * 32, TK_U16, 16,
      "0x2AEA: 16-colour body palette per sprite palette id (stream bank byte): row = id, copied to 0x182000 + bank*0x80; ids 0-11 wrestlers, 0x1C portrait frame, 0x1F announcer" },
    { "sprite_row_pose_table_offsets", "base/gfx", 0x38F14, 81 * 2 + 2, TK_U16, 1,
      "0xD256/0xDA3C/0xDCA6: per sprite row (0..80) the byte offset from 0x38F14 of its pose table; 0 = no art; pad word 0xFFFF" },
    { "sprite_stream_ptrs",            "base/gfx", 0x38FB8, 81 * 4, TK_U32, 1,
      "0xD272/0xDA60: per sprite row the ROM address of its pose stream (0 = none); rows 0-11 wrestlers, 12 referee, 13+ hardware/extras" },
    { "sprite_pose_offsets",           "base/gfx", 0x390FC, 0x3C99C - 0x390FC, TK_U16, 1,
      "the per-row pose tables sprite_row_pose_table_offsets point into: word per pose = offset into the row's stream, 0xFFFE = no such pose (0xD26C)" },
    { "sprite_streams",                "base/gfx", 0x3C99C, 0x7ED38 - 0x3C99C, TK_U8, 16,
      "pose stream bytecode for every sprite row (0xD1FC decoder): row bases in sprite_stream_ptrs, shared sub-streams addressed as 0x6B0B0 + 16-bit offset (+0x8000 kind 3); end 0x7ED38 measured by WF_STREAM_SCAN (every pose of every row, highest read 0x7ED37)" },
};
TBL_REGISTER(sprite_tables)

static int thinker_ready = 0;
static unsigned extra_loaded;          /* id sitting in bank 0xD; 0 = none (reset by the body install) */
static int portrait_loaded;            /* bank 0xF holds palette id-1 (0 = none):
                                          0x1C portraits, or the demo card's own id */
static int body_pals_pending;          /* boot install waiting for the data layer */
static unsigned scene_pal_ids[16];     /* interlude 0x2AEA map (see scene_pal_bank) */
static int scene_pal_n = -1;           /* -1 = match mode (identity map) */

/* CLONE slots (engine.h ids 12..15). Sprite stream rows 12..15 belong to
 * the referee/ring hardware, so a clone travels through the emit path as a
 * VIRTUAL row 0x60+slot (>= 81, unreachable otherwise); eng_sprite_row()
 * encodes, eng_sprite_emit_pose() decodes back to the clone id + its
 * base's art row. Palette: banks 12..15 also belong to the referee/hw
 * palettes, so a clone BORROWS the bank of a stock wrestler who is not in
 * the match ($1C1610-style id->bank map; the ROM's allocator deals banks
 * on demand the same way for the front-end screens). */
#define CLONE_ROW0 0x60u
/* clone AISLE walkers: same trick, second virtual range. aisle.c asks
 * eng_sprite_aisle_row() for the walker row; the emit path decodes it to
 * clone id + the base's walker row (0x40+base, 0x4B walks as 0x4A) and
 * serves pak poses ENG_AISLE_POSE0+cell first (art-ingest carries them),
 * ROM stream fallback (docs/ai-art-pipeline.md). */
#define CLONE_AROW0 0x80u
#define CLONE_CROW0 0xA0u   /* virtual rows: a clone's CONTINUE face cards (pak poses 800+cell) */
#define CLONE_TROW0 0xC0u   /* virtual rows: a clone's TITLE card (pak pose 810) */
static uint16_t rom16(unsigned a);     /* defined below (data layer read) */
static int clone_bank[32];   /* borrowed bank per clone slot, -1 = none (reset fills) */
static uint16_t rows_busy;             /* bit b = stock id b active this frame (base ids) */
#define WPN_BANKS 4
static int wpn_bank[WPN_BANKS];        /* borrowed bank per weapon-pak palette index, -1 = none */
static void clone_banks_reset(void) { for (int k = 0; k < 32; k++) clone_bank[k] = -1; for (int k = 0; k < WPN_BANKS; k++) wpn_bank[k] = -1; }
static unsigned borrowed_banks;
static const eng_obj *cur_objs;         /* this frame's object array (emit_obj -> index) */
static int bank_taken(int b)           /* a stock bank in play, or lent to a clone / weapon palette */
{
    if (borrowed_banks & (1u << b)) return 1;   /* lent to a badge palette */
    int taken = (rows_busy >> b) & 1;
    /* a SEATED stock wrestler owns his bank for the whole match, drawn this
     * frame or not - lending it while he is off screen handed a badge /
     * clone / weapon palette his colours ("his palette is stuffed", the
     * 12-man battle royale, user 2026-09-05: every bank is in play there) */
    if (cur_objs) for (int i = 0; i < ENG_MAX_OBJS && !taken; i++)
        if (cur_objs[i].active && cur_objs[i].wrestler < ENG_WS_EXT_MAX && eng_ws_base(cur_objs[i].wrestler) == b) taken = 1;
    for (int k = 0; k < 32 && !taken; k++) if (clone_bank[k] == b) taken = 1;
    for (int k = 0; k < WPN_BANKS && !taken; k++) if (wpn_bank[k] == b) taken = 1;
    return taken;
}
static unsigned eng_sprite_row(int wrestler)   /* object row -> emit row */
{
    if (wrestler >= 12 && wrestler < ENG_WS_EXT_MAX && eng_ws_clone_base(wrestler) >= 0)
        return CLONE_ROW0 + (unsigned)(wrestler - 12);
    return (unsigned)wrestler;
}
unsigned eng_sprite_obj_row(int wrestler) { return eng_sprite_row(wrestler); }
unsigned eng_sprite_cont_row(int wrestler)     /* continue screen: row 0x30+id; a clone with card art rides a virtual row */
{
    const eng_pkg_rec *pr;
    if (wrestler >= 12 && wrestler < ENG_WS_EXT_MAX && eng_ws_clone_base(wrestler) >= 0) {
        if (eng_pkg_pose((unsigned)wrestler, ENG_CONT_POSE0, 0, -1, &pr) > 0 && eng_pkg_pose_was_own())
            return CLONE_CROW0 + (unsigned)(wrestler - 12);
        return 0x30u + (unsigned)eng_ws_base(wrestler);   /* the base's cards until his exist */
    }
    return 0x30u + (unsigned)wrestler;
}
int eng_sprite_title_row(int wrestler)         /* title card: a clone with card art, else -1 */
{
    const eng_pkg_rec *pr;
    if (wrestler >= 12 && wrestler < ENG_WS_EXT_MAX && eng_ws_clone_base(wrestler) >= 0
        && eng_pkg_pose((unsigned)wrestler, ENG_TITLE_POSE, 0, -1, &pr) > 0 && eng_pkg_pose_was_own())
        return (int)(CLONE_TROW0 + (unsigned)(wrestler - 12));
    return -1;
}
unsigned eng_sprite_aisle_row(int wrestler)    /* aisle walker -> emit row */
{
    unsigned row;
    if (wrestler >= 12 && wrestler < ENG_WS_EXT_MAX && eng_ws_clone_base(wrestler) >= 0)
        return CLONE_AROW0 + (unsigned)(wrestler - 12);
    row = 0x40u + (unsigned)wrestler;
    if (row == 0x4Bu) row--;           /* 0x8008: Crush walks as Smash */
    return row;
}
static int clone_bank_get(int clone)   /* install-on-demand borrowed bank */
{
    int s = clone - 12, b;
    if (s < 0 || s >= ENG_WS_EXT_MAX - 12) return -1;
    if (clone_bank[s] >= 0) return clone_bank[s];
    for (b = 11; b >= 0; b--)          /* highest free stock bank */
        if (!bank_taken(b)) break;
    if (b < 0) b = eng_ws_base(clone); /* everyone in play (rumble): base colours */
    else {
        const uint16_t *pk = eng_pkg_palette((unsigned)clone);   /* own pak, else the base's */
        {   /* palette-select applies to clone slots too (palettes/12.json) */
            const uint16_t *alt = eng_palsel_pens(clone);
            if (alt) pk = alt;
        }
        extern void m68k_write_memory_16(unsigned int address, unsigned int value);
        for (int pen = 0; pen < 16; pen++)
            m68k_write_memory_16(PAL_SPRITE + (unsigned)b * 0x80u + (unsigned)pen * 2u,
                                 pk ? pk[pen]
                                    : rom16(ROM_BODY + (unsigned)eng_ws_base(clone) * 32u + (unsigned)pen * 2u));
        if (eng_dbgsel)
            fprintf(stderr, "sprite: clone %d borrows bank %d\n", clone, b);
    }
    clone_bank[s] = b;
    return b;
}

static uint16_t rom16(unsigned a)
{
    return (uint16_t)(((unsigned)tbl_ra8(a) << 8) | tbl_ra8(a + 1));
}
/* WEAPON palette bank (2026-08-27): the weapon cells (nibble 15) draw in
 * bank 0xF = body-palette row 15 (install_body_palettes). Palette index 0
 * of the weapon pak IS that row unless a mod changed it; any other index
 * (a type's own palette) or a changed row borrows a free stock bank like a
 * clone does. */
/* a palette that is not a wrestler's: borrow a free stock bank (badges) */
int eng_sprite_borrow_bank(const uint16_t *pens)
{
    int b;
    for (b = 11; b >= 0; b--) if (!bank_taken(b) && !(borrowed_banks & (1u << b))) break;
    if (b < 0) return -1;
    borrowed_banks |= 1u << b;
    for (int pen = 0; pen < 16; pen++) m68k_write_memory_16(PAL_SPRITE + (unsigned)b * 0x80u + (unsigned)pen * 2u, pens[pen]);
    if (eng_dbgsel) fprintf(stderr, "sprite: badge palette borrows bank %d\n", b);
    return b;
}
static int emit_top = -0x7FFF, emit_x0 = 0x7FFF, emit_x1 = -0x7FFF;   /* the extent of the last eng_sprite_emit_pose (cells) */
static int obj_top[ENG_MAX_OBJS];       /* per object, this frame (the compile loop records it) */
static int obj_drawn[ENG_MAX_OBJS];     /* 1 = his own sprite was emitted this frame, 2 = inside his partner's composite, 0 = not on screen */
static int obj_cx[ENG_MAX_OBJS];        /* the drawn sprite's horizontal centre (screen x) */
int eng_sprite_obj_cx(int i) { return (i >= 0 && i < ENG_MAX_OBJS) ? obj_cx[i] : 0; }
int eng_sprite_obj_top(int i) { return (i >= 0 && i < ENG_MAX_OBJS) ? obj_top[i] : 0; }
int eng_sprite_obj_drawn(int i) { return (i >= 0 && i < ENG_MAX_OBJS) ? obj_drawn[i] : 0; }
static int weapon_bank_get(int idx)
{
    const uint16_t *pk; int b;
    if (idx < 0 || idx >= WPN_BANKS) return 0x0F;
    if (wpn_bank[idx] >= 0) return wpn_bank[idx];
    pk = eng_wpn_palette(idx);
    if (!pk) return 0x0F;
    if (idx == 0) {
        int same = 1;
        for (int pen = 0; pen < 16 && same; pen++) same = pk[pen] == rom16(ROM_BODY + 15u * 32u + (unsigned)pen * 2u);
        if (same) return 0x0F;         /* stock: the ROM's own bank, 0 px */
    }
    for (b = 11; b >= 0; b--) if (!bank_taken(b)) break;
    if (b < 0) return 0x0F;            /* everyone in play: the ROM bank */
    for (int pen = 0; pen < 16; pen++)
        m68k_write_memory_16(PAL_SPRITE + (unsigned)b * 0x80u + (unsigned)pen * 2u, pk[pen]);
    if (eng_dbgsel) fprintf(stderr, "sprite: weapon palette %d borrows bank %d\n", idx, b);
    wpn_bank[idx] = b;
    return b;
}

static uint32_t rom32(unsigned a)
{
    return ((uint32_t)rom16(a) << 16) | rom16(a + 2);
}

int eng_sprite_init(const char *rom_dir)
{
    (void)rom_dir;                     /* the thinker reads the data layer, not the chips */
    if (wf_thinker_init_live() != 0) {
        fprintf(stderr, "engine: stream decoder init: %s\n", wf_thinker_error());
        return -1;
    }
    thinker_ready = 1;
    body_pals_pending = 1;             /* installed by eng_sprite_palettes_flush once the data layer is loaded */
    return 0;
}

/* eng_sprite_init runs before the data layer is bound (main.c loads the
 * tables after eng_render_init), so the boot-time 0x2AEA install is
 * deferred to the first frame: pal_load.c's per-frame latch calls this
 * before anything draws or snapshots the sprite palette bank (the attract's
 * 0x26772 snapshot included). */
void eng_sprite_palettes_flush(void)
{
    uint32_t len;
    if (!body_pals_pending || !tbl_bytes(TBL(ws_body_palettes), &len))
        return;
    eng_sprite_install_body_palettes();
    if (getenv("WF_STREAM_SCAN")) {        /* bound check for sprite_streams: decode every pose of every row */
        extern uint32_t wf_thinker_rom_hwm(void), wf_thinker_rom_lwm(void);
        extern void wf_thinker_rom_marks_reset(void);
        WfThinkerSpr spr[WF_THINKER_MAX_SPR];
        long decodes = 0, stray = 0;
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (unsigned row = 0; row < 81; row++) {
            unsigned off_tab = rom16(TAB_META + row * 2u), next = 0, npose;
            uint32_t base = rom32(TAB_STREAM + row * 4u);
            if (!off_tab || !base) continue;
            for (unsigned r = row + 1; r < 81 && !next; r++) next = rom16(TAB_META + r * 2u);
            npose = ((next ? next : 0x3C99Cu - TAB_META) - off_tab) / 2u;   /* table runs to the next row's, or to the streams */
            if (base < lo) lo = base;
            for (unsigned pose = 0; pose < npose; pose++) {
                unsigned po = rom16(TAB_META + off_tab + pose * 2u);
                if (po == 0xFFFEu || po == 0xFFFFu) continue;   /* no such pose / table terminator */
                for (int flip = 0; flip < 2; flip++)
                    for (int prow = -1; prow < 12; prow++) {   /* partner slices: the 12 wrestler rows */
                        int n;
                        wf_thinker_set_partner_row(prow);
                        wf_thinker_rom_marks_reset();
                        n = wf_thinker_decode_obj(base, po, flip, 0, 0, (uint16_t)row,
                                                  (uint16_t)(pose | (flip ? 0x8000u : 0)), spr, WF_THINKER_MAX_SPR);
                        decodes++;
                        if (wf_thinker_rom_lwm() < 0x38F14u) {   /* wandered below the sprite tables */
                            if (stray++ < 40)
                                fprintf(stderr, "stream scan: STRAY row %u pose %u flip %d partner %d -> reads 0x%X..0x%X (n %d)\n",
                                        row, pose, flip, prow, wf_thinker_rom_lwm(), wf_thinker_rom_hwm(), n);
                            continue;
                        }
                        if (wf_thinker_rom_hwm() > hi) hi = wf_thinker_rom_hwm();
                    }
            }
        }
        fprintf(stderr, "stream scan: %ld decodes (%ld stray), lowest base 0x%X, highest ROM byte read 0x%X\n",
                decodes, stray, lo, hi);
    }
}

/* 0x2AEA per-match body-palette allocation. 16 entries: 12 wrestlers +
 * referee/ring-hardware/extras — the compiler reads the table out to id
 * 15 (provenance audit). Called at boot AND on every front-end -> match
 * transition (render.c): the attract's fade (0x26642) snapshots and
 * rewrites the sprite bank 0x182000 through palette RAM, so a snapshot
 * taken from an already-faded bank ratchets the body colours darker on
 * each attract pass — after a few matches only the shadows were left
 * (playtest "sprites go black after ~5 matches"). */
void eng_sprite_install_body_palettes(void)
{
    body_pals_pending = 0;
    extra_loaded = 0;                  /* bank 0xD (announcer) re-copies on next use */
    portrait_loaded = 0;
    scene_pal_n = -1;                  /* the interlude 0x2AEA map goes with the match install */
    clone_banks_reset();               /* clone slots re-borrow (clone_bank_get) */
    borrowed_banks = 0; eng_badges_reset_banks(); eng_ropeart_reset_banks();   /* the badge palettes re-borrow too: the install overwrites every bank
                                                       (the chips came up in a wrestler's colours in the NEXT game) */
    for (int id = 0; id < 16; id++) {
        /* banks 12..15 stay the ROM referee/hardware/extras palettes —
         * clone ids 12..15 borrow a free STOCK bank instead (clone_bank_get) */
        const uint16_t *pk = (id < 12 && !getenv("WF_NOPKG")) ? eng_pkg_palette((unsigned)id) : 0;
        if (id < 12) {   /* palette-select mod: the outfit picked at the select
                          * screen. STOCK ids only — a clone's outfit choice
                          * (palettes/12..15.json) must NOT land here: banks
                          * 12..15 hold the referee/ring-hardware palettes,
                          * a clone wears its outfit via clone_bank_get. */
            const uint16_t *alt = eng_palsel_pens(id);
            if (alt) pk = alt;
        }
        for (int pen = 0; pen < 16; pen++)
            m68k_write_memory_16(PAL_SPRITE + (unsigned)id * 0x80u + (unsigned)pen * 2u,
                            pk ? pk[pen] : rom16(ROM_BODY + (unsigned)id * 32u + (unsigned)pen * 2u));
    }
}

/* A LATE stock spawn (mod backup run-in): its bank == id may have been
 * lent to a badge / rope-art / clone / weapon palette since the bell
 * (bank_taken saw the id idle) - "Demolition's backup climbed in and the
 * palette was corrupt" (user 2026-08-30). Re-copy the body palette into
 * the bank, mark the id busy and make every borrower re-borrow. */
void eng_sprite_reclaim_bank(unsigned id)
{
    const uint16_t *pk;
    if (id >= 12) return;
    rows_busy |= (uint16_t)(1u << id);
    if (borrowed_banks & (1u << id)) { borrowed_banks &= ~(1u << id); eng_badges_reset_banks(); eng_ropeart_reset_banks(); }
    for (int k = 0; k < 32; k++) if (clone_bank[k] == (int)id) clone_bank[k] = -1;
    for (int k = 0; k < WPN_BANKS; k++) if (wpn_bank[k] == (int)id) wpn_bank[k] = -1;
    pk = !getenv("WF_NOPKG") ? eng_pkg_palette(id) : 0;
    { const uint16_t *alt = eng_palsel_pens((int)id); if (alt) pk = alt; }
    for (int pen = 0; pen < 16; pen++)
        m68k_write_memory_16(PAL_SPRITE + id * 0x80u + (unsigned)pen * 2u,
                             pk ? pk[pen] : rom16(ROM_BODY + id * 32u + (unsigned)pen * 2u));
    if (eng_dbgsel) fprintf(stderr, "sprite: bank %u reclaimed for a late spawn\n", id);
}

/* 0x2AEA: the ROM allocates a palette slot per extra id on demand and
 * copies its body palette (0x2F22 + id*32) in; the engine keeps stock
 * ids at bank == id, so extras (announcer 0x1F) borrow bank 0xD, which
 * no match-time sprite uses. TODO EXACT: the $1C1600/$1C1610 allocator. */
unsigned eng_sprite_extra_bank(unsigned id)
{
    unsigned bank = 0x0Du;
    if (extra_loaded != id) {
        extra_loaded = id;
        for (int pen = 0; pen < 16; pen++)
            m68k_write_memory_16(PAL_SPRITE + bank * 0x80u + (unsigned)pen * 2u,
                                 rom16(ROM_BODY + id * 32u + (unsigned)pen * 2u));
    }
    return bank;
}

/* 0x2AEA on demand for the front-end interlude screens: the ROM's
 * $1C1600/$1C1610 allocator hands each palette id (the stream's bank
 * byte: 0x31..0x33 LOD panels, 0x37 title centre, 0x38/0x39 belt art,
 * 0x3C lettering, 0x40+id portraits) its own sprite bank as the screen
 * loads it (0xAF0A / 0xB6EC / 0xB73C / 0xB94C / 0xBE4E).  No wrestler
 * draws while these scenes run, so banks are dealt 0..15 in load order;
 * eng_sprite_install_body_palettes (every front -> match transition,
 * render.c) drops the map. */
void eng_sprite_scene_pals_begin(void) { scene_pal_n = 0; }
void eng_sprite_scene_pals_end(void)   { scene_pal_n = -1; }
void eng_sprite_scene_pals_rearm(void) { if (scene_pal_n < 0) scene_pal_n = 0; }

static unsigned scene_pal_bank(unsigned id)
{
    for (int k = 0; k < scene_pal_n; k++)
        if (scene_pal_ids[k] == id) return (unsigned)k;
    if (scene_pal_n >= 16) return id & 0x0Fu;      /* allocator full (ROM: never) */
    scene_pal_ids[scene_pal_n] = id;
    if (getenv("WF_DBGSEL"))
        fprintf(stderr, "sprite: 0x2AEA scene pal %02X -> bank %d\n", id, scene_pal_n);
    {   /* the palette-select outfit follows the wrestler into 0x2AEA
         * scenes (intro banner, interludes, ceremony) */
        const uint16_t *alt = (id < 12u) ? eng_palsel_pens((int)id) : NULL;
        const uint16_t *pk = (id < 12u && !getenv("WF_NOPKG")) ? eng_pkg_palette(id) : NULL;
        if (alt) pk = alt;
        for (int pen = 0; pen < 16; pen++)         /* 0x2AEA copy 0x2F22 + id*32 */
            m68k_write_memory_16(PAL_SPRITE + (unsigned)scene_pal_n * 0x80u + (unsigned)pen * 2u,
                                 pk ? pk[pen] : rom16(ROM_BODY + id * 32u + (unsigned)pen * 2u));
    }
    return (unsigned)scene_pal_n++;
}

/* 0x2AEA #$1C: the HUD/rumble portrait palette (ROM body-palette entry
 * 0x1C) for the portrait rows 0x10-0x1B, parked in bank 0xF (unused by
 * match sprites). Re-copied after every body-palette install. */
static unsigned bank0F_load(unsigned id)
{
    unsigned bank = 0x0Fu;
    if (portrait_loaded != (int)(id + 1u)) {
        portrait_loaded = (int)(id + 1u);
        for (int pen = 0; pen < 16; pen++)
            m68k_write_memory_16(PAL_SPRITE + bank * 0x80u + (unsigned)pen * 2u,
                                 rom16(ROM_BODY + id * 32u + (unsigned)pen * 2u));
    }
    return bank;
}
unsigned eng_sprite_portrait_bank(void) { return bank0F_load(0x1Cu); }
static unsigned bank0F_load_pens(const uint16_t *pens)   /* a card's own palette into bank 0xF */
{
    for (int pen = 0; pen < 16; pen++)
        m68k_write_memory_16(PAL_SPRITE + 0x0Fu * 0x80u + (unsigned)pen * 2u, pens[pen]);
    portrait_loaded = 0;               /* bank0F_load re-copies its entry next time */
    return 0x0Fu;
}

/* Emit one pose of one stream row at screen position (sx, sy).
 * pose bit15 = flip. partner_row < 0 disarms the DA12 interaction path.
 * Returns records written. */
int eng_sprite_emit_pose(unsigned row, unsigned pose_word, int sx, int sy,
                         int partner_row, unsigned *slot)
{
    WfThinkerSpr spr[WF_THINKER_MAX_SPR];
    unsigned pose = pose_word & 0x7FFFu;
    int flip = (pose_word & 0x8000u) ? 1 : 0;
    unsigned off_tab, pose_off;
    uint32_t base;
    int n, i, wrote = 0;
    int clone = -1, pclone = -1;       /* clone slots ride VIRTUAL rows 0x60..0x63
                                          (eng_sprite_row) — rows 12..15 are the
                                          referee/hardware. Decode to the clone id
                                          + its base's art row; the base-id pal
                                          bytes remap to the borrowed bank below. */

    int aisle_clone = 0;
    unsigned cell_pose = 0;            /* a clone's card art: the pak pose id to draw (0 = not a card row) */
    if (row >= CLONE_CROW0 && row < CLONE_CROW0 + (unsigned)(ENG_WS_EXT_MAX - 12)) {
        clone = (int)(row - CLONE_CROW0) + 12;
        row = 0x30u + (unsigned)eng_ws_base(clone);
        cell_pose = ENG_CONT_POSE0 + pose;
    } else if (row >= CLONE_TROW0 && row < CLONE_TROW0 + (unsigned)(ENG_WS_EXT_MAX - 12)) {
        clone = (int)(row - CLONE_TROW0) + 12;
        row = 0x4Du;
        cell_pose = ENG_TITLE_POSE;
    } else
    if (row >= CLONE_ROW0 && row < CLONE_ROW0 + (unsigned)(ENG_WS_EXT_MAX - 12)) {
        clone = (int)(row - CLONE_ROW0) + 12;
        row = (unsigned)eng_ws_base(clone);
    } else if (row >= CLONE_AROW0 && row < CLONE_AROW0 + (unsigned)(ENG_WS_EXT_MAX - 12)) {
        clone = (int)(row - CLONE_AROW0) + 12;
        row = 0x40u + (unsigned)eng_ws_base(clone);
        if (row == 0x4Bu) row--;
        aisle_clone = 1;
    }
    if (partner_row >= (int)CLONE_ROW0 && partner_row < (int)CLONE_ROW0 + (ENG_WS_EXT_MAX - 12)) {
        pclone = partner_row - (int)CLONE_ROW0 + 12;
        partner_row = eng_ws_base(pclone);
    }
    if (!thinker_ready || row >= 81)
        return 0;
    if (row < 0x4E && (sx < -0x3F || sx >= wf_view_w() + 0x5F
                       || sy < -0x3F || sy >= wf_view_h() + 0x3F))
        return 0;                      /* 0x2188 screen clip (stock 0x17F/0x13F
                                          for the 320x240 view): 9-bit sprite x
                                          would otherwise wrap the far ropes
                                          onto the opposite edge; widened with
                                          the zoom mod's view */
    if (row == 0x0Fu) {                /* a RINGSIDE WEAPON: a pak-ingested type's loose art
                                          (lying / tumble) draws from the weapon arena; the two
                                          ROM types fall through to the ROM stream below */
        const eng_pkg_rec *pr; int pidx = 0;
        int pn = eng_wpn_loose((int)(pose / 3u), (int)(pose % 3u), &pr, &pidx);
        if (pn > 0) {
            unsigned bank = (unsigned)weapon_bank_get(pidx);
            for (i = 0; i < pn && *slot + 16 <= WF_SPRRAM_SIZE; i++) {
                uint8_t *r = wf.spriteram + *slot;
                int cx = pr[i].x, cfx = pr[i].flipx;
                if (flip) { cx = -cx - 16; cfx = !cfx; }
                {
                    int x = cx + sx, y = pr[i].y + sy;
                    uint16_t w1 = 0x0001u | ((y & 0x100u) >> 7) | ((x & 0x100u) >> 6)
                                | (pr[i].flipy ? 0x0008u : 0) | (cfx ? 0x0010u : 0)
                                | ((unsigned)(pr[i].chain & 7u) << 5);
                    r[1] = (uint8_t)y; r[2] = (uint8_t)(w1 >> 8); r[3] = (uint8_t)w1;
                    r[5] = (uint8_t)pr[i].tile; r[7] = (uint8_t)(pr[i].tile >> 8);
                    r[9] = (uint8_t)bank; r[11] = (uint8_t)x;
                    r[12] = (uint8_t)((pr[i].tile >> 16) ? 0xE7 : 0);
                    r[13] = (uint8_t)((pr[i].tile >> 16) & 0x0Fu);
                    r[14] = (uint8_t)((pr[i].tile >> 16) ? 0x5C : 0);
                }
                *slot += 16; wrote++;
            }
            return wrote;
        }
    }
    if (cell_pose) {                   /* CARD art (continue face / title card) from the clone's pak,
                                          drawn in the card's OWN palette parked in bank 0xF (the
                                          rumble-entrant portrait bank; no match sprites on screen) */
        const eng_pkg_rec *pr; int pn = eng_pkg_pose((unsigned)clone, cell_pose, flip, -1, &pr);
        const uint16_t *cp = eng_pkg_cell_pens((unsigned)clone, cell_pose);
        unsigned bank = cp ? bank0F_load_pens(cp) : (unsigned)clone_bank_get(clone);
        if (pn <= 0 || !eng_pkg_pose_was_own()) return 0;
        for (i = 0; i < pn && *slot + 16 <= WF_SPRRAM_SIZE; i++) {
            uint8_t *r = wf.spriteram + *slot;
            int x = pr[i].x + sx, y = pr[i].y + sy;
                if (y + 16 > emit_top) emit_top = y + 16;   /* a chain extends DOWNWARD: the record y is the top cell */
                if (x < emit_x0) emit_x0 = x; if (x + 16 > emit_x1) emit_x1 = x + 16;
            uint16_t w1 = 0x0001u | ((y & 0x100u) >> 7) | ((x & 0x100u) >> 6)
                        | (pr[i].flipy ? 0x0008u : 0) | (pr[i].flipx ? 0x0010u : 0)
                        | ((unsigned)(pr[i].chain & 7u) << 5);
            r[1] = (uint8_t)y; r[2] = (uint8_t)(w1 >> 8); r[3] = (uint8_t)w1;
            r[5] = (uint8_t)pr[i].tile; r[7] = (uint8_t)(pr[i].tile >> 8);
            r[9] = (uint8_t)bank; r[11] = (uint8_t)x;
            r[12] = (uint8_t)((pr[i].tile >> 16) ? 0xE7 : 0);
            r[13] = (uint8_t)((pr[i].tile >> 16) & 0x0Fu);
            r[14] = (uint8_t)((pr[i].tile >> 16) ? 0x5C : 0);
            *slot += 16; wrote++;
        }
        return wrote;
    }
    wf_thinker_set_partner_row(partner_row);
    off_tab = rom16(TAB_META + row * 2u);
    if (!off_tab)
        return 0;
    pose_off = rom16(TAB_META + off_tab + pose * 2u);
    if (pose_off == 0xFFFEu && !(row < 12 || aisle_clone))
        return 0;                      /* (a stock row: the CLASS template may still have the
                                          pose — a move outside his set — so try the package
                                          path first; the ROM's "absent" applies below) */
    if (row < 12 || aisle_clone) {     /* package first (data/wrestlers/NN);
                                          a clone asks its OWN pak, which
                                          delegates to the base's (package.c).
                                          Aisle clones ask for pak pose
                                          ENG_AISLE_POSE0+cell (base paks
                                          never carry those, so a clone
                                          without walkout art cleanly falls
                                          through to the base ROM stream). */
        const eng_pkg_rec *pr; const uint8_t *src; int pn;
        if (aisle_clone) {
            pn = eng_pkg_pose((unsigned)clone, ENG_AISLE_POSE0 + pose, flip, -1, &pr);
            if (pn > 0) {
                static uint8_t asrc[192];
                int own_art = eng_pkg_pose_was_own();   /* delegated base frames KEEP
                                                           the base's palette bank */
                if (pn > 192) pn = 192;
                for (i = 0; i < pn; i++)
                    asrc[i] = own_art && (pr[i].pal & 0x0Fu) == (unsigned)eng_ws_base(clone) ? ENG_SRC_HOLDER_OWN : ENG_SRC_HOLDER;
                src = asrc;
            }
        } else
            pn = eng_compose(clone >= 0 ? clone : (int)row, pose, flip,
                             pclone >= 0 ? pclone : partner_row, &pr, &src);
        if (pn > 0) {
            for (i = 0; i < pn && *slot + 16 <= WF_SPRRAM_SIZE; i++) {
                uint8_t *r = wf.spriteram + *slot;
                int x = pr[i].x + sx, y = pr[i].y + sy;
                if (y + 16 > emit_top) emit_top = y + 16;   /* a chain extends DOWNWARD: the record y is the top cell */
                if (x < emit_x0) emit_x0 = x; if (x + 16 > emit_x1) emit_x1 = x + 16;
                unsigned bank = pr[i].pal & 0x0Fu;
                switch (src[i]) {
                case ENG_SRC_HOLDER_OWN:           /* own art: the package's borrowed bank (a
                                                      clone, or a class template standing in
                                                      for a stock man) */
                    { int cs0 = eng_compose_slot(0) >= 0 ? eng_compose_slot(0) : clone;
                      bank = cs0 >= 0 && cs0 < ENG_WS_MAX ? (unsigned)cs0 : (unsigned)clone_bank_get(cs0); } break;   /* a stock man in the skin layout: his own bank */
                case ENG_SRC_VICTIM_OWN:           /* the held man's own victim body */
                    { int cs1 = eng_compose_slot(1) >= 0 ? eng_compose_slot(1) : pclone;
                      bank = cs1 >= 0 && cs1 < ENG_WS_MAX ? (unsigned)cs1 : (unsigned)clone_bank_get(cs1); } break;
                case ENG_SRC_VICTIM:               /* base victim cells: a palette-only clone
                                                      borrows his bank onto them; an AI-art
                                                      clone keeps the base palette (the remap
                                                      painted pin/powerslam frames tie-dye) */
                    if (pclone >= 0 && bank == (unsigned)partner_row && !eng_pkg_has_poses((unsigned)pclone))
                        bank = (unsigned)clone_bank_get(pclone);
                    break;
                case ENG_SRC_OVERLAY:              /* weapon cells: the pak palette's bank (stock: 0xF) */
                    bank = (unsigned)weapon_bank_get((int)(pr[i].pal >> 4)); break;
                default: break;                    /* holder base cells */
                }
                uint16_t w1 = 0x0001u | ((y & 0x100u) >> 7) | ((x & 0x100u) >> 6)
                            | (pr[i].flipy ? 0x0008u : 0) | (pr[i].flipx ? 0x0010u : 0)
                            | ((unsigned)(pr[i].chain & 7u) << 5);
                r[1] = (uint8_t)y; r[2] = (uint8_t)(w1 >> 8); r[3] = (uint8_t)w1;
                r[5] = (uint8_t)pr[i].tile; r[7] = (uint8_t)(pr[i].tile >> 8);
                r[9] = (uint8_t)bank; r[11] = (uint8_t)x;
                /* clone-art arena: tile bits 16+ ride the record's unused
                 * word 6 behind a 2-byte magic (drain checks both), so a
                 * stock/68k record can never fake it. ALWAYS written: slots
                 * are reused and stale magic would corrupt ROM-art tiles. */
                r[12] = (uint8_t)((pr[i].tile >> 16) ? 0xE7 : 0);
                r[13] = (uint8_t)((pr[i].tile >> 16) & 0x0Fu);
                r[14] = (uint8_t)((pr[i].tile >> 16) ? 0x5C : 0);
                *slot += 16; wrote++;
            }
            return wrote;
        }
    }
    if (pose_off == 0xFFFEu)
        return 0;                      /* nothing in the packages either */
    base = rom32(TAB_STREAM + row * 4u);
    if (!base)
        return 0;

    n = wf_thinker_decode_obj(base, pose_off, flip,
                              (int16_t)sx, (int16_t)sy,
                              (uint16_t)row,
                              (uint16_t)(pose | (flip ? 0x8000u : 0)),
                              spr, WF_THINKER_MAX_SPR);
    if (n < 0)
        return 0;
    if (getenv("WF_POSETEST") && getenv("WF_DBGSEL")) {
        int ds, dr, dx, dy, dm;
        wf_thinker_last_da12(&ds, &dr, &dx, &dy, &dm);
        fprintf(stderr, "decode: n=%d trunc=%d da12{seen %d row %d} off_tab %04X pose_off %04X base %06X\n",
                n, wf_thinker_last_truncated(), ds, dr, off_tab, pose_off, base);
    }
    if (getenv("WF_POSETEST") && getenv("WF_DBGSEL"))
        for (i = 0; i < n; i++)
            fprintf(stderr, "rec[%d]: x %d y %d tile %04X chain %u flipx %d flipy %d pal %X\n",
                    i, spr[i].x, spr[i].y, spr[i].tile, spr[i].chain, spr[i].flipx, spr[i].flipy, spr[i].pal);
    for (i = 0; i < n && *slot + 16 <= WF_SPRRAM_SIZE; i++) {
        uint8_t *r = wf.spriteram + *slot;
        int x = spr[i].x, y = spr[i].y;
        /* The stream's bank byte (raw + dpal, applied by the decoder) is
         * the ABSOLUTE character id whose bank the art was authored for —
         * Hogan's own body bakes in 0, Warrior's bakes in 1, and grapple
         * cells carry each figure's real id (docs/engine-specs/
         * palette-banks.md). $1C1610 maps id -> allocated bank; the
         * engine installs each stock id's body palette at bank == id, so
         * the map is identity here. Reskins/extras re-map later. */
        unsigned bank = spr[i].pal & 0x0Fu;
        if (clone >= 0 && bank == row)             /* clone body: borrowed bank */
            bank = (unsigned)clone_bank_get(clone);
        else if (pclone >= 0 && bank == (unsigned)partner_row)   /* carried clone slice */
            bank = (unsigned)clone_bank_get(pclone);
        else if (scene_pal_n >= 0) bank = scene_pal_bank(spr[i].pal & 0xFFu);   /* interlude 0x2AEA map */
        else if (row == 0x1Fu) bank = eng_sprite_extra_bank(0x1Fu);   /* 0x2AEA #$1F */
        else if (row == 0x1Eu) bank = eng_sprite_extra_bank(spr[i].pal & 0xFFu);   /* the chips:
                                                        0x2AEA #$35 on show (0x874A) */
        else if (row >= 0x10u && row < 0x1Cu) {     /* portrait rows: the stream bakes pal 0x1C (frame)
                                                        and 0x10+id (face): 0x73D4 0x2AEA(row) + 0x2AEA(#$1C) */
            unsigned pid = spr[i].pal & 0xFFu;
            bank = (pid == 0x1Cu) ? eng_sprite_portrait_bank() : eng_sprite_extra_bank(pid);
        }
        else if (row >= 0x30u && row < 0x3Cu) {     /* 0x8CF8 demo match cards: 0x2AEA(row-0x20)
                                                        + 0x2AEA(#$1E) (0x8DC8/0x8DF6); no
                                                        portraits draw in the all-CPU demo, so
                                                        the card id parks in bank 0xF and 0x1E
                                                        in the extras bank 0xD */
            unsigned pid = spr[i].pal & 0xFFu;
            bank = (pid == 0x1Eu) ? eng_sprite_extra_bank(0x1Eu) : bank0F_load(pid);
        }
        uint16_t w1 = 0x0001u
                    | ((y & 0x100u) >> 7)         /* bit1 = y8  */
                    | ((x & 0x100u) >> 6)         /* bit2 = x8  */
                    | (spr[i].flipy ? 0x0008u : 0)
                    | (spr[i].flipx ? 0x0010u : 0)
                    | ((unsigned)(spr[i].chain & 7u) << 5);
        r[1] = (uint8_t)y;
        r[2] = (uint8_t)(w1 >> 8);
        r[3] = (uint8_t)w1;
        r[5] = (uint8_t)spr[i].tile;
        r[7] = (uint8_t)(spr[i].tile >> 8);
        r[9] = (uint8_t)bank;
        r[11] = (uint8_t)x;
        r[12] = 0; r[13] = 0; r[14] = 0;   /* scrub stale clone-art magic */
        *slot += 16;
        wrote++;
    }
    return wrote;
}

/* Draw order = the $2836 drain: lists 5,4,3,2,1,0 — later records draw
 * on top. Wrestlers sit ABOVE both rope lists by default (playtested:
 * every rope tier draws behind a wrestler standing at the ropes); the
 * ROM's rope-lean action drops a wrestler to list 2 — BETWEEN the rope
 * halves — which is when the near tiers cover him (0x18382). That state
 * arrives with the lean/whip machines. */
/* one object's body: the calibration context is his move/frame (state 5) */
/* the weapon type `o` is carrying, -1 none: eng_wpn_cells swaps an image
   type's cells onto the shared box poses (2026-08-27) */
static int carried_type(const eng_state *st, const eng_obj *o)
{
    if (!(o->weapon_w & WPN_HELD) || o->wobj < 1 || o->wobj > ENG_WEAPONS) return -1;
    return (int)st->wpn[o->wobj - 1].type;
}
static void emit_obj(const eng_obj *o, unsigned *slot)
{
    { int k = eng_calib_key(o); eng_compose_ctx(k >= 0 ? eng_ws_body_class(o->wrestler) : -1, k, (int)(o->frame & 0xFFu)); }
    emit_top = -0x7FFF; emit_x0 = 0x7FFF; emit_x1 = -0x7FFF;
    eng_sprite_emit_pose(eng_sprite_row(o->wrestler), o->spr, o->sx, o->sy, -1, slot);
    eng_compose_ctx(-1, -1, -1);
    if (emit_top > -0x7FFF) {           /* the badge anchor of a list-4 / list-2 man (apron, rope-lean) */
        int i = (int)(o - cur_objs);
        if (i >= 0 && i < ENG_MAX_OBJS) { obj_top[i] = emit_top; obj_cx[i] = (emit_x0 + emit_x1) / 2; obj_drawn[i] = 1; }
    }
}
void eng_sprite_emit(const eng_state *st)
{
    unsigned slot = 0;

    rows_busy = 0;                     /* stock banks in play (clone_bank_get) */
    for (int i = 0; i < ENG_MAX_OBJS; i++)
        if (st->obj[i].active)
            rows_busy |= (uint16_t)(1u << (eng_ws_base(st->obj[i].wrestler) & 0x0F));
    memset(wf.spriteram, 0, WF_SPRRAM_SIZE);
    cur_objs = st->obj;
    for (int k = 0; k < ENG_MAX_OBJS; k++) { obj_top[k] = st->obj[k].sy + 0x60; obj_drawn[k] = 0; obj_cx[k] = (int)(st->obj[k].x >> 16) - st->cam_x; }   /* badge anchors: reset per frame */
    if (getenv("WF_POSETEST")) {       /* debug: "row,pose,prow" at screen centre */
        unsigned row = 0, pose = 0; int prow = -1, px = 160, py = 140;
        sscanf(getenv("WF_POSETEST"), "%u,%x,%d,%d,%d", &row, &pose, &prow, &px, &py);
        eng_sprite_emit_pose(row, pose, px, py, prow, &slot);
        memcpy(wf.spriteram_buffered, wf.spriteram, WF_SPRRAM_SIZE);
        return;
    }
    /* list 4 (+0x12 = 4, apron men 0x11796): drained before both rope
     * lists — partners on the apron sit BEHIND the ropes. */
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        const eng_obj *o = &st->obj[i];
        if (!o->active || !(o->apron || o->list == 4) || (o->spr & 0x7FFFu) == 0x7FFFu) continue;
        if (eng_dbgsel && (st->frame % 4) == 0)
            fprintf(stderr, "emit4: row %u spr %04X slot %u\n", eng_sprite_row(o->wrestler), o->spr, slot);
        emit_obj(o, &slot);
    }
    for (int k = 0; k < ENG_FX_SLOTS; k++)                   /* list-4 companions */
        if (st->fx[k].active && st->fx[k].list == 4 && (st->fx[k].spr & 0x7FFFu) != 0x7FFFu)
            eng_sprite_emit_pose(st->fx[k].row, st->fx[k].spr, st->fx[k].sx, st->fx[k].sy, -1, &slot);
    eng_ringhw_emit(st, 3, &slot);     /* back rope halves */
    for (int i = 0; i < ENG_MAX_OBJS; i++) {                /* list 2: rope-lean */
        const eng_obj *o = &st->obj[i];
        if (!o->active || o->apron || o->list != 2 || (o->spr & 0x7FFFu) == 0x7FFFu) continue;
        eng_wpn_carry_ctx(carried_type(st, o));
        emit_obj(o, &slot);
        eng_wpn_carry_ctx(-1);
    }
    for (int k = 0; k < ENG_FX_SLOTS; k++)                   /* list-2 companions */
        if (st->fx[k].active && st->fx[k].list == 2 && (st->fx[k].spr & 0x7FFFu) != 0x7FFFu)
            eng_sprite_emit_pose(st->fx[k].row, st->fx[k].spr, st->fx[k].sx, st->fx[k].sy, -1, &slot);
    if (st->g161 & 2u)                 /* ringside weapons (sheet 0x0F): the
                                          0xFDEE machine enqueues them only in
                                          the ringside scene; resting/tumbling
                                          use +0x12 = 2 (0xFF3C/0xFF6A), the
                                          tossed flight list 0 (0xFEAC) */
        for (int k = 0; k < ENG_WEAPONS; k++) {
            const eng_weapon *w = &st->wpn[k];
            if (!w->active || w->list != 2 || (w->spr & 0x7FFFu) == 0x7FFFu) continue;
            eng_sprite_emit_pose(0x0Fu, w->spr, w->sx, w->sy, -1, &slot);
        }
    else                               /* INSIDE slots draw in the normal scene too
                                          (user 2026-08-28: weapons in the ring) */
        for (int k = 0; k < ENG_WEAPONS; k++) {
            const eng_weapon *w = &st->wpn[k];
            if (!w->active || !w->inside || w->list != 2 || (w->spr & 0x7FFFu) == 0x7FFFu) continue;
            eng_sprite_emit_pose(0x0Fu, w->spr, w->sx, w->sy, -1, &slot);
        }
    eng_ringhw_emit(st, 1, &slot);     /* front rope halves */
    /* Ring-out camera scene ($1C0161 b1, scene 2/6): the object pass calls
     * 0xF8D8 for every man still INSIDE (0xF4F6: f33 b2 clear) and the
     * referee machine calls 0xF8E4 (0x1F94A) — each drops to LIST 1 (+0x12
     * = 1, under list 0, this pass) and authors a near-rope sprite in list
     * 0 over him: row 14, $1C1134 (wrestlers) / $1C1164 (referee), y 0x151;
     * pose 0 following his x inside [0x290,0x3B0) at z 0x13E, pose 1 at x
     * 0x269 left of it, pose 0x8001 at x 0x3C7 right of it (z 0x146). The
     * slot is re-authored and re-enqueued per call, so the last man wins
     * (identical overdraw) — emit it once. Outside men keep list 0 and
     * their smaller y puts them over the rope. */
    int rs = (st->scene == 2 || st->scene == 6);
    int rope_w = -1, rope_r = -1;      /* x that authored $1C1134 / $1C1164 */
    if (rs) {
        for (int i = 0; i < ENG_MAX_OBJS; i++) {
            const eng_obj *o = &st->obj[i];
            if (!o->active || (o->role & RF_OUTSIDE) || o->apron || o->list == 4 || o->list == 2) continue;
            if ((o->spr & 0x7FFFu) == 0x7FFFu || (o->grap44 & 0x8000u)) continue;
            { int k = eng_calib_key(o); if (k >= 0) eng_compose_ctx(eng_ws_body_class(o->wrestler), k, (int)(o->frame & 0xFFu)); }
            eng_sprite_emit_pose(eng_sprite_row(o->wrestler), o->spr, o->sx, o->sy,
                                 o->partner >= 0 ? (int)eng_sprite_row(st->obj[o->partner].wrestler) : -1, &slot);
            eng_compose_ctx(-1, -1, -1);
            rope_w = o->x >> 16;
        }
        if (st->ref.active) {
            if (!(st->ref.sm == 10 && eng_badge_emit(eng_ref_down_badge(st), st->ref.sx, st->ref.sy, &slot)))   /* the external KNOCKED-OUT
                                          art (data/badges/ref_down_N.png) if packed, else the stock kneel */
                eng_sprite_emit_pose(12u, st->ref.spr, st->ref.sx, st->ref.sy, -1, &slot);
            rope_r = st->ref.x >> 16;
        }
    }
    /* Characters draw far-to-near: the ROM's per-list sort 0x2948 orders
     * by WORLD depth +0x0A descending (never by screen y — the pose
     * hotspot would leak into the order), stable on ties; the referee is
     * enqueued before the wrestler slots (draw-order.md). */
    {
        struct ent { int sy; int wy; unsigned row; uint16_t spr; int sx; int prow; int ccls, cmove, cframe, wctx; int oi; } e[ENG_MAX_OBJS + 4 + ENG_FX_SLOTS + ENG_WEAPONS];
        int n = 0;
        for (int k = 0; k < (int)(sizeof e / sizeof e[0]); k++) { e[k].ccls = -1; e[k].cmove = -1; e[k].cframe = -1; e[k].wctx = -1; e[k].oi = -1; }
        for (int k = 0; k < ENG_WEAPONS; k++) {   /* a TOSSED weapon (+0x12 = 0, 0xFEAC)
                                          joins the list-0 depth sort (any scene:
                                          inside slots fly during normal play) */
            {
                const eng_weapon *w = &st->wpn[k];
                if (!w->active || w->list != 0 || (w->spr & 0x7FFFu) == 0x7FFFu) continue;
                if (!(st->g161 & 2u) && !w->inside) continue;
                e[n].sy = w->sy; e[n].row = 0x0Fu; e[n].spr = w->spr;
                e[n].sx = w->sx; e[n].prow = -1; e[n].wy = w->y >> 16; n++;
            }
        }
        for (int k = 0; k < 2; k++) {  /* 0xF8F8-0xF970 near-rope authoring */
            int rx = k ? rope_w : rope_r;   /* referee's first (0x1F914 runs before 0xF4C2) */
            uint16_t pose; int32_t x, z;
            if (rx < 0) continue;
            if (rx >= 0x290 && rx < 0x3B0) { pose = 0; x = rx; z = 0x13E; }
            else if (rx < 0x290)           { pose = 1; x = 0x269; z = 0x146; }
            else                           { pose = 0x8001u; x = 0x3C7; z = 0x146; }
            e[n].sy = 0x151 + z - st->cam_y; e[n].row = 14u; e[n].spr = pose;
            e[n].sx = x - st->cam_x; e[n].prow = -1; e[n].wy = 0x151; n++;
        }
        if (st->ref.active && !rs) {
            e[n].sy = st->ref.sy; e[n].row = 12u; e[n].spr = st->ref.sm == 10 ? 0xFFFEu : st->ref.spr;   /* FFFE = try the ref_down badge */
            e[n].sx = st->ref.sx; e[n].prow = -1; e[n].wy = st->ref.y >> 16; n++;
        }
        if (st->ann.active) {          /* 0xA654 ring announcer, id 0x1F */
            e[n].sy = st->ann.sy; e[n].row = 0x1Fu; e[n].spr = st->ann.spr;
            e[n].sx = st->ann.sx; e[n].prow = -1; e[n].wy = st->ann.y >> 16; n++;
        }
        /* 0x10D3A companion sprites ($1C1258): enqueued by the owner's
         * handler, i.e. inside the object pass before the owner's own
         * 0x27B8 — same list-0 depth sort, own row, no partner slice. */
        for (int k = 0; k < ENG_FX_SLOTS; k++) {
            const eng_fx *f = &st->fx[k];
            if (!f->active || f->list != 0 || (f->spr & 0x7FFFu) == 0x7FFFu) continue;
            e[n].sy = f->sy; e[n].row = f->row; e[n].spr = f->spr;
            e[n].sx = f->sx; e[n].prow = -1; e[n].wy = f->y >> 16; n++;
        }
        for (int i = 0; i < ENG_MAX_OBJS; i++) {
            const eng_obj *o = &st->obj[i];
            int prow = -1;
            if (!o->active || (o->spr & 0x7FFFu) == 0x7FFFu)
                continue;              /* hidden-cell cull, 0x27C4 */
            if (o->st_flags & SF_QUEUED) continue;   /* queued rumble entrant: portrait below */
            if (o->apron || o->list == 4 || o->list == 2)
                continue;              /* already drawn in lists 4/2 */
            if (rs && !(o->role & RF_OUTSIDE))
                continue;              /* ringside scene: inside men drew in list 1 (0xF8D8) */
            if (o->grap44 & 0x8000u)
                continue;              /* lockup hidden half (+0x44 b15) */
            /* 0xD1FC resolves a DA12 cell's partner slice through +0x26
             * unconditionally; whether a cell uses it is the cell's own
             * business (the leg drop's E8 positions the foot across the
             * visible victim). */
            if (o->partner >= 0)
                prow = (int)eng_sprite_row(st->obj[o->partner].wrestler);
            e[n].sy = o->sy; e[n].row = eng_sprite_row(o->wrestler); e[n].spr = o->spr;
            e[n].sx = o->sx; e[n].prow = prow; e[n].wy = o->y >> 16;
            e[n].wctx = carried_type(st, o); e[n].oi = i;
            { int k = eng_calib_key(o); if (k >= 0) { e[n].ccls = eng_ws_body_class(o->wrestler); e[n].cmove = k; e[n].cframe = (int)(o->frame & 0xFFu); } }
            n++;
        }
        for (int i = 1; i < n; i++) {          /* stable insertion, desc sy */
            struct ent t = e[i]; int j = i - 1;
            while (j >= 0 && (getenv("WF_EMIT_REV") ? e[j].wy > t.wy : e[j].wy < t.wy)) { e[j + 1] = e[j]; j--; }
            e[j + 1] = t;
        }
        for (int i = 0; i < n; i++) {
            if (getenv("WF_ONLYROW") && e[i].row != (unsigned)atoi(getenv("WF_ONLYROW")))
                continue;              /* debug: draw one character only */
            if (getenv("WF_HIDEROW") && e[i].row == (unsigned)atoi(getenv("WF_HIDEROW")) && e[i].prow < 0)
                continue;              /* debug: hide one character's own object */
            if (eng_dbgsel && (st->frame % 4) == 0)
                fprintf(stderr, "emit[%d]: row %u spr %04X wy %X prow %d slot %u\n",
                        i, e[i].row, e[i].spr, e[i].wy, e[i].prow, slot);
            if (e[i].row == 12u && e[i].spr == 0xFFFEu) {          /* the referee KNOCKED OUT: external art */
                if (eng_badge_emit(eng_ref_down_badge(st), e[i].sx, e[i].sy, &slot)) continue;
                e[i].spr = st->ref.spr;                                /* none packed: the stock kneel */
            }
            eng_compose_ctx(e[i].ccls, e[i].cmove, e[i].cframe);   /* calibration context */
            eng_wpn_carry_ctx(e[i].wctx);
            emit_top = -0x7FFF; emit_x0 = 0x7FFF; emit_x1 = -0x7FFF;
            eng_sprite_emit_pose(e[i].row, e[i].spr, e[i].sx, e[i].sy,
                                 e[i].prow, &slot);
            if (e[i].oi >= 0 && emit_top > -0x7FFF) { obj_top[e[i].oi] = emit_top; obj_drawn[e[i].oi] = 1;   /* the man's head: where a chip floats */
                if (e[i].prow < 0) obj_cx[e[i].oi] = (emit_x0 + emit_x1) / 2;   /* alone: the picture's centre; in a two-man composite each keeps his own x */
                if (e[i].prow >= 0 && st->obj[e[i].oi].partner >= 0) {   /* the HIDDEN half of a pair is inside this composite */
                    int p = st->obj[e[i].oi].partner;
                    if (p >= 0 && p < ENG_MAX_OBJS && !obj_drawn[p]) { obj_top[p] = emit_top; obj_drawn[p] = 2; } } }
            eng_wpn_carry_ctx(-1);
            eng_compose_ctx(-1, -1, -1);
        }
    }
    /* 0x73C0/0x74C8: a queued rumble entrant shows his select-screen
     * portrait (row id + 0x10, pose +0x05) at screen (0x100,0x90), +0x0A = 0
     * so the list-0 sort leaves it on top. */
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        const eng_obj *o = &st->obj[i];
        if (!o->active || !(o->st_flags & SF_QUEUED)) continue;
        if (o->wrestler >= 12) continue;   /* a new wrestler has no portrait row: no entrant animation (user 2026-08-25) */
        eng_sprite_emit_pose(0x10u + (unsigned)eng_ws_base(o->wrestler), o->spr, 0x100, 0x90, -1, &slot);
    }
    eng_chips_emit(st, &slot);         /* 0x8710: the 1P-4P / CHANGE OVER chips
                                          over the heads, drawn last (on top) */
    /* The hardware buffers sprites on the 0x140008 write; the engine is
     * that write. */
    memcpy(wf.spriteram_buffered, wf.spriteram, WF_SPRRAM_SIZE);
}
