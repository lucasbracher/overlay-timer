#ifndef OVERLAY_TIMER_AUDIO_H
#define OVERLAY_TIMER_AUDIO_H

#include <stdbool.h>

/*
 * Audio via libao + libsndfile.
 *
 * Plays .wav files from a configurable folder. If the file does not exist,
 * generates a synthesized triangular tone (long or short).
 *
 * Playback happens in its own thread so as not to block the daemon loop;
 * successive calls simply enqueue/restart the sound.
 */

typedef enum {
    SOUND_TIMER_END = 0,   /* end of the countdown or of the last pomodoro (long) */
    SOUND_POMODORO_END,    /* end of a work block (short) */
    SOUND_BREAK_END        /* end of a break (short) */
} sound_id_t;

typedef struct audio audio_t;

/* Initializes the audio subsystem.
 *   sounds_dir : folder with timer_end.wav / pomodoro_end.wav / break_end.wav
 *                (may contain '~'); may be NULL to use only the fallback.
 *   enabled    : if false, all play calls become no-ops.
 * Returns NULL on libao initialization failure. */
audio_t *audio_init(const char *sounds_dir, bool enabled);

/* Plays the identified sound (asynchronous). */
void audio_play(audio_t *a, sound_id_t id);

/* Shuts down, waiting for the current sound to finish (up to a short limit). */
void audio_shutdown(audio_t *a);

#endif /* OVERLAY_TIMER_AUDIO_H */
