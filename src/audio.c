#include "audio.h"

#include <ao/ao.h>
#include <math.h>
#include <pthread.h>
#include <sndfile.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "util.h"

#define SYNTH_RATE 44100
#define SYNTH_FREQ 880.0
#define SYNTH_LONG_MS 600
#define SYNTH_SHORT_MS 150
#define SYNTH_AMPL 9000

struct audio {
    bool enabled;
    bool ok;          /* libao initialized */
    char *dir;        /* sounds folder (expanded) or NULL */
    int   driver_id;

    pthread_t thread;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    bool running;
    int  pending;     /* pending sound_id_t, or -1 */
};

/* ------------------------------------------------------------------ */
/* Playback                                                           */
/* ------------------------------------------------------------------ */

static const char *sound_filename(sound_id_t id) {
    switch (id) {
        case SOUND_TIMER_END:    return "timer_end.wav";
        case SOUND_POMODORO_END: return "pomodoro_end.wav";
        case SOUND_BREAK_END:    return "break_end.wav";
        default:                 return NULL;
    }
}

static bool is_long_sound(sound_id_t id) {
    return id == SOUND_TIMER_END;
}

/* Plays a .wav file. Returns true if it played. */
static bool play_file(audio_t *a, const char *path) {
    SF_INFO info;
    memset(&info, 0, sizeof info);
    SNDFILE *sf = sf_open(path, SFM_READ, &info);
    if (!sf) return false;

    ao_sample_format fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.bits = 16;
    fmt.channels = info.channels;
    fmt.rate = info.samplerate;
    fmt.byte_format = AO_FMT_NATIVE;

    ao_device *dev = ao_open_live(a->driver_id, &fmt, NULL);
    if (!dev) { sf_close(sf); return false; }

    const int FRAMES = 4096;
    short *buf = xmalloc(sizeof(short) * FRAMES * info.channels);
    sf_count_t n;
    while ((n = sf_readf_short(sf, buf, FRAMES)) > 0) {
        ao_play(dev, (char *)buf, (uint_32)(n * info.channels * sizeof(short)));
    }
    free(buf);
    ao_close(dev);
    sf_close(sf);
    return true;
}

/* Generates and plays a triangular tone (fallback). */
static void play_synth(audio_t *a, bool longer) {
    ao_sample_format fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.bits = 16;
    fmt.channels = 1;
    fmt.rate = SYNTH_RATE;
    fmt.byte_format = AO_FMT_NATIVE;

    ao_device *dev = ao_open_live(a->driver_id, &fmt, NULL);
    if (!dev) return;

    int ms = longer ? SYNTH_LONG_MS : SYNTH_SHORT_MS;
    int total = SYNTH_RATE * ms / 1000;
    short *buf = xmalloc(sizeof(short) * total);

    double period = SYNTH_RATE / SYNTH_FREQ;
    for (int i = 0; i < total; i++) {
        /* Triangular wave in [-1,1]. */
        double phase = fmod(i / period, 1.0);
        double tri = 4.0 * fabs(phase - 0.5) - 1.0;
        /* Simple attack/decay envelope to avoid clicks. */
        double env = 1.0;
        int fade = SYNTH_RATE / 100; /* 10 ms */
        if (i < fade) env = (double)i / fade;
        else if (i > total - fade) env = (double)(total - i) / fade;
        buf[i] = (short)(tri * env * SYNTH_AMPL);
    }

    ao_play(dev, (char *)buf, (uint_32)(total * sizeof(short)));
    free(buf);
    ao_close(dev);
}

static void do_play(audio_t *a, sound_id_t id) {
    const char *fname = sound_filename(id);
    if (a->dir && fname) {
        size_t n = strlen(a->dir) + 1 + strlen(fname) + 1;
        char *path = xmalloc(n);
        snprintf(path, n, "%s/%s", a->dir, fname);
        bool played = play_file(a, path);
        free(path);
        if (played) return;
    }
    play_synth(a, is_long_sound(id));
}

/* ------------------------------------------------------------------ */
/* Thread                                                             */
/* ------------------------------------------------------------------ */

static void *audio_thread(void *arg) {
    audio_t *a = arg;
    pthread_mutex_lock(&a->mtx);
    while (a->running) {
        while (a->running && a->pending < 0)
            pthread_cond_wait(&a->cond, &a->mtx);
        if (!a->running) break;
        int id = a->pending;
        a->pending = -1;
        pthread_mutex_unlock(&a->mtx);

        do_play(a, (sound_id_t)id);

        pthread_mutex_lock(&a->mtx);
    }
    pthread_mutex_unlock(&a->mtx);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

audio_t *audio_init(const char *sounds_dir, bool enabled) {
    audio_t *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->enabled = enabled;
    a->pending = -1;
    a->dir = sounds_dir ? path_expand_tilde(sounds_dir) : NULL;

    if (enabled) {
        ao_initialize();
        a->driver_id = ao_default_driver_id();
        if (a->driver_id < 0) {
            log_warn(_("audio: no default libao driver; sound disabled"));
            a->enabled = false;
        } else {
            a->ok = true;
        }
    }

    pthread_mutex_init(&a->mtx, NULL);
    pthread_cond_init(&a->cond, NULL);
    a->running = true;
    if (pthread_create(&a->thread, NULL, audio_thread, a) != 0) {
        a->running = false;
        log_warn(_("audio: could not start playback thread"));
    }
    return a;
}

void audio_play(audio_t *a, sound_id_t id) {
    if (!a || !a->enabled || !a->ok || !a->running) return;
    pthread_mutex_lock(&a->mtx);
    a->pending = (int)id;
    pthread_cond_signal(&a->cond);
    pthread_mutex_unlock(&a->mtx);
}

void audio_shutdown(audio_t *a) {
    if (!a) return;
    if (a->running) {
        pthread_mutex_lock(&a->mtx);
        a->running = false;
        pthread_cond_signal(&a->cond);
        pthread_mutex_unlock(&a->mtx);
        pthread_join(a->thread, NULL);
    }
    pthread_mutex_destroy(&a->mtx);
    pthread_cond_destroy(&a->cond);
    if (a->ok) ao_shutdown();
    free(a->dir);
    free(a);
}
