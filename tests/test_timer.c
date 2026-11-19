/* Tests for the timer logic (countdown and pomodoro). */
#include <stdio.h>
#include <string.h>

#include "../src/timer.h"
#include "../src/config.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); failures++; } \
} while (0)

/* Advances n ticks collecting the last event != NONE. */
static timer_event_t run_ticks(otimer_t *t, int n) {
    timer_event_t last = TEVENT_NONE;
    for (int i = 0; i < n; i++) {
        timer_event_t e = timer_tick(t);
        if (e != TEVENT_NONE) last = e;
    }
    return last;
}

static void test_countdown(void) {
    otimer_t t;
    timer_init_countdown(&t, 3, FMT_MMSS);
    char buf[16];
    timer_text(&t, buf, sizeof buf);
    CHECK(strcmp(buf, "00:03") == 0);

    CHECK(timer_tick(&t) == TEVENT_NONE); /* 2 */
    CHECK(timer_tick(&t) == TEVENT_NONE); /* 1 */
    CHECK(timer_tick(&t) == TEVENT_FINISHED); /* 0 */
    CHECK(timer_is_finished(&t));
    CHECK(timer_tick(&t) == TEVENT_NONE); /* does not go past finished */
}

static void test_pause(void) {
    otimer_t t;
    timer_init_countdown(&t, 2, FMT_MMSS);
    timer_pause(&t);
    CHECK(timer_tick(&t) == TEVENT_NONE);
    CHECK(timer_tick(&t) == TEVENT_NONE);
    CHECK(!timer_is_finished(&t)); /* pausado nao avanca */
    timer_resume(&t);
    CHECK(timer_tick(&t) == TEVENT_NONE);
    CHECK(timer_tick(&t) == TEVENT_FINISHED);
}

static void test_pomodoro_basic(void) {
    config_t c; config_set_defaults(&c);
    c.pomodoro_minutes = 1;  /* 60s */
    c.break_minutes = 1;     /* 60s */
    c.long_break_every = 0;

    otimer_t t;
    timer_init_pomodoro(&t, 2, &c); /* 2 pomodoros */

    char lbl[64];
    timer_label(&t, lbl, sizeof lbl);
    CHECK(strcmp(lbl, "1/2") == 0);

    /* 1st pomodoro (60s) -> end of pomodoro, enters break */
    CHECK(run_ticks(&t, 60) == TEVENT_POMODORO_END);
    CHECK(timer_in_break(&t));

    /* break (60s) -> end of break, next pomodoro */
    CHECK(run_ticks(&t, 60) == TEVENT_BREAK_END);
    CHECK(!timer_in_break(&t));
    timer_label(&t, lbl, sizeof lbl);
    CHECK(strcmp(lbl, "2/2") == 0);

    /* ultimo pomodoro (60s) -> FINISHED, sem pausa */
    CHECK(run_ticks(&t, 60) == TEVENT_FINISHED);
    CHECK(timer_is_finished(&t));
}

static void test_long_break(void) {
    config_t c; config_set_defaults(&c);
    c.pomodoro_minutes = 1;
    c.break_minutes = 1;
    c.long_break_minutes = 2;
    c.long_break_every = 2; /* pausa longa a cada 2 pomodoros */

    otimer_t t;
    timer_init_pomodoro(&t, 3, &c);

    run_ticks(&t, 60); /* fim pomodoro 1 -> pausa curta (idx 1, nao multiplo de 2) */
    CHECK(t.phase == PHASE_BREAK);
    run_ticks(&t, 60); /* fim pausa -> pomodoro 2 */
    run_ticks(&t, 60); /* fim pomodoro 2 (idx 2, multiplo de 2) -> pausa longa */
    CHECK(t.phase == PHASE_LONG_BREAK);
    CHECK(t.remaining == 120);
}

/* Starts a synced 25+5 session at mm:ss past the hour. */
static void synced_at(otimer_t *t, int count, int mm, int ss) {
    config_t c; config_set_defaults(&c); /* 25 + 5 */
    c.long_break_every = 4;
    timer_init_pomodoro(t, count, &c);
    long since_midnight = 14 * 3600L + mm * 60L + ss;
    timer_sync_pomodoro(t, timer_cycle_offset(since_midnight, t->pomo_seconds,
                                              t->break_seconds));
    config_free(&c);
}

static void test_sync(void) {
    otimer_t t;
    char lbl[64];
    /* work phase of the first half-hour */
    synced_at(&t, 4, 2, 20);
    CHECK(t.phase == PHASE_WORK && t.pomo_index == 1 && t.remaining == 22 * 60 + 40);
    synced_at(&t, 4, 19, 30);
    CHECK(t.phase == PHASE_WORK && t.remaining == 5 * 60 + 30);
    /* break of "pomodoro 0" (short, even with long_break_every) */
    synced_at(&t, 4, 28, 10);
    CHECK(t.phase == PHASE_BREAK && t.pomo_index == 0 && t.remaining == 60 + 50);
    timer_label(&t, lbl, sizeof lbl);
    CHECK(strcmp(lbl, "break 0/4") == 0);
    /* second half-hour */
    synced_at(&t, 4, 32, 20);
    CHECK(t.phase == PHASE_WORK && t.pomo_index == 1 && t.remaining == 22 * 60 + 40);
    synced_at(&t, 4, 49, 30);
    CHECK(t.phase == PHASE_WORK && t.remaining == 5 * 60 + 30);
    synced_at(&t, 4, 58, 10);
    CHECK(t.phase == PHASE_BREAK && t.pomo_index == 0 && t.remaining == 60 + 50);
    /* exactly on the boundary: full pomodoro */
    synced_at(&t, 4, 30, 0);
    CHECK(t.phase == PHASE_WORK && t.remaining == 25 * 60);

    /* break 0 ends -> pomodoro 1 of N, all N still run */
    synced_at(&t, 1, 58, 10);
    CHECK(run_ticks(&t, 110) == TEVENT_BREAK_END);
    CHECK(t.phase == PHASE_WORK && t.pomo_index == 1 && t.remaining == 25 * 60);
    CHECK(run_ticks(&t, 25 * 60) == TEVENT_FINISHED);
}

int main(void) {
    test_sync();
    test_countdown();
    test_pause();
    test_pomodoro_basic();
    test_long_break();
    if (failures == 0) { printf("all timer tests passed\n"); return 0; }
    printf("%d timer test(s) failed\n", failures);
    return 1;
}
