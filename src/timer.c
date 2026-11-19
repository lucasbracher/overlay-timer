#include "timer.h"

#include <stdio.h>
#include <string.h>

#include "util.h"

void timer_init_countdown(otimer_t *t, long seconds, time_format_t fmt) {
    memset(t, 0, sizeof *t);
    t->kind = TIMER_COUNTDOWN;
    t->format = fmt;
    t->remaining = seconds > 0 ? seconds : 0;
    t->finished = (t->remaining == 0);
}

void timer_init_pomodoro(otimer_t *t, int count, const config_t *cfg) {
    memset(t, 0, sizeof *t);
    t->kind = TIMER_POMODORO;
    t->format = cfg->format;
    t->pomo_total = count > 0 ? count : 1;
    t->pomo_index = 1;
    t->phase = PHASE_WORK;
    t->pomo_seconds = cfg->pomodoro_minutes * 60;
    t->break_seconds = cfg->break_minutes * 60;
    t->long_break_seconds = cfg->long_break_minutes * 60;
    t->long_break_every = cfg->long_break_every;
    t->remaining = t->pomo_seconds;
}

void timer_sync_pomodoro(otimer_t *t, long cycle_offset) {
    if (t->kind != TIMER_POMODORO) return;
    long cycle = (long)t->pomo_seconds + t->break_seconds;
    if (cycle <= 0) return;
    cycle_offset %= cycle;
    if (cycle_offset < 0) cycle_offset += cycle;

    if (cycle_offset < t->pomo_seconds) {
        t->pomo_index = 1;
        t->phase = PHASE_WORK;
        t->remaining = t->pomo_seconds - cycle_offset;
    } else {
        /* Break preceding the first pomodoro ("pomodoro 0"). */
        t->pomo_index = 0;
        t->phase = PHASE_BREAK;
        t->remaining = cycle - cycle_offset;
    }
}

long timer_cycle_offset(long secs_since_midnight, int work_seconds,
                        int break_seconds) {
    long cycle = (long)work_seconds + break_seconds;
    if (cycle <= 0) return 0;
    long off = secs_since_midnight % cycle;
    return off < 0 ? off + cycle : off;
}

/* Chooses the duration/phase of the break that follows the current pomodoro. */
static void enter_break(otimer_t *t) {
    bool is_long = (t->long_break_every > 0) &&
                   (t->pomo_index % t->long_break_every == 0);
    if (is_long && t->long_break_seconds > 0) {
        t->phase = PHASE_LONG_BREAK;
        t->remaining = t->long_break_seconds;
    } else {
        t->phase = PHASE_BREAK;
        t->remaining = t->break_seconds;
    }
}

timer_event_t timer_tick(otimer_t *t) {
    if (t->finished || t->paused) return TEVENT_NONE;
    if (t->remaining > 0) t->remaining--;
    if (t->remaining > 0) return TEVENT_NONE;

    /* remaining reached 0: resolve transition. */
    if (t->kind == TIMER_COUNTDOWN) {
        t->finished = true;
        return TEVENT_FINISHED;
    }

    /* Pomodoro. */
    if (t->phase == PHASE_WORK) {
        if (t->pomo_index >= t->pomo_total) {
            /* Last pomodoro: no break afterwards. */
            t->finished = true;
            return TEVENT_FINISHED;
        }
        enter_break(t);
        return TEVENT_POMODORO_END;
    } else {
        /* End of break: next pomodoro. */
        t->pomo_index++;
        t->phase = PHASE_WORK;
        t->remaining = t->pomo_seconds;
        return TEVENT_BREAK_END;
    }
}

void timer_pause(otimer_t *t)  { t->paused = true; }
void timer_resume(otimer_t *t) { t->paused = false; }

bool timer_is_paused(const otimer_t *t)   { return t->paused; }
bool timer_is_finished(const otimer_t *t) { return t->finished; }

bool timer_in_break(const otimer_t *t) {
    return t->kind == TIMER_POMODORO &&
           (t->phase == PHASE_BREAK || t->phase == PHASE_LONG_BREAK);
}

void timer_text(const otimer_t *t, char *buf, size_t bufsz) {
    format_time(t->remaining, t->format == FMT_HHMMSS, buf, bufsz);
}

void timer_label(const otimer_t *t, char *buf, size_t bufsz) {
    if (t->kind != TIMER_POMODORO) { buf[0] = '\0'; return; }
    if (timer_in_break(t))
        snprintf(buf, bufsz, "%s %d/%d",
                 t->phase == PHASE_LONG_BREAK ? "long break" : "break",
                 t->pomo_index, t->pomo_total);
    else
        snprintf(buf, bufsz, "%d/%d", t->pomo_index, t->pomo_total);
}
