#ifndef OVERLAY_TIMER_TIMER_H
#define OVERLAY_TIMER_TIMER_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"

/* Timer session type. */
typedef enum {
    TIMER_COUNTDOWN = 0,
    TIMER_POMODORO
} timer_kind_t;

/* Current phase (only relevant in pomodoro mode). */
typedef enum {
    PHASE_WORK = 0,
    PHASE_BREAK,
    PHASE_LONG_BREAK
} timer_phase_t;

/* Event returned by timer_tick(). */
typedef enum {
    TEVENT_NONE = 0,     /* nothing special on this tick */
    TEVENT_POMODORO_END, /* a work block ended (not the last one) */
    TEVENT_BREAK_END,    /* a break ended */
    TEVENT_FINISHED      /* the whole session ended (countdown or last pomodoro) */
} timer_event_t;

typedef struct {
    timer_kind_t kind;
    time_format_t format;

    long remaining;      /* seconds remaining in the current phase/count */
    bool paused;
    bool finished;

    /* Pomodoro. */
    int  pomo_total;     /* total pomodoros */
    int  pomo_index;     /* current pomodoro (1-based) */
    timer_phase_t phase;
    int  pomo_seconds;       /* duration of each pomodoro */
    int  break_seconds;      /* short break duration */
    int  long_break_seconds; /* long break duration */
    int  long_break_every;   /* long break every N pomodoros; 0 = never */
} otimer_t;

/* Initializes a simple countdown. */
void timer_init_countdown(otimer_t *t, long seconds, time_format_t fmt);

/* Initializes a pomodoro session using the durations from `cfg`. */
void timer_init_pomodoro(otimer_t *t, int count, const config_t *cfg);

/* Aligns a freshly initialized pomodoro session with the wall clock.
 * `cycle_offset` is how many seconds have elapsed in the current
 * (pomodoro + break) cycle, e.g. for 25+5 min cycles anchored at :00/:30,
 * the seconds since the last :00 or :30. If the offset falls inside the work
 * block, the first pomodoro starts with the remaining work time; otherwise
 * the session starts in the (short) break of "pomodoro 0" with the remaining
 * break time, and all N pomodoros still follow. */
void timer_sync_pomodoro(otimer_t *t, long cycle_offset);

/* Seconds elapsed in the current (work + break) cycle at local time
 * `secs_since_midnight`, with cycles anchored at local midnight. */
long timer_cycle_offset(long secs_since_midnight, int work_seconds,
                        int break_seconds);

/* Advances one second (must be called only when not paused).
 * Returns the resulting event and performs the phase transition if needed. */
timer_event_t timer_tick(otimer_t *t);

void timer_pause(otimer_t *t);
void timer_resume(otimer_t *t);

bool timer_is_paused(const otimer_t *t);
bool timer_is_finished(const otimer_t *t);

/* true if the current phase is a break (short or long). */
bool timer_in_break(const otimer_t *t);

/* Fills the counter text (e.g. "24:59"). */
void timer_text(const otimer_t *t, char *buf, size_t bufsz);

/* Fills the small label (e.g. "2/4" during work, "break" during a break).
 * For countdown, results in an empty string. */
void timer_label(const otimer_t *t, char *buf, size_t bufsz);

#endif /* OVERLAY_TIMER_TIMER_H */
