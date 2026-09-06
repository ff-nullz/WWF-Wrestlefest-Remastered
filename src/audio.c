/* WrestleFest audio playback.
 *
 * Two-layer model:
 * 1. Events: 68000 writes command to 0x14000C -> lookup sample -> play once
 * 2. Music:  cmd 0x01-0x12 play data/music/cmd_XX.wav (MAME YM2151 render);
 *    cmd 0x00 stops. Attract 0x03 already contains the drum scene.
 *
 * Do not loop bank0_001/002 as "match ambience" — those hits only belong
 * under one attract theme (cmd 0x03), and they are already in that WAV.
 *
 * Audio must NOT touch CPU state or timing — it hangs off the sound latch
 * only. make regress stays silent (no audio_init).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include <sys/stat.h>
#include "audio.h"
#include "sndboard.h"
#include "profile.h"
#include "wf.h"
#include "assetlog.h"

/* Sample cache — loaded once on init */
typedef struct {
    uint8_t *data;
    uint32_t len;
    SDL_AudioSpec spec;
} Sample;

static Sample samples[128][2];  /* [phrase][bank] */
static bool samples_loaded = false;

/* Command -> (bank, phrase) mapping */
typedef struct {
    uint8_t cmd;
    uint8_t bank;
    uint8_t phrase;
    char label[64];
} CmdMapping;

#define MAX_CMD_MAP 160
static CmdMapping cmd_map[MAX_CMD_MAP];
static int cmd_map_count = 0;

/* "cur" in the mapping file: play from whichever OKI bank is currently
 * selected instead of switching. Phrases 1-19 are byte-identical in both
 * banks (the ROM duplicates them for exactly this reason), so resolving a
 * BANK_CURRENT row against bank 0 is exact, not an approximation. */
#define BANK_CURRENT 2

/* The M6295 sees one 256 KB bank at a time. The Z80 switches it by writing
 * 0xE800, and its switch routine (sound ROM 0x026e) stops every playing
 * voice before doing so -- so samples from different banks can never
 * overlap. Modelled here for the same reason. */
static int current_bank = 0;

/* Commands that drive the YM2151 rather than the OKI. Tracked only so the
 * debug log can say "unimplemented music layer" instead of "unknown". */
typedef struct {
    uint8_t cmd;
    char label[64];
} MusicCmd;

static MusicCmd music_map[64];
static int music_map_count = 0;

/* Commands that silence every voice and reset the bank. */
static uint8_t stop_map[8];
static char stop_labels[8][64];
static int stop_map_count = 0;

/* WF_AUDIO_DEBUG=1 narrates every sound latch. */
static int audio_debug = 0;

/* Voice pool for event samples */
#define MAX_VOICES 8
typedef struct {
    const Sample *s;
    uint32_t pos;
    bool active;
} Voice;

static Voice voices[MAX_VOICES];

/* YM music: one looping stream. Skip the MAME-record preamble
 * (record_ym_song.lua posts the command at frame 100). */
#define MUSIC_SONGS      0x13
#define MUSIC_SKIP_FRAME 100
#define MUSIC_SKIP_HZ    57.4448

static Sample music[MUSIC_SONGS];
static uint32_t music_loop[MUSIC_SONGS];
static int music_playing_cmd = 0;
static uint32_t music_pos = 0;

/* Emulated sound board (sndboard.c) — the default when data/sound/*.bin
 * exist (WF_SOUND=wav forces the legacy WAV player). Music commands with a
 * mod-supplied music/cmd_XX.wav override stream the WAV instead (the board
 * gets a stop), so profiles can replace tunes with plain files. */
static int emu_mode = 0;
static Sample music_ovr[MUSIC_SONGS];
static int8_t music_ovr_state[MUSIC_SONGS];   /* 0 unknown, 1 loaded, -1 none */
static int music_is_ovr = 0;

/* Audio device */
static SDL_AudioDeviceID audio_dev = 0;
static SDL_AudioSpec audio_spec;

/* Path to samples directory */
static const char *samples_path = NULL;

/* Percussion enabled (for --sound-percussion) */
int audio_play_percussion = 0;

/* Forward declarations */
static void audio_callback(void *userdata, Uint8 *stream, int len);
static int load_sound_mapping(const char *path);
static int load_sample(const char *base_path, int bank, int phrase);
static int convert_to_dev(Sample *s, int from_sdl_wav);
static float sample_seconds(const Sample *s);
static int load_music(void);
static void play_event_sample(uint8_t cmd);
static void music_stop(void);
static void music_start(uint8_t cmd);
static Sample *music_override(uint8_t cmd);


int audio_init(const char *samples_dir, int enable_percussion)
{
    if (!samples_dir)
        samples_dir = "data/sounds";
    samples_path = samples_dir;
    audio_play_percussion = enable_percussion;

    const char *dbg = getenv("WF_AUDIO_DEBUG");
    audio_debug = (dbg && *dbg && strcmp(dbg, "0") != 0);

    /* Initialize SDL audio subsystem */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "audio: SDL_InitSubSystem failed: %s\n", SDL_GetError());
        return -1;
    }

    /* Device matches the MAME YM renders (48 kHz stereo). OKI 8 kHz
     * mono samples are converted up on load. */
    SDL_AudioSpec want;
    memset(&want, 0, sizeof(want));
    want.freq = 48000;
    want.format = AUDIO_S16;
    want.channels = 2;
    want.samples = 2048;
    want.callback = audio_callback;
    want.userdata = NULL;

    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &audio_spec, 0);
    if (audio_dev == 0) {
        fprintf(stderr, "audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    /* The emulated board is the default sound engine; the WAV player is
     * the fallback (no data/sound/*.bin) and the forced legacy mode
     * (WF_SOUND=wav). Mods can still override tunes with WAVs in emu
     * mode (music_override). */
    {
        const char *m = getenv("WF_SOUND");
        if (!(m && !strcmp(m, "wav")) && snd_board_init_files() == 0) {
            emu_mode = 1;
            fprintf(stderr, "audio: EMULATED sound board (Z80+YM2151+OKI, data/sound/*.bin)\n");
        } else if (!(m && !strcmp(m, "wav")))
            fprintf(stderr, "audio: no data/sound/*.bin (run ./wfengine --export-sound) - legacy WAV mode\n");
    }
    wf_asset_log("sound-map", "data/sound-commands.txt");
    if (load_sound_mapping("data/sound-commands.txt") != 0 && !emu_mode) {
        fprintf(stderr, "audio: failed to load sound mapping from data/sound-commands.txt\n");
        return -1;
    }
    if (load_music() != 0 && !emu_mode)
        return -1;

    samples_loaded = true;
    {
        int n = 0, bank, phrase, m;
        for (bank = 0; bank < 2; bank++)
            for (phrase = 0; phrase < 128; phrase++)
                if (samples[phrase][bank].data)
                    n++;
        m = 0;
        for (phrase = 1; phrase < MUSIC_SONGS; phrase++)
            if (music[phrase].data)
                m++;
        fprintf(stderr, "audio: loaded %d sample WAV(s) from %s, %d music WAV(s)\n",
                n, samples_dir, m);
    }
    SDL_PauseAudioDevice(audio_dev, 0);
    return 0;
}


void audio_shutdown(void)
{
    if (audio_dev) {
        SDL_CloseAudioDevice(audio_dev);
        audio_dev = 0;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);

    for (int bank = 0; bank < 2; bank++) {
        for (int phrase = 0; phrase < 128; phrase++) {
            free(samples[phrase][bank].data);
            samples[phrase][bank].data = NULL;
        }
    }
    for (int i = 0; i < MUSIC_SONGS; i++) {
        free(music[i].data);
        music[i].data = NULL;
    }
}


void audio_on_sound_latch(uint16_t value)
{
    if (!samples_loaded || audio_dev == 0)
        return;

    /* Command byte is low byte of the 16-bit write */
    uint8_t cmd = value & 0xFF;
    if (emu_mode) {
        if (audio_debug)
            fprintf(stderr, "audio: cmd=0x%02X -> sound board\n", cmd);
        SDL_LockAudioDevice(audio_dev);
        if (cmd != 0 && cmd < 0x20 && music_override(cmd)) {
            /* a mod supplies this tune as a WAV: stream it, silence the
             * board's music layer (the game already stopped the old tune) */
            music_playing_cmd = cmd;
            music_pos = 0;
            music_is_ovr = 1;
            snd_board_latch(0x00);
        } else {
            if (cmd < 0x20 && music_is_ovr) music_stop();   /* new tune / stop */
            snd_board_latch(cmd);
        }
        SDL_UnlockAudioDevice(audio_dev);
        return;
    }
    play_event_sample(cmd);
}



static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    if (emu_mode && snd_board_ready())
        snd_board_render((int16_t *)stream, len / 4);   /* 48k stereo S16 */
    else
        memset(stream, 0, len);

    /* Mix active event voices (legacy samples AND library WAVs) */
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active || !voices[i].s || !voices[i].s->data)
            continue;

        uint32_t remaining = voices[i].s->len - voices[i].pos;
        if (remaining <= 0) {
            voices[i].active = false;
            continue;
        }

        int to_mix = (remaining < (uint32_t)len) ? (int)remaining : len;
        SDL_MixAudioFormat(stream, voices[i].s->data + voices[i].pos,
                           audio_spec.format, to_mix, 64);
        voices[i].pos += to_mix;

        if (voices[i].pos >= voices[i].s->len) {
            voices[i].active = false;
        }
    }

    if (music_playing_cmd > 0 && music_playing_cmd < MUSIC_SONGS) {
        Sample *s = music_is_ovr ? &music_ovr[music_playing_cmd]
                                 : &music[music_playing_cmd];
        uint32_t loop = music_is_ovr ? 0 : music_loop[music_playing_cmd];
        if (s->data && s->len > loop) {
            int remaining = len;
            int offset = 0;
            while (remaining > 0) {
                if (music_pos >= s->len)
                    music_pos = loop;
                int avail = (int)(s->len - music_pos);
                int to_copy = (avail < remaining) ? avail : remaining;
                SDL_MixAudioFormat((Uint8 *)stream + offset, s->data + music_pos,
                                   audio_spec.format, to_copy, 96);
                music_pos += (uint32_t)to_copy;
                offset += to_copy;
                remaining -= to_copy;
            }
        }
    }
}


/* Copy the rest of a mapping line as a label: trim leading blanks and the
 * trailing newline. Empty input yields an empty string, not garbage. */
static void copy_label(char *dst, size_t dstsz, const char *src)
{
    while (*src == ' ' || *src == '\t')
        src++;

    size_t n = 0;
    while (src[n] && src[n] != '\n' && src[n] != '\r' && n < dstsz - 1)
        n++;
    memcpy(dst, src, n);
    dst[n] = '\0';

    /* Trim trailing blanks */
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\t'))
        dst[--n] = '\0';
}


static int load_sound_mapping(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* Skip comments and blank lines */
        if (line[0] == '#' || line[0] == '\n')
            continue;

        unsigned int cmd, bank, phrase;
        char wav[128];
        int consumed = 0;

        /* "stop <cmd> [label]" — silence everything, reset bank. */
        if (!strncmp(line, "stop", 4)) {
            if (sscanf(line, "stop %x %n", &cmd, &consumed) < 1)
                continue;
            if (stop_map_count >= 8) {
                fprintf(stderr, "audio: too many stop rows in %s\n", path);
                continue;
            }
            stop_map[stop_map_count] = (uint8_t)cmd;
            copy_label(stop_labels[stop_map_count],
                       sizeof stop_labels[stop_map_count],
                       consumed > 0 ? line + consumed : "");
            stop_map_count++;
            continue;
        }

        /* "music <cmd> [label]" — YM2151 command, no OKI sample by design. */
        if (!strncmp(line, "music", 5)) {
            if (sscanf(line, "music %x %n", &cmd, &consumed) < 1)
                continue;
            if (music_map_count >= 32) {
                fprintf(stderr, "audio: too many music rows in %s\n", path);
                continue;
            }
            music_map[music_map_count].cmd = (uint8_t)cmd;
            copy_label(music_map[music_map_count].label,
                       sizeof music_map[music_map_count].label,
                       line + consumed);
            music_map_count++;
            continue;
        }

        /* Bank field is 0, 1, or "cur" (play from the current bank). */
        char bankstr[8];
        if (sscanf(line, "%x %7s %u %127s %n", &cmd, bankstr, &phrase, wav,
                   &consumed) < 4)
            continue;
        if (!strcmp(bankstr, "cur"))
            bank = BANK_CURRENT;
        else if (bankstr[0] >= '0' && bankstr[0] <= '1' && !bankstr[1])
            bank = (unsigned int)(bankstr[0] - '0');
        else
            continue;

        if (cmd_map_count >= MAX_CMD_MAP) {
            fprintf(stderr, "audio: too many mappings in %s\n", path);
            break;
        }

        cmd_map[cmd_map_count].cmd = (uint8_t)cmd;
        cmd_map[cmd_map_count].bank = (uint8_t)bank;
        cmd_map[cmd_map_count].phrase = (uint8_t)phrase;
        /* %n is not counted by the sscanf return value, so a row with no
         * label leaves consumed at 0 — clamp rather than index backwards. */
        copy_label(cmd_map[cmd_map_count].label,
                   sizeof cmd_map[cmd_map_count].label,
                   consumed > 0 ? line + consumed : "");
        cmd_map_count++;

        /* Pre-load this sample */
        /* A "cur" row can be read from either bank at runtime, so preload
         * both. (Phrases 1-19 are present and identical in both banks.) */
        if (bank == BANK_CURRENT) {
            load_sample(samples_path, 0, phrase);
            load_sample(samples_path, 1, phrase);
        } else {
            load_sample(samples_path, (int)bank, phrase);
        }
    }

    fclose(f);
    return 0;
}


static int load_sample(const char *base_path, int bank, int phrase)
{
    if (phrase >= 128 || bank >= 2)
        return -1;

    if (samples[phrase][bank].data)
        return 0;  /* Already loaded */

    char path[512];
    snprintf(path, sizeof(path), "%s/bank%d_%03d.wav", base_path, bank, phrase);

    if (SDL_LoadWAV(path, &samples[phrase][bank].spec,
                    &samples[phrase][bank].data,
                    &samples[phrase][bank].len) == NULL) {
        /* Not fatal — some phrases have no sample */
        return -1;
    }
    if (convert_to_dev(&samples[phrase][bank], 1) != 0) {
        fprintf(stderr, "audio: convert failed %s\n", path);
        return -1;
    }
    wf_asset_log("wav", path);
    return 0;
}

/* Convert a loaded WAV into the device format. from_sdl_wav: buffer came
 * from SDL_LoadWAV (FreeWAV) rather than malloc. */
static int convert_to_dev(Sample *s, int from_sdl_wav)
{
    SDL_AudioCVT cvt;
    uint8_t *out;

    if (!s->data)
        return -1;
    if (SDL_BuildAudioCVT(&cvt, s->spec.format, s->spec.channels, s->spec.freq,
                          audio_spec.format, audio_spec.channels,
                          audio_spec.freq) < 0)
        return -1;
    if (!cvt.needed) {
        if (from_sdl_wav) {
            out = malloc(s->len);
            if (!out)
                return -1;
            memcpy(out, s->data, s->len);
            SDL_FreeWAV(s->data);
            s->data = out;
        }
        s->spec = audio_spec;
        return 0;
    }
    cvt.len = (int)s->len;
    cvt.buf = malloc((size_t)cvt.len * (size_t)cvt.len_mult);
    if (!cvt.buf)
        return -1;
    memcpy(cvt.buf, s->data, s->len);
    if (from_sdl_wav)
        SDL_FreeWAV(s->data);
    else
        free(s->data);
    s->data = NULL;
    if (SDL_ConvertAudio(&cvt) != 0) {
        free(cvt.buf);
        return -1;
    }
    if ((uint32_t)cvt.len_cvt < (uint32_t)cvt.len * (uint32_t)cvt.len_mult) {
        out = realloc(cvt.buf, (size_t)cvt.len_cvt);
        if (out)
            cvt.buf = out;
    }
    s->data = cvt.buf;
    s->len = (uint32_t)cvt.len_cvt;
    s->spec = audio_spec;
    return 0;
}

static uint32_t music_skip_bytes(const Sample *s)
{
    double sec = (double)MUSIC_SKIP_FRAME / MUSIC_SKIP_HZ;
    unsigned frame = (unsigned)(s->spec.channels * 2);
    uint32_t skip;

    if (!frame || !s->spec.freq)
        return 0;
    skip = (uint32_t)(sec * (double)s->spec.freq) * frame;
    if (skip >= s->len)
        return 0;
    return skip;
}

static int load_music(void)
{
    int cmd, n = 0;

    for (cmd = 1; cmd < MUSIC_SONGS; cmd++) {
        char path[64];
        Sample *s = &music[cmd];

        snprintf(path, sizeof path, "data/music/cmd_%02X.wav", cmd);
        if (SDL_LoadWAV(path, &s->spec, &s->data, &s->len) == NULL) {
            fprintf(stderr, "audio: missing %s\n", path);
            return -1;
        }
        if (convert_to_dev(s, 1) != 0) {
            fprintf(stderr, "audio: convert failed %s\n", path);
            return -1;
        }
        music_loop[cmd] = music_skip_bytes(s);
        wf_asset_log("music", path);
        n++;
    }
    fprintf(stderr, "audio: music ready (%d songs, skip %.2fs)\n",
            n, (double)MUSIC_SKIP_FRAME / MUSIC_SKIP_HZ);
    return 0;
}

static void music_stop(void)
{
    music_playing_cmd = 0;
    music_pos = 0;
    music_is_ovr = 0;
}

/* editor: forget cached override lookups after a profile switch */
void audio_reset_music_overrides(void)
{
    if (audio_dev) SDL_LockAudioDevice(audio_dev);
    for (int i = 0; i < MUSIC_SONGS; i++) {
        if (music_ovr_state[i] > 0) free(music_ovr[i].data);   /* convert_to_dev mallocs */
        memset(&music_ovr[i], 0, sizeof music_ovr[i]);
        music_ovr_state[i] = 0;
    }
    if (music_is_ovr) { music_playing_cmd = 0; music_pos = 0; music_is_ovr = 0; }
    if (audio_dev) SDL_UnlockAudioDevice(audio_dev);
}

int audio_ready(void) { return audio_dev != 0; }

/* sounds/ library one-shots: small path-keyed cache of device-format clips */
#define WAV_CACHE 24
static struct { char path[300]; Sample s; double secs; int bad; } wav_cache[WAV_CACHE];
static int wav_cache_n, wav_cache_next;
double audio_play_wav(const char *path)
{
    int k;
    if (audio_dev == 0 || !path || !path[0]) return 0;
    for (k = 0; k < wav_cache_n; k++) if (!strcmp(wav_cache[k].path, path)) break;
    if (k == wav_cache_n) {
        struct stat st;
        if (wav_cache_n < WAV_CACHE) k = wav_cache_n++;
        else { k = wav_cache_next; wav_cache_next = (wav_cache_next + 1) % WAV_CACHE;
               if (wav_cache[k].s.data) { SDL_LockAudioDevice(audio_dev); for (int v = 0; v < MAX_VOICES; v++) if (voices[v].s == &wav_cache[k].s) voices[v].active = false; SDL_UnlockAudioDevice(audio_dev); free(wav_cache[k].s.data); } }
        memset(&wav_cache[k], 0, sizeof wav_cache[k]);
        snprintf(wav_cache[k].path, sizeof wav_cache[k].path, "%s", path);
        if (stat(path, &st) != 0 || SDL_LoadWAV(path, &wav_cache[k].s.spec, &wav_cache[k].s.data, &wav_cache[k].s.len) == NULL
            || convert_to_dev(&wav_cache[k].s, 1) != 0) {
            fprintf(stderr, "audio: bad wav %s\n", path);
            wav_cache[k].bad = 1; wav_cache[k].s.data = NULL; wav_cache[k].s.len = 0;
        } else {
            wav_cache[k].secs = sample_seconds(&wav_cache[k].s);
            wf_asset_log("wav", path);
        }
    }
    if (wav_cache[k].bad) return 0;
    SDL_LockAudioDevice(audio_dev);
    for (int v = 0; v < MAX_VOICES; v++) if (!voices[v].active) {
        voices[v].s = &wav_cache[k].s; voices[v].pos = 0; voices[v].active = true; break;
    }
    SDL_UnlockAudioDevice(audio_dev);
    return wav_cache[k].secs;
}

/* mod-layer music override: mods/<m>/music/cmd_XX.wav (lazy load) */
static Sample *music_override(uint8_t cmd)
{
    char rel[64], path[512];
    Sample *s;
    if (cmd == 0 || cmd >= MUSIC_SONGS) return NULL;
    if (music_ovr_state[cmd] < 0) return NULL;
    if (music_ovr_state[cmd] > 0) return &music_ovr[cmd];
    snprintf(rel, sizeof rel, "music/cmd_%02X.wav", cmd);
    if (!wf_mod_resolve(rel, path, sizeof path)) { music_ovr_state[cmd] = -1; return NULL; }
    s = &music_ovr[cmd];
    if (SDL_LoadWAV(path, &s->spec, &s->data, &s->len) == NULL
        || convert_to_dev(s, 1) != 0) {
        fprintf(stderr, "audio: bad override %s\n", path);
        music_ovr_state[cmd] = -1;
        return NULL;
    }
    fprintf(stderr, "audio: music override %s\n", path);
    wf_asset_log("music", path);
    music_ovr_state[cmd] = 1;
    return s;
}

static void music_start(uint8_t cmd)
{
    if (cmd == 0 || cmd >= MUSIC_SONGS || !music[cmd].data) {
        music_stop();
        return;
    }
    music_playing_cmd = cmd;
    music_pos = music_loop[cmd];
}


/* Seconds of audio in a loaded sample, for the debug log. */
static float sample_seconds(const Sample *s)
{
    int rate = s->spec.freq ? s->spec.freq : 8000;
    int ch = s->spec.channels ? s->spec.channels : 1;
    return (float)s->len / (float)(2 * ch) / (float)rate;
}


static void play_event_sample(uint8_t cmd)
{
    int matched = 0;

    /* YM path (Z80 0x0335): 0x00 fade/stop, 0x01-0x12 start song,
     * 0x1F special (no WAV). Attract 0x02 vs 0x03 is the drum scene. */
    if (cmd < 0x20) {
        if (cmd == 0x00) {
            if (audio_debug)
                fprintf(stderr, "audio: cmd=0x00  MUSIC STOP\n");
            SDL_LockAudioDevice(audio_dev);
            music_stop();
            SDL_UnlockAudioDevice(audio_dev);
            return;
        }
        if (cmd == 0x1F) {
            if (audio_debug)
                fprintf(stderr, "audio: cmd=0x1F  MUSIC SPECIAL (no WAV)\n");
            return;
        }
        if (cmd < MUSIC_SONGS && music[cmd].data) {
            if (audio_debug)
                fprintf(stderr, "audio: cmd=0x%02X  MUSIC  %5.2fs  data/music/cmd_%02X.wav\n",
                        cmd, sample_seconds(&music[cmd]), cmd);
            SDL_LockAudioDevice(audio_dev);
            music_start(cmd);
            SDL_UnlockAudioDevice(audio_dev);
            return;
        }
        if (audio_debug)
            fprintf(stderr, "audio: cmd=0x%02X  MUSIC stub (no WAV)\n", cmd);
        return;
    }

    /* Stop-all runs before the sample table: the Z80 handler at 0x0106 kills
     * every voice and resets the bank to 0. */
    for (int i = 0; i < stop_map_count; i++) {
        if (stop_map[i] == cmd) {
            if (audio_debug)
                fprintf(stderr, "audio: cmd=0x%02X  STOP ALL          %s\n",
                        cmd, stop_labels[i][0] ? stop_labels[i] : "stop");
            SDL_LockAudioDevice(audio_dev);
            for (int v = 0; v < MAX_VOICES; v++)
                voices[v].active = false;
            SDL_UnlockAudioDevice(audio_dev);
            current_bank = 0;
            return;
        }
    }

    /* Find all mappings for this command and play them */
    for (int i = 0; i < cmd_map_count; i++) {
        if (cmd_map[i].cmd == cmd) {
            uint8_t phrase = cmd_map[i].phrase;
            uint8_t bank;

            /* Resolve the bank exactly as the Z80 does: an explicit 0/1
             * switches the OKI bank, and switching stops everything already
             * playing; "cur" leaves the bank alone. */
            if (cmd_map[i].bank == BANK_CURRENT) {
                bank = (uint8_t)current_bank;
            } else {
                bank = cmd_map[i].bank;
                if (bank != current_bank) {
                    if (audio_debug)
                        fprintf(stderr, "audio: cmd=0x%02X  bank %d -> %d, "
                                        "stopping all voices\n",
                                cmd, current_bank, bank);
                    SDL_LockAudioDevice(audio_dev);
                    for (int v = 0; v < MAX_VOICES; v++)
                        voices[v].active = false;
                    SDL_UnlockAudioDevice(audio_dev);
                    current_bank = bank;
                }
            }
            const char *label = cmd_map[i].label[0] ? cmd_map[i].label
                                                    : "(unlabelled)";
            matched++;

            /* Stops arrive as their own "stop" row, handled above; the
             * generated table has no phrase-0 sample rows. */
            Sample *s = &samples[phrase][bank];
            if (!s->data) {
                /* Mapped but the .wav is missing — this one IS a bug. */
                fprintf(stderr,
                        "audio: cmd=0x%02X  bank%d_%03d  *** NOT LOADED, "
                        "SILENT ***   %s\n", cmd, bank, phrase, label);
                continue;
            }

            if (audio_debug)
                fprintf(stderr, "audio: cmd=0x%02X  bank%d_%03d  %5.2fs  %s\n",
                        cmd, bank, phrase, sample_seconds(s), label);

            /* Claim a voice (thread-safe) */
            SDL_LockAudioDevice(audio_dev);

            int slot = -1;
            /* Find free voice */
            for (int v = 0; v < MAX_VOICES; v++) {
                if (!voices[v].active) {
                    slot = v;
                    break;
                }
            }

            /* If none free, steal the oldest */
            if (slot < 0)
                slot = 0;

            voices[slot].s = s;
            voices[slot].pos = 0;
            voices[slot].active = true;

            SDL_UnlockAudioDevice(audio_dev);
        }
    }

    if (matched || !audio_debug)
        return;

    fprintf(stderr, "audio: cmd=0x%02X  *** UNMAPPED — no entry in "
                    "data/sound-commands.txt ***\n", cmd);
}