/* Tests for the window-system-independent screen selection helpers. */
#include <stdio.h>
#include <string.h>

#include "../src/backend.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); failures++; } \
} while (0)

static void test_resolve_basic(void) {
    screen_layout_t l;
    int req[] = {0, 1};
    backend_resolve_screens(req, 2, 2, &l);
    CHECK(l.n_monitors == 2);
    CHECK(l.n_shown == 2 && l.shown[0] == 0 && l.shown[1] == 1);
    CHECK(!l.fallback);
}

static void test_resolve_order_kept(void) {
    screen_layout_t l;
    int req[] = {2, 0};
    backend_resolve_screens(req, 2, 3, &l);
    CHECK(l.n_shown == 2 && l.shown[0] == 2 && l.shown[1] == 0);
}

/* A monitor went away: the overlay for it is dropped, not stacked. */
static void test_resolve_monitor_removed(void) {
    screen_layout_t l;
    int req[] = {0, 1};
    backend_resolve_screens(req, 2, 1, &l);
    CHECK(l.n_shown == 1 && l.shown[0] == 0);
    CHECK(!l.fallback);
}

static void test_resolve_duplicates(void) {
    screen_layout_t l;
    int req[] = {1, 1, 0, 1};
    backend_resolve_screens(req, 4, 2, &l);
    CHECK(l.n_shown == 2 && l.shown[0] == 1 && l.shown[1] == 0);
}

static void test_resolve_fallback(void) {
    screen_layout_t l;
    int req[] = {3};
    backend_resolve_screens(req, 1, 2, &l);
    CHECK(l.n_shown == 1 && l.shown[0] == 0);
    CHECK(l.fallback);
}

static void test_resolve_no_monitor(void) {
    screen_layout_t l;
    int req[] = {0, 1};
    backend_resolve_screens(req, 2, 0, &l);
    CHECK(l.n_monitors == 0 && l.n_shown == 0 && !l.fallback);
}

static void test_describe(void) {
    char buf[256];
    screen_layout_t l;

    int a[] = {0, 1};
    backend_resolve_screens(a, 2, 2, &l);
    backend_describe_layout(a, 2, &l, buf, sizeof buf);
    CHECK(strcmp(buf, "showing on screen(s) 0,1 (2 monitor(s))") == 0);

    backend_resolve_screens(a, 2, 1, &l);
    backend_describe_layout(a, 2, &l, buf, sizeof buf);
    CHECK(strcmp(buf, "showing on screen(s) 0 (1 monitor(s)); unavailable: 1") == 0);

    int c[] = {4, 4, 5};
    backend_resolve_screens(c, 3, 2, &l);
    backend_describe_layout(c, 3, &l, buf, sizeof buf);
    CHECK(strcmp(buf, "showing on screen(s) 0 (2 monitor(s)); unavailable: 4,5; "
                      "falling back to screen 0") == 0);

    backend_resolve_screens(a, 2, 0, &l);
    backend_describe_layout(a, 2, &l, buf, sizeof buf);
    CHECK(strcmp(buf, "no monitor available; waiting for one") == 0);

    /* Truncation must not overflow. */
    char tiny[8];
    backend_resolve_screens(a, 2, 2, &l);
    backend_describe_layout(a, 2, &l, tiny, sizeof tiny);
    CHECK(strlen(tiny) < sizeof tiny);
}

int main(void) {
    test_resolve_basic();
    test_resolve_order_kept();
    test_resolve_monitor_removed();
    test_resolve_duplicates();
    test_resolve_fallback();
    test_resolve_no_monitor();
    test_describe();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("backend: all tests passed\n");
    return 0;
}
