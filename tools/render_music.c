/* --render-music: dump a sound-latch command through the emulated board
 * (sndboard.c — the ROM's own Z80 driver) to a 48 kHz stereo WAV, so a
 * modder can start a music edit from the real full-length tune.
 *
 *   wfengine --render-music 0x03 60 out.wav     one command, 60 s
 *   wfengine --render-music all 90 somedir/     cmd_01..12.wav for each tune
 *
 * Boots from data/sound/*.bin (mod-resolved: an active --profile renders
 * ITS sound program). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../src/sndboard.h"

static void wr32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc((v >> 24) & 255, f); }
static void wr16(FILE *f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }

/* "0x04,0x05": every command posted in the same instant, as the game does
 * when a frame fires several (jingle + announcer, impact + grunt) */
static int render_one(const char *cmdarg, double secs, const char *out)
{
    uint8_t cmd = (uint8_t)strtol(cmdarg, NULL, 0);
    enum { RATE = 48000, CH = 2 };
    uint32_t frames = (uint32_t)(secs * RATE);
    uint32_t dbytes = frames * CH * 2u;
    int16_t buf[4096 * CH];
    FILE *f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "render-music: cannot write %s\n", out); return 1; }
    fwrite("RIFF", 1, 4, f); wr32(f, 36 + dbytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wr32(f, 16); wr16(f, 1); wr16(f, CH);
    wr32(f, RATE); wr32(f, RATE * CH * 2); wr16(f, CH * 2); wr16(f, 16);
    fwrite("data", 1, 4, f); wr32(f, dbytes);
    snd_board_latch(0x00);                       /* stop everything first */
    { int16_t warm[512 * CH]; snd_board_render(warm, 512); }
    for (const char *p = cmdarg; p; ) {
        snd_board_latch((uint8_t)strtol(p, NULL, 0));
        p = strchr(p, ',');
        if (p) p++;
    }
    for (uint32_t done = 0; done < frames; ) {
        int n = (int)(frames - done < 4096 ? frames - done : 4096);
        snd_board_render(buf, n);
        fwrite(buf, 2, (size_t)n * CH, f);
        done += (uint32_t)n;
    }
    fclose(f);
    printf("render-music: cmd 0x%02X -> %s (%.1f s)\n", cmd, out, secs);
    return 0;
}

int tool_render_music(const char *cmdarg, double secs, const char *out)
{
    if (secs <= 0 || secs > 600) secs = 60;
    if (snd_board_init_files() != 0) {
        fprintf(stderr, "render-music: cannot boot the sound board — run "
                        "`./wfengine --export-sound` once to create data/sound/*.bin\n");
        return 1;
    }
    if (!strcmp(cmdarg, "all")) {
        char path[512];
        for (int c = 0x01; c <= 0x12; c++) {
            snprintf(path, sizeof path, "%s/cmd_%02X.wav", out, c);
            { char cs[8]; snprintf(cs, sizeof cs, "%d", c); if (render_one(cs, secs, path)) return 1; }
        }
        return 0;
    }
    return render_one(cmdarg, secs, out);
}
