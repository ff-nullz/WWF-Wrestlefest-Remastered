/* Emulated sound board — see sndboard.h for the wiring (from the MAME
 * ddragon3 driver). The Z80 runs the shipped sound program verbatim; the
 * YM2151 is Nuked-OPM clocked cycle-locked with the CPU; the OKI M6295 is
 * implemented here from the chip's documented behaviour (Dialogic ADPCM,
 * 128-entry phrase table at the base of its (banked) ROM window, 8 kHz at
 * 1.056 MHz / pin7 high, whole-window 256 KB banking via 0xE800 —
 * set_rom_bank(data & 1) in the oracle). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sndboard.h"
#include "vendor/z80.h"
#include "vendor/opm.h"

/* ---- OKI M6295 ---- */
#define OKI_BANK 0x40000u

static const uint8_t oki_volume[16] = {   /* x/32 steps, 9.. = mute */
    0x20, 0x16, 0x10, 0x0b, 0x08, 0x06, 0x04, 0x03,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const int16_t adpcm_step[49] = {
    16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253,
    279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552
};
static const int8_t adpcm_adj[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

typedef struct {
    int playing;
    uint32_t addr;       /* nibble address inside the 256K window */
    uint32_t stop;       /* first nibble past the sample */
    int16_t signal;      /* 12-bit ADPCM accumulator */
    int8_t step;         /* 0..48 */
    uint8_t vol;         /* 0..32 */
} OkiVoice;

typedef struct {
    const uint8_t *rom;
    size_t len;
    uint32_t bank;       /* byte offset of the 256K window */
    OkiVoice v[4];
    int pending;         /* phrase armed by a 0x80|n write, -1 = none */
} Oki;

static Oki oki;

static uint8_t oki_rom(uint32_t byteaddr)
{
    uint32_t a = oki.bank + (byteaddr & (OKI_BANK - 1));
    return (a < oki.len) ? oki.rom[a] : 0;
}

static uint8_t oki_read(void)
{
    uint8_t r = 0xF0;                      /* unused status bits read 1 */
    for (int i = 0; i < 4; i++)
        if (oki.v[i].playing) r |= (uint8_t)(1u << i);
    return r;
}

static void oki_write(uint8_t d)
{
    if (oki.pending >= 0) {                /* 2nd byte: channels | volume */
        for (int i = 0; i < 4; i++)
            if (d & (0x10u << i)) {
                OkiVoice *v = &oki.v[i];
                uint32_t t = (uint32_t)oki.pending * 8u;
                uint32_t start = ((uint32_t)oki_rom(t) << 16)
                               | ((uint32_t)oki_rom(t + 1) << 8) | oki_rom(t + 2);
                uint32_t stop  = ((uint32_t)oki_rom(t + 3) << 16)
                               | ((uint32_t)oki_rom(t + 4) << 8) | oki_rom(t + 5);
                start &= 0x3FFFF; stop &= 0x3FFFF;
                if (stop > start) {
                    v->playing = 1;
                    v->addr = start * 2u;          /* nibbles, high first */
                    v->stop = (stop + 1u) * 2u;
                    v->signal = 0; v->step = 0;
                    v->vol = oki_volume[d & 0x0F];
                } else v->playing = 0;
            }
        oki.pending = -1;
        return;
    }
    if (d & 0x80) { oki.pending = d & 0x7F; return; }
    for (int i = 0; i < 4; i++)            /* stop: bits 6..3 = channels */
        if (d & (0x08u << i)) oki.v[i].playing = 0;
}

/* one 8 kHz tick: decode a nibble on every live voice, return the mix */
static int32_t oki_tick(void)
{
    int32_t out = 0;
    for (int i = 0; i < 4; i++) {
        OkiVoice *v = &oki.v[i];
        uint8_t byte, nib;
        int32_t s, diff;
        if (!v->playing) continue;
        if (v->addr >= v->stop) { v->playing = 0; continue; }
        byte = oki_rom(v->addr >> 1);
        nib = (v->addr & 1u) ? (byte & 0x0F) : (byte >> 4);
        v->addr++;
        s = adpcm_step[v->step];
        diff = s >> 3;
        if (nib & 1) diff += s >> 2;
        if (nib & 2) diff += s >> 1;
        if (nib & 4) diff += s;
        if (nib & 8) diff = -diff;
        v->signal = (int16_t)(v->signal + diff);
        if (v->signal > 2047) v->signal = 2047;
        else if (v->signal < -2048) v->signal = -2048;
        v->step = (int8_t)(v->step + adpcm_adj[nib & 7]);
        if (v->step < 0) v->step = 0;
        else if (v->step > 48) v->step = 48;
        out += (int32_t)v->signal * v->vol;   /* +-2047 * 32 = 17 bit */
    }
    return out;
}

/* ---- board ---- */
static z80 cpu;
static opm_t fm;
static uint8_t zrom[0x10000];
static uint8_t zram[0x800];
static uint8_t oki_store[0x80000];
static uint8_t latch;
static int ready;
static uint8_t ym_irq_prev;

/* Command FIFO. The 68k side ($2052) writes the latch and spins ~60 us, so
 * back-to-back posts (jingle + announcer, impact + grunt) each get their
 * NMI on real hardware. Here the Z80 only runs inside the audio callback,
 * so a second post in the same game frame would overwrite the first before
 * the Z80 had fetched it: queue them and hand over the next one once the
 * handler has read the previous (plus a short hold-off, as the ROM does). */
static uint8_t cmd_q[64];
static unsigned cmd_head, cmd_tail;
static int latch_pending;                    /* NMI raised, not yet read */
static int latch_holdoff;                    /* board samples until next pop */

static uint8_t z_rd(void *ud, uint16_t a)
{
    (void)ud;
    if (a < 0xC000) return zrom[a];
    if (a < 0xC800) return zram[a & 0x7FF];
    if (a == 0xC800 || a == 0xC801) return OPM_Read(&fm, a & 1);
    if (a == 0xD800) return oki_read();
    if (a == 0xE000) { latch_pending = 0; return latch; }
    return 0xFF;
}

static void z_wr(void *ud, uint16_t a, uint8_t d)
{
    (void)ud;
    if (a >= 0xC000 && a < 0xC800) { zram[a & 0x7FF] = d; return; }
    if (a == 0xC800 || a == 0xC801) { OPM_Write(&fm, a & 1, d); return; }
    if (a == 0xD800) { oki_write(d); return; }
    if (a == 0xE800) { oki.bank = (d & 1u) ? OKI_BANK : 0; return; }
}

static uint8_t z_in(z80 *z, uint8_t port) { (void)z; (void)port; return 0xFF; }
static void z_out(z80 *z, uint8_t port, uint8_t d) { (void)z; (void)port; (void)d; }

/* one YM sample = 64 shared 3.579545 MHz clocks; OKI ticks at 8000 Hz on
 * its own 1.056 MHz crystal -> 8000/55930.39 per YM sample */
#define YM_RATE   (3579545.0 / 64.0)
#define OKI_STEP  (8000.0 / (3579545.0 / 64.0))

static double oki_frac;
static int32_t oki_hold;                     /* zero-order hold between ticks */

/* one board sample (at YM_RATE), mono, ~16-bit range.
 * The Z80 and the OPM share the 3.579545 MHz clock and MUST advance in
 * lockstep per instruction: Nuked-OPM consumes a write per internal
 * cycle, so batching 64 CPU cycles before clocking the chip drops
 * back-to-back register writes (observed: timers never started). */
static int32_t board_sample(void)
{
    static unsigned long opm_cyc;               /* OPM clocks issued */
    unsigned long target = opm_cyc + 64;
    int32_t ymout[2] = { 0, 0 };
    uint8_t irq;
    int32_t mix;

    if (latch_holdoff > 0) latch_holdoff--;
    else if (!latch_pending && cmd_head != cmd_tail) {
        latch = cmd_q[cmd_head++ & 63u];
        latch_pending = 1;
        latch_holdoff = 4;                   /* ~4.5 kcycles, > the ROM's spin */
        z80_gen_nmi(&cpu);
    }

    while (opm_cyc < target) {
        unsigned long upto;
        if (cpu.cyc <= opm_cyc) z80_step(&cpu);
        upto = cpu.cyc < target ? cpu.cyc : target;
        /* one OPM_Clock = one INTERNAL cycle = 2 master clocks (the core's
         * slot loop is `cycles & 31`, 32 per sample) — clocking it per
         * master cycle ran the chip double-speed (tempo x2). */
        while (opm_cyc < upto) { OPM_Clock(&fm, ymout, NULL, NULL, NULL); opm_cyc += 2; }
    }
    irq = OPM_ReadIRQ(&fm);
    if (irq && !ym_irq_prev) z80_gen_int(&cpu, 0xFF);
    ym_irq_prev = irq;

    oki_frac += OKI_STEP;
    if (oki_frac >= 1.0) { oki_frac -= 1.0; oki_hold = oki_tick(); }

    /* YM DAC ~ +-32768 per channel; OKI 17-bit sum.
     * Oracle routes (mono): YM 0.33 each, OKI 0.66. */
    mix = (int32_t)((ymout[0] + ymout[1]) * 0.33)
        + (int32_t)(oki_hold * 0.66 * 0.5);
    return (int32_t)(mix * 1.35);   /* level-matched to the MAME reference render */
}

int snd_board_init(const uint8_t *z80rom, size_t zlen,
                   const uint8_t *okirom, size_t olen)
{
    if (!z80rom || zlen < 0xC000 || !okirom || olen == 0 || olen > sizeof oki_store)
        return -1;
    memset(zrom, 0xFF, sizeof zrom);
    memcpy(zrom, z80rom, zlen > sizeof zrom ? sizeof zrom : zlen);
    memcpy(oki_store, okirom, olen);
    memset(zram, 0, sizeof zram);
    memset(&oki, 0, sizeof oki);
    oki.rom = oki_store; oki.len = olen; oki.pending = -1;
    z80_init(&cpu);
    cpu.read_byte = z_rd; cpu.write_byte = z_wr;
    cpu.port_in = z_in; cpu.port_out = z_out;
    OPM_Reset(&fm, 0);
    latch = 0; ym_irq_prev = 0; oki_frac = 0; oki_hold = 0;
    cmd_head = cmd_tail = 0; latch_pending = 0; latch_holdoff = 0;
    /* let the program boot (~100 ms) before the first command lands */
    for (int i = 0; i < 5600; i++) (void)board_sample();
    ready = 1;
    return 0;
}

int snd_board_ready(void) { return ready; }

void snd_board_latch(uint8_t v)
{
    if (!ready) return;
    if (cmd_tail - cmd_head >= 64u) cmd_head++;  /* overflow: drop the oldest */
    cmd_q[cmd_tail++ & 63u] = v;
}

/* 55930.39 Hz board -> 48000 Hz out, linear interpolation */
void snd_board_render(int16_t *out, int frames)
{
    static double pos;                      /* 0..1 between prev and cur */
    static int32_t prev, cur;
    const double step = YM_RATE / 48000.0;
    if (!ready) { memset(out, 0, (size_t)frames * 4u); return; }
    for (int f = 0; f < frames; f++) {
        int32_t s;
        pos += step;
        while (pos >= 1.0) { prev = cur; cur = board_sample(); pos -= 1.0; }
        s = prev + (int32_t)((cur - prev) * pos);
        if (s > 32767) s = 32767;
        else if (s < -32768) s = -32768;
        out[f * 2] = (int16_t)s;
        out[f * 2 + 1] = (int16_t)s;
    }
}

/* convenience: boot from data/sound/*.bin (mod-resolvable, ADR-001 —
 * the engine never opens rom/; run `wfengine --export-sound` once) */
#include "profile.h"
static uint8_t *snd_slurp(const char *rel, size_t *n)
{
    char path[512]; FILE *f; long sz; uint8_t *b;
    if (!wf_mod_resolve(rel, path, sizeof path))
        snprintf(path, sizeof path, "data/%s", rel);
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 0x100000) { fclose(f); return NULL; }
    b = malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f); *n = (size_t)sz;
    return b;
}

int snd_board_init_files(void)
{
    size_t zn = 0, on = 0;
    uint8_t *z = snd_slurp("sound/z80.bin", &zn);
    uint8_t *o = snd_slurp("sound/oki.bin", &on);
    int rc = (z && o) ? snd_board_init(z, zn, o, on) : -1;
    free(z); free(o);
    return rc;
}
