/* Emulated WrestleFest sound board: Z80 + YM2151 + OKI M6295 running the
 * game's own sound program, so every tune and sample is the real thing at
 * full length. Wiring transcribed from the MAME ddragon3 driver (the
 * oracle):
 *
 *   Z80 3.579545 MHz     0x0000-0xBFFF ROM (31a11-2.ic42, first 48K)
 *                        0xC000-0xC7FF RAM
 *                        0xC800-0xC801 YM2151 (3.579545 MHz -> /64 = 55930.4 Hz)
 *                        0xD800        OKI M6295 (1.056 MHz, pin7 H -> 8000 Hz)
 *                        0xE000        sound latch (68k 0x14000C; write = NMI)
 *                        0xE800        OKI bank (bit0 x 256 KB of 31j10.ic73)
 *   YM2151 IRQ -> Z80 INT.  Mono mix: YM 0.33 + 0.33, OKI 0.66.
 *
 * Cores: src/vendor/z80.c (superzazu, MIT) and src/vendor/opm.c
 * (Nuked-OPM, LGPL); the OKI decoder is ours (sndboard.c). */
#ifndef WF_SNDBOARD_H
#define WF_SNDBOARD_H
#include <stdint.h>
#include <stddef.h>

/* Boot the board from the two sound ROM images (z80: 64K program,
 * oki: 512K ADPCM). Copies the buffers. 0 = ok. */
int  snd_board_init(const uint8_t *z80rom, size_t zlen,
                    const uint8_t *okirom, size_t olen);

/* 68k sound-latch write (the command byte): latch + NMI.
 * Caller serializes against snd_board_render (SDL_LockAudioDevice). */
void snd_board_latch(uint8_t v);

/* Render `frames` frames of 48 kHz interleaved stereo S16 into out,
 * OVERWRITING it (the board is the mix bus; callers mix WAVs on top). */
void snd_board_render(int16_t *out, int frames);

int  snd_board_ready(void);

/* boot from data/sound/z80.bin + oki.bin (mod-resolved); 0 = ok */
int  snd_board_init_files(void);
#endif
