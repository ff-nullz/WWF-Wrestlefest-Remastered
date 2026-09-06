/* WrestleFest audio playback */

#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

/* Initialize audio. enable_percussion is accepted but unused: drums live
 * in attract music cmd 0x03, not a match-flag loop. */
int audio_init(const char *samples_dir, int enable_percussion);

/* Shutdown audio system. */
void audio_shutdown(void);

/* Called when 68000 writes to sound latch (0x14000C). */
void audio_on_sound_latch(uint16_t value);

/* Emulated-board extras (sndboard.c behind audio.c): drop cached mod
 * music overrides after a profile switch; is the device open (editor
 * lazy-inits audio for its Sounds panel). */
void audio_reset_music_overrides(void);
int  audio_ready(void);

/* One-shot WAV from the sounds/ library (wrestler sound map, editor
 * audition): mixed over the board; returns the clip's seconds, 0 on
 * failure. Decoded clips are cached by path. */
double audio_play_wav(const char *path);

#endif /* AUDIO_H */