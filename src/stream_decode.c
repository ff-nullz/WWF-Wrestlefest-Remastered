/* C port of the 0xD1FC sprite compiler.
 *
 * Stack-accurate port of ROM D2AE / D802 / DA12 / DC3C + D43A / D540 / D71A.
 *
 * Two callers share this one copy so they can never drift:
 *   - tools/thinker builds it as libwfstream.so and composes poses offline
 *     from stream.bin, unclipped, against a dummy object.
 *   - the port compiles it in and drives it from the live machine
 *     (wf_thinker_init_reader / wf_thinker_decode_obj / wf_thinker_set_clip)
 *     to check its sprite list against what the 68k compiler actually wrote.
 *
 * It reads and writes only its own private mem[] image. It never touches
 * machine memory and never skips 0xD1FC by itself.
 */
#include "stream_decode.h"
#include "generated/rom_cycles.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tbl.h"

#define ROM_SIZE   0x80000u
#define MEM_SIZE   0x200000u
#define STACK_TOP  0x001C3E00u
#define DUMMY_OBJ  0x001C05B0u
#define BANK_TAB   0x001C1610u
#define SPR_BASE   0x000C2000u
#define TAB_6B     0x0006B0B0u   /* base of the shared sub-streams; every ROM byte this
                                    decoder reads lives in sprite.c's tables (sprite_row_
                                    pose_table_offsets / sprite_stream_ptrs / sprite_pose_
                                    offsets / sprite_streams) — the data layer resolves them */

static uint8_t mem[MEM_SIZE];
static uint32_t d[8], a[8];
static uint32_t g_entry_d[8];
static int g_entry_d_valid;
static WfThinkerSpr *g_out;
static int g_cap, g_n;
static char g_err[256];
static int g_ready;
static int16_t g_ox = 0x90, g_oy = 0x130;
static uint8_t g_flip, g_ex, g_ey;
static uint32_t g_stream_base;
static uint32_t g_stream_end;
static int g_nest;
static int g_ops;
static int g_clip;
/* ($12,A7,D7.w) / ($13,A7,D7.w): the per-partner X/Y bias 0xDA12 installs
 * before dispatching the partner's slice, and clears for its own body.
 * Added by every emit loop (00D316 / 00D334 in D2AE, 00D650 / 00D67A in
 * the D43A loop). Zero for every non-interaction stream. */
static int8_t g_off_x, g_off_y;
/* ($7,A7,D7.w): a palette bias added to the pen byte before it indexes
 * 0x1C1610. Applied by the D43A emit loop (00D6D0) and D71A (00D78E) only --
 * D2AE's and D802's own bodies do not use it. */
static int g_pal_bias;

/* Structural event counts for the cycle model. The decoder walks the same
 * path the ROM does, so what the ROM's cost is a function of, these are a
 * function of too. Regressing measured cost on these tells us the model
 * rather than making us guess it. */
enum {
    WF_EV_EMIT = 0,   /* sprite written to spriteram */
    WF_EV_CLIP,       /* sprite computed then dropped by the ring-edge clip */
    WF_EV_DISPATCH,   /* d2ae/d802/da12/dc3c entry */
    WF_EV_OVERLAY,    /* d43a/d540 entry */
    WF_EV_D71A,       /* d71a entry */
    WF_EV_FETCH,      /* fetch_ptr call */
    WF_EV_BIT9,       /* parse_bit9_header call */
    WF_EV_SLICE,      /* dc3c / da12 sub-slice iteration */
    WF_EV_FETCH0,     /* fetch_ptr by kind: inline / literal / word / word+8000 */
    WF_EV_FETCH1,
    WF_EV_FETCH2,
    WF_EV_FETCH3,
    WF_EV_CHAIN,      /* total chained tiles across emitted sprites */
    WF_EV_EMIT_D2AE,  /* emits split by the loop that produced them */
    WF_EV_EMIT_D802,
    WF_EV_EMIT_D71A,
    WF_EV_EMIT_D43A,
    WF_EV_EXTRA,      /* bit-9 extra bytes actually consumed */
    WF_EV_FLIPEMIT,   /* emits made while the flip bit is set: 00D24A bset and
                       * the per-sprite 00D322 neg.b cost extra */
    WF_EV_N
};
static long g_ev[WF_EV_N];

/* ---- cycle accounting ----------------------------------------------------
 *
 * What the ROM would have charged for the path this decode just walked.
 * Costs come from src/generated/rom_cycles.h, lifted out of the recompiler's
 * own output, so these are the 68000's numbers rather than anyone's estimate.
 *
 * Only routines that have been annotated contribute; g_cyc_partial marks a
 * decode that walked through one that has not, so a caller can decline to
 * charge rather than charge a number that is quietly short. */
static long g_cycles;
static int g_cyc_partial;

/* Which routine the charge belongs to, so a discrepancy can be attributed
 * rather than hunted. 0 d1fc, 1 d2ae, 2 d802, 3 fetch handlers, 4 other. */
#define CYCB_D1FC 0
#define CYCB_D2AE 1
#define CYCB_D802 2
#define CYCB_FETCH 3
#define CYCB_OTHER 4
#define CYCB_D43A 5
#define CYCB_D540 6
#define CYCB_LOOP 7
#define CYCB_DA12 8
#define CYCB_N 9
static long g_cyc_by[CYCB_N];
/* Model-cycle offset at which each record was emitted (see emit below). */
static long g_emit_cycle[WF_THINKER_MAX_SPR];
static int g_cyc_bucket = CYCB_OTHER;
static int g_saw_d540;   /* for targeting a trace at the trailer path */

/* Every PC the model charges, so a residual can be diffed against the PCs the
 * 68k actually executed (WF_D1FC_TRACE) instead of guessed at. Off unless a
 * caller turns it on. */
#define WF_CYC_PC_MAX 8192
static unsigned g_cyc_pc[WF_CYC_PC_MAX];
static short g_cyc_pc_cost[WF_CYC_PC_MAX];
static int g_cyc_npc, g_cyc_rec;

static void cyc_note(unsigned pc, int cost)
{
    if (g_cyc_rec && g_cyc_npc < WF_CYC_PC_MAX) {
        g_cyc_pc_cost[g_cyc_npc] = (short)cost;
        g_cyc_pc[g_cyc_npc++] = pc;
    }
}

static void cyc_run(unsigned from, unsigned to)
{
    int n = wf_rom_cycle_run(from, to);

    if (g_cyc_rec) {
        int i;

        for (i = 0; i < WF_ROM_CYCLES_N; i++)
            if (wf_rom_cycles[i].pc >= from && wf_rom_cycles[i].pc <= to)
                cyc_note(wf_rom_cycles[i].pc, wf_rom_cycles[i].cycles);
    }
    g_cycles += n;
    g_cyc_by[g_cyc_bucket] += n;
}

static void cyc_at(unsigned pc, int taken)
{
    int n = wf_rom_cycle_at(pc, taken);

    cyc_note(pc, n);
    g_cycles += n;
    g_cyc_by[g_cyc_bucket] += n;
}

static uint16_t g_partner_row = 0xffffu;
static uint8_t g_emitter;      /* routine currently emitting */
static uint8_t g_overlay;      /* inside a bit-9 / trailer overlay */
static uint8_t g_tag[WF_THINKER_MAX_SPR];
static long g_work;
static int g_trunc;
#define WF_THINKER_MAX_OPS 4096
/* dc3c/d43a recurse into sub-streams; nesting is capped but a desynced or
 * mis-seeked header can still spin a wide loop that never emits, which is
 * what hangs a full Crush export. Emission budgets do not cover that, so
 * bound total dispatch work too. */
#define WF_THINKER_MAX_WORK 65536

/* Charge one unit of decoder work. Returns 0 when the budget is spent, at
 * which point every loop must unwind rather than keep walking the stream. */
static int work_ok(void)
{
    if (g_work >= WF_THINKER_MAX_WORK) {
        g_trunc = 1;
        return 0;
    }
    g_work++;
    return 1;
}

int wf_thinker_last_truncated(void)
{
    return g_trunc;
}

/* Last 0xDA12 interaction seen: the partner row the decode used and the
 * per-partner X/Y bias it read. Carry moves (press, slam, headlock walk)
 * hang entirely off these, so they are what a misplaced victim points at. */
static int g_da12_seen, g_da12_row, g_da12_ox, g_da12_oy, g_da12_multi;

void wf_thinker_last_da12(int *seen, int *row, int *ox, int *oy, int *multi)
{
    if (seen)  *seen = g_da12_seen;
    if (row)   *row = g_da12_row;
    if (ox)    *ox = g_da12_ox;
    if (oy)    *oy = g_da12_oy;
    if (multi) *multi = g_da12_multi;
}

/* The bit-9 extras as they stood at the last emit, for attributing a
 * constant positional bias to the byte that produced it. */
static int8_t g_last_ex, g_last_ey;

static int in_own_stream(uint32_t p)
{
    if (!g_stream_base || g_stream_end <= g_stream_base)
        return 1;
    return p >= g_stream_base && p + 2u < g_stream_end;
}

static void d2ae(uint16_t header, uint32_t *a1);
static void d802(uint16_t header, uint32_t *a1);
static void da12(uint16_t header, uint32_t *a1);
static void dc3c(uint16_t header, uint32_t *a1);
static void dispatch(uint16_t header, uint32_t *a1);
static void d43a_body(uint32_t *src);

static void set_err(const char *s)
{
    snprintf(g_err, sizeof g_err, "%s", s);
}

const char *wf_thinker_error(void)
{
    return g_err[0] ? g_err : "";
}

static uint32_t mask(uint32_t addr)
{
    return addr & (MEM_SIZE - 1u);
}

/* Live engine: ROM-space reads come from the data layer (tbl.h), never a
 * private ROM copy; RAM (stack, dummy object, bank table) stays in mem[].
 * Offline tools (wf_thinker_init from chip files) keep the ROM in mem[]. */
static int g_live_tbl;
/* Highest ROM byte the live decoder has touched (WF_STREAM_SCAN in sprite.c
 * uses it to prove the sprite_streams table bound). */
static uint32_t g_rom_hwm, g_rom_lwm = 0xFFFFFFFFu;
uint32_t wf_thinker_rom_hwm(void) { return g_rom_hwm; }
uint32_t wf_thinker_rom_lwm(void) { return g_rom_lwm; }
void wf_thinker_rom_marks_reset(void) { g_rom_hwm = 0; g_rom_lwm = 0xFFFFFFFFu; }
static uint8_t r8(uint32_t addr)
{
    addr = mask(addr);
    if (g_live_tbl && addr < ROM_SIZE) {
        if (addr > g_rom_hwm) g_rom_hwm = addr;
        if (addr < g_rom_lwm) g_rom_lwm = addr;
        return (uint8_t)tbl_ra8(addr);
    }
    return mem[addr];
}

static uint16_t r16(uint32_t addr)
{
    addr = mask(addr);
    if (g_live_tbl && addr < ROM_SIZE) {
        if (addr + 1 > g_rom_hwm) g_rom_hwm = addr + 1;
        if (addr < g_rom_lwm) g_rom_lwm = addr;
        return (uint16_t)tbl_ra16(addr);
    }
    return (uint16_t)((mem[addr] << 8) | mem[addr + 1]);
}

static uint32_t r32(uint32_t addr)
{
    return ((uint32_t)r16(addr) << 16) | r16(addr + 2);
}

static void w8(uint32_t addr, uint8_t v)
{
    mem[mask(addr)] = v;
}

static void w16(uint32_t addr, uint16_t v)
{
    addr = mask(addr);
    mem[addr] = (uint8_t)(v >> 8);
    mem[addr + 1] = (uint8_t)v;
}

static void w32(uint32_t addr, uint32_t v)
{
    w16(addr, (uint16_t)(v >> 16));
    w16(addr + 2, (uint16_t)v);
}

static int16_t sx8(uint8_t v)
{
    return (int16_t)(int8_t)v;
}

static uint32_t set_b(uint32_t old, uint8_t value)
{
    return (old & 0xffffff00u) | value;
}

static uint32_t set_w(uint32_t old, uint16_t value)
{
    return (old & 0xffff0000u) | value;
}

static uint32_t ext_w(uint32_t old)
{
    return set_w(old, (uint16_t)(int16_t)(int8_t)old);
}

static uint16_t reg_size_word(uint16_t x, uint16_t y)
{
    unsigned borrow = (0x00ffu < (unsigned)y) ? 1u : 0u;
    uint16_t v = (uint16_t)((x & 0xff00u)
                  | (((x & 0x00ffu) >> 1) | (borrow << 7)));

    v = (uint16_t)(v >> 6);
    return (uint16_t)((v & 0xff00u) | ((uint8_t)v & 6u));
}

static uint32_t ea_d7(int16_t disp)
{
    return a[7] + (uint32_t)(int32_t)(int16_t)(d[7] & 0xffffu) + (uint32_t)(int32_t)disp;
}

/* High byte of the words the ROM stores at +2/+4/+6, set by whichever emit
 * loop is about to call emit_sprite(). See WfThinkerSpr. */
static uint8_t g_w2_hi, g_w4_hi, g_w6_hi;

/* The byte-stream loops (00D344 in D2AE, 00D762 in D71A) carry y in D2 and
 * 0xFF-y in D4, and replace only the low byte before each store. */
static void store_hi_from_y(int16_t y)
{
    g_w2_hi = (uint8_t)((uint16_t)y >> 8);
    g_w4_hi = g_w2_hi;
    g_w6_hi = (uint8_t)((uint16_t)(0x00ffu - (uint16_t)y) >> 8);
}

static void emit_sprite(int16_t x, int16_t y, uint16_t attr, uint8_t t_lo,
                        uint8_t t_hi, uint8_t pal)
{
    WfThinkerSpr s;
    unsigned chain, cmask, number;

    if (g_n >= g_cap || g_ops > WF_THINKER_MAX_OPS) {
        g_trunc = 1;
        return;
    }
    g_ops++;
    /* 00D6D8 indexes 0x1C1610 with the full byte, so a biased pen legitimately
     * exceeds 15. The caller resolves it through the live table. */
    memset(&s, 0, sizeof s);
    s.x = x;
    s.y = y;
    s.flipx = (uint8_t)((attr >> 4) & 1u);
    s.flipy = (uint8_t)((attr >> 3) & 1u);
    chain = (unsigned)(attr & 0x00e0u) >> 5;
    s.chain = (uint8_t)chain;
    s.pal = pal;
    cmask = ~((1u << (chain ? 32u - (unsigned)__builtin_clz(chain) : 0u)) - 1u);
    if (chain == 0)
        cmask = ~0u;
    else {
        unsigned w = 0, v = chain;
        while (v) {
            v >>= 1;
            w++;
        }
        cmask = ~((1u << w) - 1u);
    }
    number = ((unsigned)t_lo | ((unsigned)t_hi << 8)) & cmask;
    s.tile = (uint16_t)number;
    s.raw_tile_lo = t_lo;
    s.raw_tile_hi = t_hi;
    s.attr = (uint8_t)attr;
    s.w2_hi = g_w2_hi;
    s.w4_hi = g_w4_hi;
    s.w6_hi = g_w6_hi;
    g_ev[WF_EV_EMIT]++;
    if (g_flip)
        g_ev[WF_EV_FLIPEMIT]++;
    g_ev[WF_EV_CHAIN] += chain;
    g_ev[WF_EV_EMIT_D2AE + (g_emitter & 3u)]++;
    g_tag[g_n] = (uint8_t)(g_emitter | g_overlay);
    g_last_ex = (int8_t)g_ex;
    g_last_ey = (int8_t)g_ey;
    /* Model cycles spent when this record was emitted. The live replacement
     * publishes records as its stall drains, and needs to know *when* each one
     * would have appeared. A uniform fraction of the total is not good enough
     * — the emit paths differ in cost, and several decodes mix them — so the
     * offset is taken from the cycle model at the moment of emit. */
    if (g_n < WF_THINKER_MAX_SPR)
        g_emit_cycle[g_n] = g_cycles;
    g_out[g_n++] = s;
}

/* The 9th bits of x and y, which the hardware carries in the attribute word
 * (bit 1 = y8, bit 2 = x8) rather than alongside the coordinates.
 *
 * 00D694  move.w D0,D3        ; D3 = x
 * 00D696  moveq #0,D4 / not.b ; D4 = 0x00FF
 * 00D69A  sub.w  D1,D4        ; X := borrow, i.e. y > 0xFF -> y's 9th bit
 * 00D69C  roxr.b #1,D3        ; rotate X into bit 7 of D3's LOW BYTE only,
 *                             ; leaving the high byte (and so x's 9th bit
 *                             ; at bit 8) untouched
 * 00D69E  lsr.w  #6,D3
 * 00D6A0  andi.b #6,D3        ; bit 7 -> bit 1 (y8), bit 8 -> bit 2 (x8)
 *
 * Rotating the whole word, or feeding bit 0 of x instead of the borrow,
 * yields x's own bits 8 and 9 in those positions -- wrong for every sprite
 * that crosses x=255 or y=255. */
static uint8_t size_bits(int16_t x, int16_t y)
{
    unsigned xw = (unsigned)(uint16_t)x;
    unsigned yw = (unsigned)(uint16_t)y;
    unsigned borrow = (0x00ffu < yw) ? 1u : 0u;
    unsigned d3 = (xw & 0xff00u)
                | (((xw & 0x00ffu) >> 1) | (borrow << 7));

    d3 >>= 6;
    return (uint8_t)(d3 & 6u);
}

/* The two overlay routines use different fetcher tables:
 *   0xD43A -> 0xD3EA (D3FA/D402/D40C/D420), no bias
 *   0xD540 -> 0xD4EA (D4FA/D504/D50E/D524), every pointer biased by D4
 * D4 is (header low byte - parent count), which steps the trailer past the
 * parent's share of the shared data. Kind 1 is a literal byte and takes no
 * bias in either table. */
/* Which dispatch table the caller fetches through. The three tables hold
 * different handlers at different PCs, so the cycle charge differs even though
 * the C is shared. */
#define FETCH_TAB_D3EA 0   /* D2AE / D43A */
#define FETCH_TAB_D4EA 1   /* D540 */
#define FETCH_TAB_D7B2 2   /* D802 */
static const unsigned fetch_handler[3][4] = {
    { 0x00d3fau, 0x00d402u, 0x00d40cu, 0x00d420u },
    { 0x00d4fau, 0x00d504u, 0x00d50eu, 0x00d524u },
    { 0x00d7c2u, 0x00d7cau, 0x00d7d4u, 0x00d7e8u },
};
static const unsigned fetch_handler_end[3][4] = {
    { 0x00d400u, 0x00d40au, 0x00d41eu, 0x00d438u },
    { 0x00d502u, 0x00d50cu, 0x00d522u, 0x00d53eu },
    { 0x00d7c8u, 0x00d7d2u, 0x00d7e6u, 0x00d800u },
};
static int g_fetch_tab = FETCH_TAB_D3EA;

/* emit_from_ptrs is the C for two different ROM loops: D802's own at 00D8F0
 * and the one at 00D63C that D43A and D540 share. Same shape, different PCs
 * and therefore different costs. */
#define EMIT_LOOP_D63C 0
#define EMIT_LOOP_D802 1
static int g_emit_loop = EMIT_LOOP_D63C;

/* Each of the six streams is read as `move.l An,D1 / bmi skip /
 * move.b (An)+,D1`: a literal (high bit set) skips the read. */
static void cyc_stream_read(unsigned test_pc, unsigned br_pc, unsigned rd_pc,
                            uint32_t ptr)
{
    int literal = (ptr & 0x80000000u) != 0;

    cyc_at(test_pc, 0);
    cyc_at(br_pc, literal);
    if (!literal)
        cyc_at(rd_pc, 0);
}

static uint32_t fetch_ptr(uint32_t *ptr, int kind, uint32_t add, int16_t skip,
                          uint32_t d4)
{
    uint32_t p = *ptr;
    uint32_t val;
    uint32_t odd;

    g_ev[WF_EV_FETCH]++;
    cyc_run(fetch_handler[g_fetch_tab][kind & 3],
            fetch_handler_end[g_fetch_tab][kind & 3]);
    g_ev[WF_EV_FETCH0 + (kind & 3)]++;
    switch (kind & 3) {
    case 0:
        val = p;
        *ptr = p + (uint32_t)(uint16_t)skip;
        return val + d4;
    case 1:
        val = r8(p);
        *ptr = p + 1;
        return val | 0x80000000u;
    case 2:
        odd = p & 1u;
        val = r16(p + odd);
        *ptr = p + odd + 2;
        return val + add + d4;
    default:
        odd = p & 1u;
        val = r16(p + odd);
        *ptr = p + odd + 2;
        return val + add + 0x8000u + d4;
    }
}

/* roxr.w #first,Dn ; addx.w with 0 ; roxr.w #1 ; addx ; roxr.w #1 ; addx.
 * roxr rotates through X, so the bit addx picks up is the last one shifted
 * out, and addx's own carry-out is 0 here and clears X before the next
 * rotate. 0xD43A uses first = 4, 0xD540 uses first = 5. */
static unsigned roxr3_bits(uint16_t v, int first)
{
    int counts[3];
    unsigned x = 0, acc = 0;
    int step, b;

    counts[0] = first;
    counts[1] = 1;
    counts[2] = 1;
    for (step = 0; step < 3; step++) {
        for (b = 0; b < counts[step]; b++) {
            unsigned nx = v & 1u;

            v = (uint16_t)((v >> 1) | (x << 15));
            x = nx;
        }
        acc += x;
        x = 0;
    }
    return acc;
}

static uint8_t take_byte(uint32_t *ptr)
{
    uint32_t p = *ptr;
    uint8_t v;

    if (p & 0x80000000u)
        return (uint8_t)p;
    v = r8(p);
    *ptr = p + 1;
    return v;
}

static void skip_byte(uint32_t *ptr)
{
    if (!(*ptr & 0x80000000u))
        *ptr += 1;
}

static void emit_from_ptrs(uint32_t *px, uint32_t *py, uint32_t *pa,
                           uint32_t *ptile_a, uint32_t *ptile_b, uint32_t *ppal,
                           int extra_x, int extra_y, int count)
{
    int i;
    uint8_t flip = g_flip;
    uint8_t bx, by;

    if (count < 0)
        count = 0;
    if (count > 255)
        count = 255;   /* the ROM's dbra takes the count byte as-is (00D8F0 /
                          00D64A loops); the old defensive clamp of 32 cut the
                          LOD taunt panels' base pose short — row 0x2C pose 0
                          is one D802 record with count 0x55 (85 strips; MAME
                          spriteram ground truth 87 live records) and the cut
                          showed as "half of Hawk disappeared" (V438 playtest) */
    if (g_emit_loop == EMIT_LOOP_D802) {
        d[5] = set_w(d[5], (uint16_t)(count - 1));
        d[6] = 0x001c1610u;
    } else {
        d[6] = (uint32_t)(uint16_t)(count - 1);
        d[5] = 0x001c1610u;
    }
    /* 00D650/00D654 add.b, 00D660 neg.b, 00D662 ext.w, 00D664 add.w -- the
     * biases are folded in a BYTE at a time and the flip negates a byte, so
     * a sum that overflows int8 wraps before it is sign-extended. Widening
     * first (sx8 then 16-bit adds) silently disagrees whenever it does. */
    /* Only ($12)/($13) -- the DA12 partner bias -- is added by every emit
     * loop. The bit-9 extras ($5)/($6) are added by the D43A loop (00D650 /
     * 00D676) and D71A (00D730 / 00D752) but NOT by D2AE's or D802's own
     * bodies (00D316 / 00D334, 00D8F6 / 00D918), so the caller passes them
     * in rather than this reading the globals. Applying them everywhere let
     * a stale extra from a nested overlay leak into the next main body. */
    bx = (uint8_t)(extra_x + (int)(uint8_t)g_off_x);
    by = (uint8_t)(extra_y + (int)(uint8_t)g_off_y);
    if (g_emit_loop == EMIT_LOOP_D63C) {
        int save_b = g_cyc_bucket;

        /* 00D63C..00D648 sets the loop up once: the count from ($4,A7,D7),
         * the dbra bias and the 0x1C1610 base. The iteration proper starts
         * at 00D64A, which is also the dbra target. */
        g_cyc_bucket = CYCB_LOOP;
        cyc_run(0x00d63cu, 0x00d648u);
        g_cyc_bucket = save_b;
    }
    for (i = 0; i < count; i++) {
        int16_t x, y;
        uint8_t attr, t_a, t_b, sb, palb, v;
        uint32_t xreg = *px;

        if (g_ops > WF_THINKER_MAX_OPS) {
            g_trunc = 1;
            break;
        }
        g_ops++;
        if (g_emit_loop == EMIT_LOOP_D802) {
            cyc_stream_read(0x00d8f0u, 0x00d8f2u, 0x00d8f4u, *px);
            cyc_run(0x00d8f6u, 0x00d8fau);
            cyc_at(0x00d900u, !(flip & 0x10));
            if (flip & 0x10)
                cyc_at(0x00d902u, 0);
            cyc_run(0x00d904u, 0x00d90au);
        } else {
            g_cyc_bucket = CYCB_LOOP;
            /* 00D64A..00D668: the loop D43A and D540 share. */
            cyc_stream_read(0x00d64au, 0x00d64cu, 0x00d64eu, *px);
            cyc_run(0x00d650u, 0x00d654u);      /* the ($12) and ($5) biases */
            cyc_at(0x00d658u, 0);               /* btst #4 flip */
            cyc_at(0x00d65eu, !(flip & 0x10));  /* beq $d662 */
            if (flip & 0x10)
                cyc_at(0x00d660u, 0);           /* neg.b */
            cyc_run(0x00d662u, 0x00d668u);
        }
        v = (uint8_t)(take_byte(px) + bx);
        if (flip & 0x10)
            v = (uint8_t)(0u - v);
        x = (int16_t)((int16_t)(int8_t)v + g_ox);

        v = (uint8_t)(take_byte(py) + by);
        y = (int16_t)((int16_t)(int8_t)v + g_oy);

        if (g_emit_loop == EMIT_LOOP_D802) {
            int cx = g_clip && (uint16_t)x > 0x015fu;
            int cy = !cx && g_clip && (uint16_t)y > 0x0187u;

            cyc_at(0x00d90eu, cx);              /* bhi $d9e4 */
            if (!cx) {
                cyc_stream_read(0x00d912u, 0x00d914u, 0x00d916u, *py);
                cyc_run(0x00d918u, 0x00d922u);
                cyc_at(0x00d926u, cy);          /* bhi $d9ec */
            }
            if (!cx && !cy) {
                cyc_run(0x00d92au, 0x00d940u);
                cyc_stream_read(0x00d944u, 0x00d946u, 0x00d948u, *pa);
                cyc_run(0x00d94au, 0x00d952u);
                cyc_stream_read(0x00d954u, 0x00d956u, 0x00d958u, *ptile_a);
                cyc_at(0x00d95au, 0);
                cyc_stream_read(0x00d95cu, 0x00d95eu, 0x00d960u, *ptile_b);
                cyc_at(0x00d962u, 0);
                cyc_stream_read(0x00d964u, 0x00d966u, 0x00d968u, *ppal);
                cyc_run(0x00d96au, 0x00d97cu);
            } else {
                /* 00D9E4..00DA0E steps FIVE streams -- A3 y, A4 attr, A5 pen,
                 * A6 tile-hi, A1 tile-lo -- each behind its own bmi, then
                 * jmp $d980. A0 (x) is not stepped: 00D8F4 already did.
                 * An x-clip enters at 00D9E4, a y-clip at 00D9EC. */
                int lit_a1 = (*ptile_a & 0x80000000u) != 0;

                if (cx)
                    cyc_stream_read(0x00d9e4u, 0x00d9e6u, 0x00d9e8u, *py);
                cyc_stream_read(0x00d9ecu, 0x00d9eeu, 0x00d9f0u, *pa);
                cyc_stream_read(0x00d9f4u, 0x00d9f6u, 0x00d9f8u, *ppal);
                cyc_stream_read(0x00d9fcu, 0x00d9feu, 0x00da00u, *ptile_b);
                cyc_at(0x00da04u, 0);
                cyc_at(0x00da06u, lit_a1);      /* bmi straight to $d980 */
                if (!lit_a1) {
                    cyc_at(0x00da0au, 0);
                    cyc_at(0x00da0eu, 0);       /* jmp $d980 */
                }
            }
            cyc_at(0x00d980u, i + 1 < count);   /* dbra */
        } else {
            int cx = g_clip && (uint16_t)x > 0x015fu;
            int cy = !cx && g_clip && (uint16_t)y > 0x0187u;

            cyc_at(0x00d66cu, cx);              /* bhi $d6ee on x */
            if (!cx) {
                cyc_stream_read(0x00d670u, 0x00d672u, 0x00d674u, *py);
                cyc_run(0x00d676u, 0x00d684u);
                cyc_at(0x00d688u, cy);          /* bhi $d6f6 on y */
            }
            if (!cx && !cy) {
                cyc_run(0x00d68au, 0x00d6a0u);
                cyc_stream_read(0x00d6a4u, 0x00d6a6u, 0x00d6a8u, *pa);
                cyc_run(0x00d6aau, 0x00d6b2u);
                cyc_stream_read(0x00d6b4u, 0x00d6b6u, 0x00d6b8u, *ptile_a);
                cyc_at(0x00d6bau, 0);
                cyc_stream_read(0x00d6bcu, 0x00d6beu, 0x00d6c2u, *ptile_b);
                cyc_at(0x00d6c4u, 0);
                cyc_stream_read(0x00d6c6u, 0x00d6c8u, 0x00d6cau, *ppal);
                cyc_run(0x00d6ccu, 0x00d6e2u);
            } else {
                /* 00D6EE..00D716 steps A3 y, A4 attr, A5 pen, A6 tile-hi and
                 * A0 tile-lo, then jmp $d6e6. A1 (x) is not stepped. */
                int lit_a0 = (*ptile_a & 0x80000000u) != 0;

                if (cx)
                    cyc_stream_read(0x00d6eeu, 0x00d6f0u, 0x00d6f2u, *py);
                cyc_stream_read(0x00d6f6u, 0x00d6f8u, 0x00d6fau, *pa);
                cyc_stream_read(0x00d6feu, 0x00d700u, 0x00d702u, *ppal);
                cyc_stream_read(0x00d706u, 0x00d708u, 0x00d70au, *ptile_b);
                cyc_at(0x00d70eu, 0);
                cyc_at(0x00d710u, lit_a0);      /* bmi straight to $d6e6 */
                if (!lit_a0) {
                    cyc_at(0x00d712u, 0);
                    cyc_at(0x00d716u, 0);       /* jmp $d6e6 */
                }
            }
            cyc_at(0x00d6e6u, i + 1 < count);   /* dbra */
            g_cyc_bucket = CYCB_D802;
        }

        /* In-match clips off-screen writes. Export keeps every tile. */
        if (g_clip && ((uint16_t)x > 0x015fu || (uint16_t)y > 0x0187u)) {
            g_ev[WF_EV_CLIP]++;
            take_byte(pa);
            take_byte(ptile_a);
            take_byte(ptile_b);
            skip_byte(ppal);
            if (g_emit_loop == EMIT_LOOP_D802)
                d[5] = set_w(d[5], (uint16_t)(d[5] - 1u));
            else
                d[6] = set_w(d[6], (uint16_t)(d[6] - 1u));
            continue;
        }
        x = (int16_t)(x - 0x10);
        y = (int16_t)(y - 0x90);

        if (g_emit_loop == EMIT_LOOP_D802)
            d[3] = (xreg & 0xffff0000u) | (uint16_t)x;
        else
            d[3] = set_w(d[3], (uint16_t)x);
        d[4] = 0x000000ffu;
        d[4] = set_w(d[4], (uint16_t)(d[4] - (uint16_t)y));
        d[3] = set_w(d[3], reg_size_word((uint16_t)x, (uint16_t)y));
        d[3] = set_b(d[3], g_flip);
        d[4] = *ppal;                         /* 00D6D4 / 00D96E */

        sb = size_bits(x, y);
        /* 00D6A4/00D944 `move.l An,D1` loads the whole pointer before the
         * optional `move.b (An)+,D1` replaces its low byte, so the stored
         * word's high byte is the pointer's -- a literal writes its own
         * address into spriteram. Capture before take_byte advances them. */
        g_w2_hi = (uint8_t)(*pa >> 8);
        g_w4_hi = (uint8_t)(*ptile_a >> 8);
        g_w6_hi = (uint8_t)(*ptile_b >> 8);
        attr = (uint8_t)(take_byte(pa) | sb);
        attr = (uint8_t)(attr ^ flip);
        t_a = take_byte(ptile_a);
        t_b = take_byte(ptile_b);
        palb = (uint8_t)(take_byte(ppal) + (unsigned)g_pal_bias);
        emit_sprite(x, y, attr, t_a, t_b, palb);
        if (g_emit_loop == EMIT_LOOP_D802)
            d[5] = set_w(d[5], (uint16_t)(d[5] - 1u));
        else
            d[6] = set_w(d[6], (uint16_t)(d[6] - 1u));
    }
}

static void parse_bit9_header(uint32_t *a1, uint32_t *src)
{
    g_ev[WF_EV_BIT9]++;
    uint16_t w0, w1;
    uint8_t flags, count;

    count = r8(*a1);
    w8(ea_d7(4), count);
    /* 00D2C0..00D2C8 (and 00D816..00D81E in D802) clear all three extra
     * slots before the flags are examined, so an absent flag means zero --
     * not "whatever the last header set". g_ex/g_ey shadow slots 5 and 6 for
     * the emit paths and have to be cleared with them, or a stale bias from
     * an earlier slice silently displaces this one. */
    w8(ea_d7(5), 0);
    w8(ea_d7(6), 0);
    w8(ea_d7(7), 0);
    g_ex = 0;
    g_ey = 0;
    w0 = r16(*a1);
    *a1 += 2;
    flags = (uint8_t)w0;
    w1 = r16(*a1);
    *a1 += 2;
    *src = ((uint32_t)(w0 & 0x000fu) << 16) | w1;
    d[2] = set_w(d[2], w0);
    d[2] = set_b(d[2], (uint8_t)d[2] & 0x0fu);
    d[2] = (d[2] << 16) | (d[2] >> 16);
    d[2] = set_w(d[2], w1);
    if (flags & 0x40) {
        g_ev[WF_EV_EXTRA]++;
        g_ex = r8(*a1);
        w8(ea_d7(5), r8((*a1)++));
    }
    if (flags & 0x20) {
        g_ev[WF_EV_EXTRA]++;
        g_ey = r8(*a1);
        w8(ea_d7(6), r8((*a1)++));
    }
    if (flags & 0x10) {
        g_ev[WF_EV_EXTRA]++;
        w8(ea_d7(7), r8((*a1)++));
    }
}

/* The trailer entries at 00D396 (D2AE) and 00D990 (D802) align the cursor to
 * an even address before reading the overlay header; the bit-9 pre-overlay at
 * 00D2BC / 00D812 does not. Reading that header one byte off makes every
 * command the trailer emits garbage. */
static void parse_trailer_header(uint32_t *a1, uint32_t *src)
{
    *a1 += (*a1 & 1u);
    parse_bit9_header(a1, src);
}

static void d71a(uint32_t *src)
{
    uint8_t save_emit = g_emitter;
    int count = (int)r8(ea_d7(4));

    g_emitter = WF_THINKER_TAG_D71A;
    g_ev[WF_EV_D71A]++;
    /* D71A: dbra uses count-1. 0 means one sprite was stored as 1. */
    uint8_t bx = (uint8_t)((int)(uint8_t)g_ex + (int)(uint8_t)g_off_x);
    uint8_t by = (uint8_t)((int)(uint8_t)g_ey + (int)(uint8_t)g_off_y);
    uint8_t flip = g_flip;
    int i;

    if (count <= 0)
        count = 1;
    if (count > 255)
        count = 255;   /* byte count, no ROM clamp (00D71A loop) */
    {
        int save_b = g_cyc_bucket;

        g_cyc_bucket = CYCB_OTHER;
        cyc_run(0x00d71au, 0x00d722u);          /* count read, table ptr */
        g_cyc_bucket = save_b;
    }
    for (i = 0; i < count; i++) {
        int16_t x, y;
        uint8_t attr, pal, t_b, t_a, sb, v;
        int cx, cy;
        int save_b = g_cyc_bucket;

        if (g_ops > WF_THINKER_MAX_OPS) {
            g_trunc = 1;
            break;
        }
        g_ops++;
        d[6] = 5;
        g_cyc_bucket = CYCB_OTHER;
        cyc_run(0x00d728u, 0x00d730u);          /* moveq, x byte, two biases */
        cyc_at(0x00d734u, 0);                   /* btst #4 flip */
        cyc_at(0x00d73au, !(flip & 0x10));      /* beq $d73e */
        if (flip & 0x10)
            cyc_at(0x00d73cu, 0);               /* neg.b */
        cyc_run(0x00d73eu, 0x00d744u);
        /* 00D72A..00D740: add.b / add.b / neg.b / ext.w / add.w. */
        v = (uint8_t)(r8((*src)++) + bx);
        if (flip & 0x10)
            v = (uint8_t)(0u - v);
        x = (int16_t)((int16_t)(int8_t)v + g_ox);

        v = (uint8_t)(r8((*src)++) + by);
        y = (int16_t)((int16_t)(int8_t)v + g_oy);

        cx = g_clip && (uint16_t)x > 0x015fu;
        cy = !cx && g_clip && (uint16_t)y > 0x0187u;
        cyc_at(0x00d748u, cx);                  /* bhi $d7a6 on x */
        if (!cx) {
            cyc_run(0x00d74au, 0x00d75cu);      /* y byte and its two biases */
            cyc_at(0x00d760u, cy);              /* bhi $d7a6 on y */
        }
        if (!cx && !cy)
            cyc_run(0x00d762u, 0x00d7a2u);      /* the six spriteram words */
        /* 00D7A6 lea (A0,D6.w),A0 runs on every path; D6 carries how much of
         * the record the clip skipped. */
        cyc_run(0x00d7a6u, 0x00d7a6u);
        cyc_at(0x00d7aau, i + 1 < count);       /* dbra */
        g_cyc_bucket = save_b;

        if (cx || cy) {
            g_ev[WF_EV_CLIP]++;
            if (!cx)
                d[6] = set_w(d[6], 4);
            d[2] = set_b(d[2], v);
            d[2] = ext_w(d[2]);
            d[2] = set_w(d[2], (uint16_t)y);
            *src += 4;
            continue;
        }
        x = (int16_t)(x - 0x10);
        y = (int16_t)(y - 0x90);

        d[2] = set_w(ext_w(set_b(d[2], v)), (uint16_t)y);
        d[6] = set_w(d[6], 0);
        d[3] = set_w(d[3], (uint16_t)x);
        d[4] = 0x000000ffu;
        d[4] = set_w(d[4], (uint16_t)(d[4] - (uint16_t)y));
        d[3] = set_w(d[3], reg_size_word((uint16_t)x, (uint16_t)y));

        sb = size_bits(x, y);
        store_hi_from_y(y);
        attr = (uint8_t)(r8((*src)++) | sb);
        attr = (uint8_t)(attr ^ flip);
        pal = (uint8_t)(r8((*src)++) + (unsigned)r8(ea_d7(7)));  /* 00D78E */
        t_b = r8((*src)++);
        t_a = r8((*src)++);
        d[2] = set_b(d[2], t_a);
        d[4] = set_b(d[4], t_b);
        d[3] = set_w(d[3], 0);
        d[3] = set_b(d[3], r8(BANK_TAB + pal));
        emit_sprite(x, y, attr, t_a, t_b, pal);
    }
    {
        int save_b = g_cyc_bucket;

        g_cyc_bucket = CYCB_OTHER;
        cyc_run(0x00d7aeu, 0x00d7b0u);          /* subq #4,D7 and rts */
        g_cyc_bucket = save_b;
    }
    g_emitter = save_emit;
}

static void d43a(uint32_t *src)
{
    g_ev[WF_EV_OVERLAY]++;
    if (g_nest > 6) {
        g_trunc = 1;
        return;
    }
    if (!work_ok())
        return;
    g_nest++;
    /* 00D2FA / 00D852 reach D43A through jsr, which pushes a 4-byte return
     * address. D43A's own addq.w #4,D7 exists to cancel exactly that, so
     * ($4,A7,D7) inside D71A lands back on the byte D2AE/D802 wrote the
     * sprite count into at 00D2BC. Model the push or the slot is off by
     * four and D71A reads a stale byte. */
    a[7] -= 4;
    d43a_body(src);
    a[7] += 4;
    g_nest--;
}

/* 0xD540 -- the header-bit-8 trailer overlay. Reached by jsr from 00D3DC
 * (D2AE) and 00D9D6 (D802). It is NOT 0xD43A: it biases every fetched
 * pointer by D4 = (header low byte - parent count), rotates its bit-9 skip
 * as #5/#1/#1, and in its uncompressed branch steps over 6 bytes per unit of
 * that difference before handing off to D71A. Treating it as D43A made every
 * trailer command wrong. */
static void d540_body(uint32_t *src)
{
    g_saw_d540 = 1;
    uint16_t hdr, pack;
    uint32_t p0, p1, p2, p3, p4, p5;
    int count, stride, diff;
    uint32_t d4;

    d[7] = (d[7] + 4) & 0xffffffffu;          /* 00D540 addi.w #4,D7 */
    hdr = r16(*src);
    *src += 2;
    count = (int)r8(ea_d7(4));                /* 00D56E / 00D58C */
    stride = (int)(hdr & 0xffu);
    diff = (int)((uint8_t)(stride - count));  /* sub.b, byte-wide */

    {
        int save_b = g_cyc_bucket;

        g_cyc_bucket = CYCB_D540;
        cyc_run(0x00d540u, 0x00d546u);          /* addi, header, btst #14 */
        cyc_at(0x00d54au, (hdr & 0x4000u) != 0);/* bne $d588 */
        g_cyc_bucket = save_b;
    }
    if (!(hdr & 0x4000u)) {
        int save_b = g_cyc_bucket;

        g_cyc_bucket = CYCB_D540;
        cyc_run(0x00d54eu, 0x00d54eu);          /* btst #9 */
        cyc_at(0x00d552u, !(hdr & 0x0200u));    /* beq $d56e */
        if (hdr & 0x0200u)
            cyc_run(0x00d554u, 0x00d56au);      /* roxr #5,#1,#1 skip */
        cyc_run(0x00d56eu, 0x00d584u);          /* the 6*diff skip, jmp $d71a */
        g_cyc_bucket = save_b;
        if (hdr & 0x0200u) {
            uint16_t d1 = r16(*src);

            d[2] = 0;
            d[3] = 0;
            *src += 2;                         /* 00D554 move.w (A0)+,D1 */
            *src += 2;                         /* 00D556 lea ($2,A0),A0  */
            *src += roxr3_bits(d1, 5);         /* 00D56A lea (A0,D2.w),A0 */
        }
        /* 00D57A lsl.w #2 / 00D57C lsl.w #1 / 00D57E add.w -> 6 * diff. */
        d[2] = set_b(d[2], (uint8_t)count);
        d[3] = set_w(d[3], 0);
        d[3] = set_b(d[3], (uint8_t)diff);
        d[2] = set_w(d[2], (uint16_t)d[3]);
        d[3] = set_w(d[3], (uint16_t)((uint16_t)d[3] << 2));
        d[2] = set_w(d[2], (uint16_t)((uint16_t)d[2] << 1));
        d[3] = set_w(d[3], (uint16_t)(d[3] + d[2]));
        *src += (uint32_t)((unsigned)(6 * diff) & 0xffffu);
        d71a(src);                             /* 00D584 jmp $d71a */
        d[7] = (d[7] - 4) & 0xffffffffu;
        return;
    }

    d[5] = set_w(d[5], 0);
    d[5] = set_b(d[5], (uint8_t)hdr);
    d[2] = set_b(d[2], (uint8_t)count);
    d[4] = 0;
    d[4] = set_b(d[4], (uint8_t)diff);
    d[6] = TAB_6B;
    d4 = (uint32_t)diff;                       /* 00D594 sub.b D2,D4 */
    {
        int save_b = g_cyc_bucket;

        g_cyc_bucket = CYCB_D540;
        cyc_run(0x00d588u, 0x00d59cu);          /* D5, parent count, D4, D6 */
        cyc_at(0x00d5a0u, !(hdr & 0x0200u));    /* beq $d5c2 */
        if (hdr & 0x0200u)
            cyc_run(0x00d5a2u, 0x00d5beu);      /* roxr skip, rounded even */
        /* 00D5C2..00D638: pack word, six jsr fetches through the $D4EA table
         * (handlers charged in fetch_ptr), then bra $d63c. */
        cyc_run(0x00d5c2u, 0x00d638u);
        g_cyc_bucket = save_b;
    }
    if (hdr & 0x0200u) {
        uint16_t d0 = r16(*src);
        unsigned n;

        d[2] = 0;
        *src += 2;                             /* 00D5A2 */
        *src += 2;                             /* 00D5A4 */
        n = roxr3_bits(d0, 5);
        n = (n + 1u) & 0xfeu;                  /* 00D5B8 addq.b / 00D5BA andi.b */
        *src += n;                             /* 00D5BE */
    }
    pack = r16(*src);                          /* 00D5C4 */
    *src += 2;
    g_fetch_tab = FETCH_TAB_D4EA;
    p0 = fetch_ptr(src, pack & 3, TAB_6B, (int16_t)stride, d4);
    p1 = fetch_ptr(src, (pack >> 2) & 3, TAB_6B, (int16_t)stride, d4);
    p2 = fetch_ptr(src, (pack >> 4) & 3, TAB_6B, (int16_t)stride, d4);
    p3 = fetch_ptr(src, (pack >> 6) & 3, TAB_6B, (int16_t)stride, d4);
    p4 = fetch_ptr(src, (pack >> 8) & 3, TAB_6B, (int16_t)stride, d4);
    p5 = fetch_ptr(src, (pack >> 10) & 3, TAB_6B, (int16_t)stride, d4);
    d[3] = set_w(d[3], (uint16_t)(((pack >> 10) & 3) << 2));
    g_fetch_tab = FETCH_TAB_D3EA;
    {
        uint8_t save_emit = g_emitter;

        g_emitter = WF_THINKER_TAG_D43A;
        g_pal_bias = (int)r8(ea_d7(7));
        emit_from_ptrs(&p0, &p1, &p2, &p5, &p4, &p3,
                       (int)(uint8_t)g_ex, (int)(uint8_t)g_ey, count);
        g_pal_bias = 0;
        g_emitter = save_emit;
    }
    {
        int save_b = g_cyc_bucket;

        g_cyc_bucket = CYCB_OTHER;
        cyc_run(0x00d6eau, 0x00d6ecu);          /* the shared loop's exit */
        g_cyc_bucket = save_b;
    }
    d[7] = (d[7] - 4) & 0xffffffffu;
}

static void d540(uint32_t *src)
{
    g_ev[WF_EV_OVERLAY]++;
    if (g_nest > 6) {
        g_trunc = 1;
        return;
    }
    if (!work_ok())
        return;
    g_nest++;
    a[7] -= 4;                                 /* jsr return address */
    d540_body(src);
    a[7] += 4;
    g_nest--;
}

static void d43a_body(uint32_t *src)
{
    uint16_t hdr, pack;
    uint32_t p0, p1, p2, p3, p4, p5;
    int count, stride;
    int extra_x, extra_y;

    d[7] = (d[7] + 4) & 0xffffffffu;
    hdr = r16(*src);
    *src += 2;
    {
        int save_b = g_cyc_bucket;

        g_cyc_bucket = CYCB_D43A;
        cyc_run(0x00d43au, 0x00d43eu);          /* addq, header, btst #14 */
        cyc_at(0x00d442u, !(hdr & 0x4000u));    /* beq $d71a */
        g_cyc_bucket = save_b;
    }
    if (!(hdr & 0x4000u)) {
        /* 00D442 beq $d71a: straight through, the count slot untouched.
         * D71A emits the parent's count, not this overlay's header byte —
         * overwriting the slot here made the overlay spray extra tiles. */
        d71a(src);
        d[7] = (d[7] - 4) & 0xffffffffu;
        return;
    }
    {
        int save_b = g_cyc_bucket;

        g_cyc_bucket = CYCB_D43A;
        cyc_run(0x00d446u, 0x00d446u);          /* btst #9 */
        cyc_at(0x00d44au, !(hdr & 0x0200u));    /* beq $d466 */
        if (hdr & 0x0200u)
            cyc_run(0x00d44cu, 0x00d462u);      /* the roxr skip block */
        cyc_run(0x00d466u, 0x00d472u);          /* count, 6B0B0, pack, push A2 */
        g_cyc_bucket = save_b;
    }
    if (hdr & 0x0200u) {
        /* 00D44C move.w (A0)+,D1 / 00D44E lea ($2,A0),A0, then three times
         * roxr.w #4,D1 + addx.w D3,D2 with D3 = 0, then lea (A0,D2.w),A0.
         *
         * roxr rotates THROUGH X: each single step takes bit 0 out into X
         * and feeds the old X back in at bit 15, so after #4 the X that
         * addx picks up is the fourth bit shifted out, not bit 0. addx's
         * own carry-out is 0 here (D2 is tiny, D3 is 0), which clears X
         * again before the next rotate. */
        uint16_t d1 = r16(*src);

        d[2] = 0;
        d[3] = 0;
        *src += 2;
        *src += 2;
        *src += roxr3_bits(d1, 4);
    }
    pack = r16(*src);
    *src += 2;
    /* Two different numbers, and conflating them is what made this overlay
     * spray tiles over good poses:
     *
     *   D5 = header low byte  -> only the kind-0 fetcher stride
     *                            (00D4FE lea (A0,D5.w),A0)
     *   ($4,A7,D7) = parent's count -> the emit loop bound
     *                            (00D63E move.b ($4,A7,D7.w),D6)
     *
     * The header byte is routinely larger than the parent count, so using
     * it for the loop walked past the real data and emitted plausible but
     * non-existent tiles. */
    d[5] = set_w(d[5], 0);
    d[5] = set_b(d[5], (uint8_t)hdr);
    d[6] = TAB_6B;
    stride = (int)(hdr & 0xffu);
    {
        int save_b = g_cyc_bucket;

        /* 00D474..00D4E6: six jsr-dispatched fetches through the $D3EA table
         * (each handler charged inside fetch_ptr), then bra $d63c. */
        g_cyc_bucket = CYCB_D43A;
        cyc_run(0x00d474u, 0x00d4e6u);
        g_cyc_bucket = save_b;
    }
    p0 = fetch_ptr(src, pack & 3, TAB_6B, (int16_t)stride, 0);
    p1 = fetch_ptr(src, (pack >> 2) & 3, TAB_6B, (int16_t)stride, 0);
    p2 = fetch_ptr(src, (pack >> 4) & 3, TAB_6B, (int16_t)stride, 0);
    p3 = fetch_ptr(src, (pack >> 6) & 3, TAB_6B, (int16_t)stride, 0);
    p4 = fetch_ptr(src, (pack >> 8) & 3, TAB_6B, (int16_t)stride, 0);
    p5 = fetch_ptr(src, (pack >> 10) & 3, TAB_6B, (int16_t)stride, 0);
    d[3] = set_w(d[3], (uint16_t)(((pack >> 10) & 3) << 2));
    count = (int)r8(ea_d7(4));
    extra_x = (int)(uint8_t)g_ex;             /* 00D654 add.b ($5,A7,D7.w) */
    extra_y = (int)(uint8_t)g_ey;             /* 00D676 add.b ($6,A7,D7.w) */
    {
        uint8_t save_emit = g_emitter;

        g_emitter = WF_THINKER_TAG_D43A;
        g_pal_bias = (int)r8(ea_d7(7));           /* 00D6D0 */
        emit_from_ptrs(&p0, &p1, &p2, &p5, &p4, &p3, extra_x, extra_y, count);
        g_pal_bias = 0;
        g_emitter = save_emit;
    }
    {
        int save_b = g_cyc_bucket;

        g_cyc_bucket = CYCB_OTHER;
        cyc_run(0x00d6eau, 0x00d6ecu);          /* subq #4,D7 and rts */
        g_cyc_bucket = save_b;
    }
    d[7] = (d[7] - 4) & 0xffffffffu;
}

static void d2ae(uint16_t header, uint32_t *a1)
{
    int count, i;
    int extra_x, extra_y;
    uint8_t flip;

    a[7] -= 2;
    w16(a[7], header);
    a[7] -= 4;
    w32(a[7], *a1);
    d[7] += 0x0au;

    g_cyc_bucket = CYCB_D2AE;
    cyc_run(0x00d2aeu, 0x00d2b6u);              /* push, push, addi, btst */
    cyc_at(0x00d2bau, (header & 0x0200u) == 0); /* beq $d300 */

    if (header & 0x0200u) {
        uint32_t src = 0;
        uint8_t flags9;

        /* 00D2BC..00D2D8: count and the three extra slots, then the two
         * header words. The three optional extra bytes each cost a btst, a
         * branch, and a move when present. */
        cyc_run(0x00d2bcu, 0x00d2d8u);
        flags9 = (uint8_t)r16(*a1);
        cyc_at(0x00d2dau, 0);                   /* btst #6 */
        cyc_at(0x00d2deu, !(flags9 & 0x40));    /* beq $d2e4 */
        if (flags9 & 0x40)
            cyc_at(0x00d2e0u, 0);
        cyc_at(0x00d2e4u, 0);
        cyc_at(0x00d2e8u, !(flags9 & 0x20));
        if (flags9 & 0x20)
            cyc_at(0x00d2eau, 0);
        cyc_at(0x00d2eeu, 0);
        cyc_at(0x00d2f2u, !(flags9 & 0x10));
        if (flags9 & 0x10)
            cyc_at(0x00d2f4u, 0);
        cyc_run(0x00d2f8u, 0x00d2feu);          /* save A1, jsr d43a, restore */

        /* 68k always follows bit-9. Own-span skip dropped Hogan hair
         * (pointer into Hawk's stream — shared tiles). */
        parse_bit9_header(a1, &src);
        if (src >= 0x200u && src + 2u < ROM_SIZE)
        {
            uint8_t save_ov = g_overlay;

            g_overlay = WF_THINKER_TAG_OVERLAY;
            d43a(&src);
            g_overlay = save_ov;
        }
        g_ex = 0;
        g_ey = 0;
    }

    count = (int)(header & 0xffu);
    /* 00D316 / 00D334 add ($12)/($13) and nothing else. */
    extra_x = 0;
    extra_y = 0;
    flip = g_flip;

    if (count < 0)
        count = 0;
    if (count > 255)
        count = 255;   /* byte count, no ROM clamp (00D300 loop) */

    g_emitter = WF_THINKER_TAG_D2AE;
    /* 00D300..00D308: the count is read and tested before the loop. */
    cyc_run(0x00d300u, 0x00d306u);
    cyc_at(0x00d308u, count == 0);              /* bmi $d38c */
    if (count > 0)
        cyc_at(0x00d30cu, 0);                   /* lea $1c1610,A3 */
    {
        uint8_t bx = (uint8_t)(extra_x + (int)(uint8_t)g_off_x);
        uint8_t by = (uint8_t)(extra_y + (int)(uint8_t)g_off_y);

    for (i = 0; i < count; i++) {
        int16_t x, y;
        uint8_t attr, pal, t_b, t_a, sb, v;
        int clipx, clipy;

        d[6] = 5;                              /* 00D312 moveq #5,D6 */
        /* 00D312..00D32E: x is built, flipped and range-checked. */
        cyc_run(0x00d312u, 0x00d316u);
        cyc_at(0x00d31au, 0);                   /* btst #4 of the flip byte */
        cyc_at(0x00d320u, !(flip & 0x10));      /* beq $d324 */
        if (flip & 0x10)
            cyc_at(0x00d322u, 0);               /* neg.b */
        cyc_run(0x00d324u, 0x00d32au);

        /* 00D314..00D326 / 00D332..00D33A: byte adds, byte negate, then
         * ext.w -- see emit_from_ptrs. */
        v = (uint8_t)(r8((*a1)++) + bx);
        if (flip & 0x10)
            v = (uint8_t)(0u - v);
        x = (int16_t)((int16_t)(int8_t)v + g_ox);

        v = (uint8_t)(r8((*a1)++) + by);
        y = (int16_t)((int16_t)(int8_t)v + g_oy);

        clipx = g_clip && (uint16_t)x > 0x015fu;
        clipy = !clipx && g_clip && (uint16_t)y > 0x0187u;
        cyc_at(0x00d32eu, clipx);               /* bhi $d384 on x */
        if (!clipx) {
            d[6] = set_w(d[6], 4);              /* 00D330 subq.w #1,D6 */
            d[2] = set_b(d[2], v);
            d[2] = ext_w(d[2]);
            d[2] = set_w(d[2], (uint16_t)y);
            cyc_run(0x00d330u, 0x00d33eu);      /* y built and checked */
            cyc_at(0x00d342u, clipy);           /* bhi $d384 on y */
        }
        if (!clipx && !clipy)
            cyc_run(0x00d344u, 0x00d380u);      /* the six spriteram words */
        cyc_run(0x00d384u, 0x00d384u);          /* lea (A1,D6.w),A1 */
        cyc_at(0x00d388u, i + 1 < count);       /* dbra */

        if (clipx || clipy) {
            g_ev[WF_EV_CLIP]++;
            (*a1) += 4;
            continue;
        }
        x = (int16_t)(x - 0x10);
        y = (int16_t)(y - 0x90);

        d[2] = set_w(d[2], (uint16_t)y);
        d[6] = set_w(d[6], 0);
        d[3] = set_w(d[3], (uint16_t)x);
        d[4] = 0x000000ffu;
        d[4] = set_w(d[4], (uint16_t)(d[4] - (uint16_t)y));
        d[3] = set_w(d[3], reg_size_word((uint16_t)x, (uint16_t)y));

        sb = size_bits(x, y);
        store_hi_from_y(y);
        attr = (uint8_t)(r8((*a1)++) | sb);
        attr = (uint8_t)(attr ^ flip);
        pal = r8((*a1)++);
        t_b = r8((*a1)++);
        t_a = r8((*a1)++);
        d[2] = set_b(d[2], t_a);
        d[4] = set_b(d[4], t_b);
        d[3] = set_w(d[3], 0);
        d[3] = set_b(d[3], r8(BANK_TAB + pal));
        emit_sprite(x, y, attr, t_a, t_b, pal);
    }
    }

    /* 00D38C/00D390: btst #0 of the pushed header's high byte = header bit 8.
     * The trailer runs 0xD540, not 0xD43A, from the current cursor. */
    cyc_run(0x00d38cu, 0x00d390u);
    cyc_at(0x00d394u, (header & 0x0100u) == 0); /* beq $d3e0 */
    if (header & 0x0100u) {
        /* 00D396..00D3DC: align the cursor, take the count and the three
         * optional extra bytes, then jsr $d540. */
        uint32_t cur = *a1 + (*a1 & 1u);
        /* The flags are the low byte of the FIRST header word: 00D3B0
         * move.w (A1)+,D2 then 00D3B2 move.b D2,D1, and the btsts test D1. */
        uint8_t f = (uint8_t)r16(cur);

        cyc_run(0x00d396u, 0x00d3a0u);
        cyc_run(0x00d3a4u, 0x00d3bcu);          /* ends at movea.l D2,A0 */
        cyc_at(0x00d3beu, 0);
        cyc_at(0x00d3c2u, !(f & 0x40));
        if (f & 0x40)
            cyc_at(0x00d3c4u, 0);
        cyc_at(0x00d3c8u, 0);
        cyc_at(0x00d3ccu, !(f & 0x20));
        if (f & 0x20)
            cyc_at(0x00d3ceu, 0);
        cyc_at(0x00d3d2u, 0);
        cyc_at(0x00d3d6u, !(f & 0x10));
        if (f & 0x10)
            cyc_at(0x00d3d8u, 0);
        cyc_at(0x00d3dcu, 0);                   /* jsr $d540 */
    }
    if (header & 0x0100u) {
        uint32_t src = 0;
        uint32_t cur = *a1;

        parse_trailer_header(&cur, &src);
        if (src >= 0x200u && src + 2u < ROM_SIZE)
        {
            uint8_t save_ov = g_overlay;

            g_overlay = (uint8_t)(WF_THINKER_TAG_OVERLAY | WF_THINKER_TAG_TRAILER);
            d540(&src);
            g_overlay = save_ov;
        }
    }

    cyc_run(0x00d3e0u, 0x00d3e8u);              /* subi.b, lea, rts */

    d[7] -= 0x0au;
    a[7] += 6;
}

static void d802(uint16_t header, uint32_t *a1)
{
    uint16_t pack;
    uint32_t p0, p1, p2, p3, p4, p5;
    int count;
    uint32_t src;
    int extra_x, extra_y;

    a[7] -= 2;
    w16(a[7], header);
    a[7] -= 4;
    d[7] += 0x0au;

    g_cyc_bucket = CYCB_D802;
    cyc_run(0x00d802u, 0x00d80cu);              /* push, lea, addi, btst */
    cyc_at(0x00d810u, (header & 0x0200u) == 0); /* beq $d858 */

    if (header & 0x0200u) {
        uint8_t flags9 = (uint8_t)r16(*a1);

        cyc_run(0x00d812u, 0x00d82eu);
        cyc_at(0x00d830u, 0);
        cyc_at(0x00d834u, !(flags9 & 0x40));
        if (flags9 & 0x40)
            cyc_at(0x00d836u, 0);
        cyc_at(0x00d83au, 0);
        cyc_at(0x00d83eu, !(flags9 & 0x20));
        if (flags9 & 0x20)
            cyc_at(0x00d842u, 0);
        cyc_at(0x00d846u, 0);
        cyc_at(0x00d84au, !(flags9 & 0x10));
        if (flags9 & 0x10)
            cyc_at(0x00d84cu, 0);
        cyc_run(0x00d850u, 0x00d856u);          /* save A1, jsr d43a, restore */

        parse_bit9_header(a1, &src);
        if (src >= 0x200u && src + 2u < ROM_SIZE)
        {
            uint8_t save_ov = g_overlay;

            g_overlay = WF_THINKER_TAG_OVERLAY;
            d43a(&src);
            g_overlay = save_ov;
        }
        g_ex = 0;
        g_ey = 0;
    }
    if (*a1 & 1u)
        (*a1)++;
    d[6] = TAB_6B;
    pack = r16(*a1);
    *a1 += 2;

    count = (int)(header & 0xffu);
    d[5] = set_w(d[5], 0);
    d[5] = set_b(d[5], (uint8_t)count);
    d[4] = 1;
    extra_x = 0;
    extra_y = 0;

    g_fetch_tab = FETCH_TAB_D7B2;
    p0 = fetch_ptr(a1, pack & 3, TAB_6B, (int16_t)count, 0);
    p1 = fetch_ptr(a1, (pack >> 2) & 3, TAB_6B, (int16_t)count, 0);
    p2 = fetch_ptr(a1, (pack >> 4) & 3, TAB_6B, (int16_t)count, 0);
    p3 = fetch_ptr(a1, (pack >> 6) & 3, TAB_6B, (int16_t)count, 0);
    p4 = fetch_ptr(a1, (pack >> 8) & 3, TAB_6B, (int16_t)count, 0);
    p5 = fetch_ptr(a1, (pack >> 10) & 3, TAB_6B, (int16_t)count, 0);
    d[3] = set_w(d[3], (uint16_t)(((pack >> 10) & 3) << 2));
    g_fetch_tab = FETCH_TAB_D3EA;

    /* Last fetcher result is the A1 tile stream; p5 was fetched from
     * current a1 after the others. Order matches D802: A0,A3,A4,A5,A6,A1. */
    /* 00D858..00D8EA: align A1, read the pack word, six jsr-dispatched
     * fetchers (their handlers are charged inside fetch_ptr), then setup. */
    cyc_run(0x00d858u, 0x00d8eau);

    g_emitter = WF_THINKER_TAG_D802;
    g_pal_bias = 0;                                /* D802's body: no bias */
    g_emit_loop = EMIT_LOOP_D802;
    emit_from_ptrs(&p0, &p1, &p2, &p5, &p4, &p3, extra_x, extra_y, count);
    g_emit_loop = EMIT_LOOP_D63C;

    /* Header bit 8 (btst #0 of the high byte at 0xD984): a second
     * overlay compiled by 0xD540 (same shape as D43A). Smash cell 8
     * stores the belly tiles here; skipping it left a torso hole. */
    cyc_run(0x00d984u, 0x00d984u);              /* btst #0 of the header byte */
    cyc_at(0x00d98au, (header & 0x0100u) == 0); /* beq $d9da */
    if (header & 0x0100u) {
        /* The trailer's bit-9 header has the same three optional extra bytes
         * as the pre-overlay one at 00D812, so it cannot be a flat range
         * either: a range sum charges all three moves and the wrong side of
         * each beq. The flags are the low byte of the FIRST header word,
         * read at the cursor 00D996 has aligned. */
        uint32_t tcur = *a1;
        uint8_t tflags;

        tcur += (tcur & 1u);
        tflags = (uint8_t)r16(tcur);
        cyc_run(0x00d98eu, 0x00d9b8u);
        cyc_at(0x00d9bcu, !(tflags & 0x40));
        if (tflags & 0x40)
            cyc_at(0x00d9beu, 0);
        cyc_at(0x00d9c2u, 0);
        cyc_at(0x00d9c6u, !(tflags & 0x20));
        if (tflags & 0x20)
            cyc_at(0x00d9c8u, 0);
        cyc_at(0x00d9ccu, 0);
        cyc_at(0x00d9d0u, !(tflags & 0x10));
        if (tflags & 0x10)
            cyc_at(0x00d9d2u, 0);
        cyc_at(0x00d9d6u, 0);                   /* jsr $d540 */
    }
    cyc_run(0x00d9dau, 0x00d9e2u);              /* lea, subi.b, rts */

    /* 00D8E4 move.l A1,(A7) stores the cursor AFTER the six fetchers, and the
     * trailer at 00D98E reloads that, so it continues from the current
     * position -- aligned to even first (00D990). */
    if (header & 0x0100u) {
        uint32_t cur = *a1;

        src = 0;
        parse_trailer_header(&cur, &src);
        if (src >= 0x200u && src + 2u < ROM_SIZE)
        {
            uint8_t save_ov = g_overlay;

            g_overlay = (uint8_t)(WF_THINKER_TAG_OVERLAY | WF_THINKER_TAG_TRAILER);
            d540(&src);
            g_overlay = save_ov;
        }
    }

    d[7] -= 0x0au;
    a[7] += 6;
}

static void dispatch(uint16_t header, uint32_t *a1)
{
    g_ev[WF_EV_DISPATCH]++;
    if (g_nest > 6) {
        g_trunc = 1;
        return;
    }
    if (!work_ok())
        return;
    g_nest++;
    switch ((header >> 14) & 3) {
    case 0:
        d2ae(header, a1);
        break;
    case 1:
        d802(header, a1);
        break;
    case 2:
        da12(header, a1);
        break;
    default:
        dc3c(header, a1);
        break;
    }
    g_nest--;
}

static void d2ae_or_d802(uint16_t header, uint32_t *a1)
{
    dispatch(header, a1);
}

static void dc3c(uint16_t header, uint32_t *a1)
{
    int n = (int)(header & 0xffu) - 1;
    uint32_t base;
    int i;
    int last_802 = 0;      /* which arm the final iteration took: the two
                            * exits (00DC7C and 00DC96) are separate code */

    d[7] += 0x0au;
    a[7] -= 6;
    {
        int save_b = g_cyc_bucket;

        g_cyc_bucket = CYCB_OTHER;
        cyc_run(0x00dc3cu, 0x00dc4au);          /* frame, count, save cursor */
        g_cyc_bucket = save_b;
    }
    base = g_stream_base;
    for (i = 0; i <= n; i++) {
        uint32_t off = r16(*a1);
        uint32_t p;
        uint16_t sub;

        /* Unwind rather than keep walking: a spent budget means dispatch is
         * already a no-op, so the remaining iterations would only burn time. */
        if (!work_ok())
            break;
        g_ev[WF_EV_SLICE]++;
        *a1 += 2;
        p = base + off;
        sub = r16(p);
        p += 2;
        {
            int save_b = g_cyc_bucket;
            int is802 = ((sub >> 14) & 3u) == 1;

            g_cyc_bucket = CYCB_OTHER;
            /* 00DC4E..00DC6A: re-read the cursor, index the slice, test the
             * sub-header, then jsr D2AE (00DC6C) or D802 (00DC86). Each arm
             * restores the cursor and dbra's back to 00DC4E. */
            cyc_run(0x00dc4eu, 0x00dc66u);
            cyc_at(0x00dc6au, is802);           /* bne $dc86 */
            if (is802)
                cyc_run(0x00dc86u, 0x00dc8eu);
            else
                cyc_run(0x00dc6cu, 0x00dc74u);
            cyc_at(is802 ? 0x00dc92u : 0x00dc78u, i < n);
            last_802 = is802;
            g_cyc_bucket = save_b;
        }
        d2ae_or_d802(sub, &p);
    }
    {
        int save_b = g_cyc_bucket;

        g_cyc_bucket = CYCB_OTHER;
        if (last_802)
            cyc_run(0x00dc96u, 0x00dc9eu);
        else
            cyc_run(0x00dc7cu, 0x00dc84u);      /* lea, subi.w, rts */
        g_cyc_bucket = save_b;
    }
    a[7] += 6;
    d[7] -= 0x0au;
}

/* 0xDA12 -- the interaction compiler. It drives TWO streams at once: this
 * body's slice (word 1, relative to our own stream base) and the partner's
 * slice (word 0, an index into the PARTNER's frame table). The two are
 * interleaved a slice at a time under a bitmask in the header's low byte,
 * which is why a grapple lands both wrestlers inside one A2 interval.
 *
 * The ROM finds the partner through the linked object at (A0)+0x26; we are
 * handed its row directly because the decoder has no live object graph.
 * With g_partner_row unset only this body is compiled, which is the old
 * behaviour. */
static void da12(uint16_t header, uint32_t *a1)
{
    uint32_t own_base = g_stream_base;
    uint32_t ptr_a, ptr_b = 0;
    uint32_t tab, partner_base = 0;
    uint16_t other, own_off, pose_off, ctrl = header;
    int cnt_a = 0, cnt_b = 0;
    int multi_a = 0, multi_b = 0, done_a = 0, done_b = 1;
    int8_t off_x = 0, off_y = 0;
    int guard = 0;

    /* 00DA12 lea (-$12,A7),A7 / 00DA16 addi.b #$16,D7. The tracked slices
     * live in C locals rather than that frame, but the frame itself still
     * has to move: everything DA12 dispatches into resolves ($4,A7,D7) and
     * ($7,A7,D7) against it, and 0xD540 reads both. */
    a[7] -= 0x12u;
    d[7] = (d[7] + 0x16u) & 0xffffffffu;
    g_da12_seen = 1;
    g_da12_row = (int)(int16_t)g_partner_row;
    g_da12_ox = 0;
    g_da12_oy = 0;

    g_cyc_bucket = CYCB_DA12;
    cyc_run(0x00da12u, 0x00da34u);

    /* 00DA1E / 00DA22 */
    other = r16(*a1);
    *a1 += 2;
    own_off = r16(*a1);
    *a1 += 2;
    ptr_a = own_base + own_off;

    if (g_partner_row >= 12u) {
        /* 00DA38 `bcc $da38` branches to itself, so the ROM never leaves a
         * row >= 12 and there is no cost to model. The C skips the partner
         * instead; charge nothing and decline. */
        g_cyc_partial = 1;
    } else {
        cyc_at(0x00da38u, 0);
        cyc_run(0x00da3au, 0x00da50u);
        /* 00DA3A..00DA70: index the partner's own frame table and stream. */
        tab = 0x00038f14u + (uint32_t)(int32_t)(int16_t)
              r16(0x00038f14u + (uint32_t)g_partner_row * 2u);
        pose_off = r16(tab + (uint32_t)(uint16_t)(other << 1));
        cyc_at(0x00da54u, pose_off != 0xfffeu);
        if (pose_off == 0xfffeu) {
            /* 00DA56..00DA5E is an early rts that emits nothing at all --
             * and note it undoes D7 by $10 where every other exit undoes
             * $16. The C falls through and still runs track A, so the two
             * disagree here; it has never been observed, but until that is
             * settled a call that reaches it cannot be charged. */
            g_cyc_partial = 1;
        }
        if (pose_off != 0xfffeu) {          /* 00DA50 cmpi.w #-2 */
            cyc_run(0x00da60u, 0x00da90u);
            partner_base = r32(0x00038fb8u + (uint32_t)g_partner_row * 4u);
            ptr_b = partner_base + pose_off;
            /* 00DA7C: x/y bias pair, indexed by partner row * 2. */
            off_x = (int8_t)r8(*a1 + (uint32_t)g_partner_row * 2u);
            off_y = (int8_t)r8(*a1 + (uint32_t)g_partner_row * 2u + 1u);
            done_b = 0;
            g_da12_ox = off_x;
            g_da12_oy = off_y;
        }
    }

    /* 00DA92 / 00DAA4: a negative leading word is a slice count prefix. */
    if (!done_b) {
        uint16_t w = r16(ptr_b);

        if ((int16_t)w < 0) {
            ptr_b += 2;
            cnt_b = (int)(uint8_t)w - 1;
            multi_b = 1;
        }
    }
    {
        uint16_t w = r16(ptr_a);

        if ((int16_t)w < 0) {
            ptr_a += 2;
            cnt_a = (int)(uint8_t)w - 1;
            multi_a = 1;
        }
    }
    g_da12_multi = (multi_a ? 1 : 0) | (multi_b ? 2 : 0);

    /* 00DA92..00DAC6: both count prefixes, then the five stores that seed
     * the frame the whole loop indexes. Only reachable with a partner --
     * without one the ROM has already returned. */
    if (!done_b) {
        cyc_run(0x00da92u, 0x00da94u);
        cyc_at(0x00da96u, !multi_b);
        if (multi_b)
            cyc_run(0x00da98u, 0x00daa0u);
        cyc_run(0x00daa4u, 0x00daa6u);
        cyc_at(0x00daa8u, !multi_a);
        if (multi_a)
            cyc_run(0x00daaau, 0x00dab2u);
        cyc_run(0x00dab6u, 0x00dac6u);
    }

    /* 00DACA..00DC3A. The ROM is a jump table between two tracks, and only
     * 00DACA consumes an interleave bit: when a track finishes it jumps
     * straight into the other one (jmp $db82 from A, jmp $dad8 from B)
     * without taking a bit, and a track with slices left loops back to
     * 00DACA and does take one. Treating it as "one bit per dispatch" put
     * the tracks out of step, which is what corrupts a grapple. */
    for (;;) {
        int is802;

        if (guard++ > 128 || !work_ok()) {
            g_cyc_partial = 1;                /* truncated: no honest cost */
            break;
        }
        /* L_DACA */
        {
            int want_b = (ctrl & 1u) != 0;

            g_cyc_bucket = CYCB_DA12;
            cyc_run(0x00dacau, 0x00dad0u);
            cyc_at(0x00dad4u, want_b);
            ctrl = (uint16_t)(ctrl >> 1);
            if (want_b)
                goto L_DB82;
        }
    L_DAD8:
        g_cyc_bucket = CYCB_DA12;
        cyc_at(0x00dad8u, 0);
        cyc_at(0x00dadeu, done_a);
        if (done_a)
            goto L_DB70;
    L_DAE2:
        g_cyc_bucket = CYCB_DA12;
        cyc_at(0x00dae2u, 0);
        cyc_at(0x00dae8u, multi_a);
        g_off_x = 0;                          /* 00DAF6 clr.w ($12,A7,D7) */
        g_off_y = 0;
        if (multi_a) {                        /* 00DB1E */
            uint16_t off = r16(ptr_a);
            uint32_t p;
            uint16_t sub;
            int more;

            ptr_a += 2;
            p = own_base + off;
            sub = r16(p);
            p += 2;
            /* The ROM only tests bit 14 here, so a bit-15 header would go to
             * 0xD2AE/0xD802 on hardware and to 0xDA12/0xDC3C in dispatch().
             * Never seen; decline rather than charge the wrong routine. */
            if (sub & 0x8000u)
                g_cyc_partial = 1;
            is802 = (sub & 0x4000u) != 0;
            cyc_run(0x00db1eu, 0x00db34u);
            cyc_at(0x00db38u, is802);
            cyc_run(is802 ? 0x00db54u : 0x00db3au,
                    is802 ? 0x00db58u : 0x00db3eu);   /* clr.w + jsr */
            dispatch(sub, &p);
            g_cyc_bucket = CYCB_DA12;
            cyc_at(is802 ? 0x00db5cu : 0x00db42u, 0); /* subq.b */
            more = --cnt_a >= 0;
            cyc_at(is802 ? 0x00db60u : 0x00db46u, more);
            if (more)
                continue;                     /* 00DB46 bpl $daca */
            cyc_run(is802 ? 0x00db64u : 0x00db48u,
                    is802 ? 0x00db6au : 0x00db4eu);   /* bset + jmp */
            done_a = 1;
            goto L_DB82;
        } else {                              /* 00DAEA */
            uint32_t p = ptr_a;
            uint16_t sub = r16(p);

            p += 2;
            if (sub & 0x8000u)
                g_cyc_partial = 1;
            is802 = (sub & 0x4000u) != 0;
            cyc_run(0x00daeau, 0x00daf0u);
            cyc_at(0x00daf4u, is802);
            cyc_run(is802 ? 0x00db0au : 0x00daf6u,
                    is802 ? 0x00db0eu : 0x00dafau);   /* clr.w + jsr */
            dispatch(sub, &p);
            g_cyc_bucket = CYCB_DA12;
            cyc_run(is802 ? 0x00db12u : 0x00dafeu,
                    is802 ? 0x00db18u : 0x00db04u);   /* bset + jmp */
            done_a = 1;
            goto L_DB82;                      /* 00DB04 jmp $db82 */
        }
    L_DB70:
        g_cyc_bucket = CYCB_DA12;
        cyc_at(0x00db70u, 0);
        cyc_at(0x00db76u, !done_b);
        if (done_b) {
            cyc_run(0x00db78u, 0x00db80u);    /* 00DB76/00DB80 rts */
            break;
        }
        goto L_DB8C;
    L_DB82:
        g_cyc_bucket = CYCB_DA12;
        cyc_at(0x00db82u, 0);
        cyc_at(0x00db88u, done_b);
        if (done_b)
            goto L_DC28;
    L_DB8C:
        g_cyc_bucket = CYCB_DA12;
        cyc_at(0x00db8cu, 0);
        cyc_at(0x00db92u, multi_b);
        g_off_x = off_x;                      /* 00DBA0 / 00DBEC */
        g_off_y = off_y;
        if (multi_b) {                        /* 00DBD0 */
            uint16_t off = r16(ptr_b);
            uint32_t p;
            uint16_t sub;
            int more;

            ptr_b += 2;
            p = partner_base + off;
            sub = r16(p);
            p += 2;
            if (sub & 0x8000u)
                g_cyc_partial = 1;
            is802 = (sub & 0x4000u) != 0;
            cyc_run(0x00dbd0u, 0x00dbe6u);
            cyc_at(0x00dbeau, is802);
            cyc_run(is802 ? 0x00dc0au : 0x00dbecu,
                    is802 ? 0x00dc12u : 0x00dbf4u);   /* two move.w + jsr */
            dispatch(sub, &p);
            g_cyc_bucket = CYCB_DA12;
            g_off_x = 0;
            g_off_y = 0;
            cyc_at(is802 ? 0x00dc16u : 0x00dbf8u, 0); /* subq.b */
            more = --cnt_b >= 0;
            cyc_at(is802 ? 0x00dc1au : 0x00dbfcu, more);
            if (more)
                continue;                     /* 00DBFC bpl $daca */
            cyc_run(is802 ? 0x00dc1eu : 0x00dc00u,
                    is802 ? 0x00dc24u : 0x00dc06u);   /* bset + jmp $dad8 */
            done_b = 1;
            goto L_DAD8;                      /* 00DC06 jmp $dad8 */
        } else {                              /* 00DB94 */
            uint32_t p = ptr_b;
            uint16_t sub = r16(p);

            p += 2;
            if (sub & 0x8000u)
                g_cyc_partial = 1;
            is802 = (sub & 0x4000u) != 0;
            cyc_run(0x00db94u, 0x00db9au);
            cyc_at(0x00db9eu, is802);
            cyc_run(is802 ? 0x00dbb8u : 0x00dba0u,
                    is802 ? 0x00dbc0u : 0x00dba8u);   /* two move.w + jsr */
            dispatch(sub, &p);
            g_cyc_bucket = CYCB_DA12;
            g_off_x = 0;
            g_off_y = 0;
            cyc_run(is802 ? 0x00dbc4u : 0x00dbacu,
                    is802 ? 0x00dbcau : 0x00dbb2u);   /* bset + jmp $dad8 */
            done_b = 1;
            goto L_DAD8;                      /* 00DBB2 jmp $dad8 */
        }
    L_DC28:
        g_cyc_bucket = CYCB_DA12;
        cyc_at(0x00dc28u, 0);
        cyc_at(0x00dc2eu, !done_a);
        if (done_a) {
            cyc_run(0x00dc32u, 0x00dc3au);    /* 00DC32 rts */
            break;
        }
        goto L_DAE2;                          /* 00DC2E beq $dae2 */
    }
    g_off_x = 0;
    g_off_y = 0;
    a[7] += 0x12u;                                 /* 00DB78 / 00DC32 */
    d[7] = (d[7] - 0x16u) & 0xffffffffu;
}

void wf_thinker_set_partner_row(int row)
{
    g_partner_row = (row >= 0 && row < 12) ? (uint16_t)row : 0xffffu;
}

int wf_thinker_decode_obj(uint32_t stream_base, uint32_t stream_off, int flip,
                          int16_t ox, int16_t oy, uint16_t row, uint16_t pose,
                          WfThinkerSpr *out, int cap)
{
    uint32_t a1;
    uint16_t header;
    uint16_t kind;

    if (!g_ready || !out || cap <= 0) {
        set_err("decode: not ready");
        return -1;
    }
    if (stream_base + stream_off + 2 >= ROM_SIZE) {
        set_err("decode: off past ROM");
        return -1;
    }

    if (g_entry_d_valid)
        memcpy(d, g_entry_d, sizeof d);
    else
        memset(d, 0, sizeof d);
    g_entry_d_valid = 0;
    memset(a, 0, sizeof a);
    g_out = out;
    g_cap = cap;
    g_n = 0;

    /* The ROM folds the object's screen words into the running origin as
     * +0x10 / +0x90. The offline dummy object (0x80/0xa0) is just the
     * ox/oy = 0x80/0xa0 case of this. */
    g_ox = (int16_t)(ox + 0x10);
    g_oy = (int16_t)(oy + 0x90);
    g_flip = flip ? 0x10 : 0;
    g_ex = 0;
    g_ey = 0;
    g_stream_base = stream_base;
    g_nest = 0;
    g_ops = 0;
    g_work = 0;
    g_trunc = 0;
    g_emitter = 0;
    g_overlay = 0;
    g_off_x = 0;
    g_off_y = 0;
    g_pal_bias = 0;
    g_cycles = 0;
    g_cyc_partial = 0;
    memset(g_cyc_by, 0, sizeof g_cyc_by);
    g_cyc_bucket = CYCB_D1FC;
    g_saw_d540 = 0;
    memset(g_ev, 0, sizeof g_ev);
    g_da12_seen = 0;
    g_da12_multi = 0;
    memset(g_tag, 0xff, sizeof g_tag);

    w16(DUMMY_OBJ + 2, row);
    w16(DUMMY_OBJ + 4, pose);
    w16(DUMMY_OBJ + 0x14, (uint16_t)ox);
    w16(DUMMY_OBJ + 0x16, (uint16_t)oy);
    w32(DUMMY_OBJ + 0x26, DUMMY_OBJ);

    a[0] = DUMMY_OBJ;
    a[2] = SPR_BASE;
    a[7] = STACK_TOP;
    /* DCA0 runs before the saved-register compiler body.  It leaves the
     * selected pose-stream word in D2, then D1FC clears D7. */
    d[2] = set_w(d[2], (uint16_t)stream_off);
    d[7] = 0;
    a[7] -= 0x28;
    w16(a[7] + 0x00, (uint16_t)(ox + 0x10));
    w16(a[7] + 0x02, (uint16_t)(oy + 0x90));
    w16(a[7] + 0x12, 0);
    w8(a[7] + 0x08, flip ? 0x10 : 0);
    w32(a[7] + 0x0a, stream_base);
    w32(a[7] + 0x14, DUMMY_OBJ);

    /* 00D1FC..00D298 entry and 00D29C..00D2A4 exit wrap every compile. The
     * 00D248 bpl is not taken when the pose word is negative, which then also
     * runs the 00D24A bset. */
    /* 00D204 and 00D210 are the prologue's two early rts and are branched
     * over on the normal path, so they must not be swept up by a range sum;
     * wf_rom_cycle_run sums every table entry between its bounds. 00D2A4's
     * rts is outside the window too -- ported.c closes the measurement at
     * pc == 0x00D2A4, before that instruction runs.
     *
     * Charging 00D210 and 00D2A4 used to make the total come out right, but
     * only because the generator dropped movem's per-register term and the
     * two movem.l either side of the body (00D212, 00D2A0) were each 16
     * short. Two wrongs cancelling; both are fixed. */
    cyc_run(0x00d1fcu, 0x00d202u);
    cyc_run(0x00d206u, 0x00d20eu);
    cyc_run(0x00d212u, 0x00d246u);
    cyc_at(0x00d248u, !flip);
    if (flip)
        cyc_at(0x00d24au, 0);
    cyc_run(0x00d250u, 0x00d298u);
    cyc_run(0x00d29cu, 0x00d2a0u);

    a1 = stream_base + stream_off;
    header = r16(a1);
    a1 += 2;
    kind = header;
    /* rol.w #3 ; andi.w #6 → bits 15..14 */
    kind = (uint16_t)(((kind << 3) | (kind >> 13)) & 6u);
    switch (kind) {
    case 0:
        d2ae(header, &a1);
        break;
    case 2:
        d802(header, &a1);
        break;
    case 4:
        da12(header, &a1);
        break;
    default:
        dc3c(header, &a1);
        break;
    }
    return g_n;
}

/* Engine init: no ROM copy — reads go to the data layer. */
int wf_thinker_init_live(void)
{
    uint32_t i;
    memset(mem, 0, sizeof mem);
    memset(g_err, 0, sizeof g_err);
    g_ready = 0;
    g_live_tbl = 1;
    for (i = 0; i < 16; i++)
        w8(BANK_TAB + i, (uint8_t)i);
    g_ready = 1;
    return 0;
}
