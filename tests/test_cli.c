/* Tests for the command-line parser (src/cli.c) and the IPC serialization. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/cli.h"
#include "../src/config.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); failures++; } \
} while (0)

/* Prepares opts with the default config and parses a literal argv. */
#define PARSE(...) \
    do { \
        char *argv[] = { "overlay-timer", ##__VA_ARGS__, NULL }; \
        int argc = (int)(sizeof(argv) / sizeof(argv[0])) - 1; \
        memset(&opts, 0, sizeof opts); \
        config_set_defaults(&opts.cfg); \
        ok = cli_parse(argc, argv, &opts); \
    } while (0)

static void test_countdown(void) {
    cli_opts_t opts; bool ok;
    PARSE("-t", "25:00");
    CHECK(ok);
    CHECK(opts.action == ACTION_START_TIMER);
    CHECK(opts.time_seconds == 1500);
    cli_free(&opts);
}

static void test_pomodoro(void) {
    cli_opts_t opts; bool ok;
    PARSE("--pomodoro", "4");
    CHECK(ok);
    CHECK(opts.action == ACTION_START_POMODORO);
    CHECK(opts.pomodoro_count == 4);
    cli_free(&opts);

    PARSE("--pomodoro", "0");
    CHECK(!ok); /* invalid count */
    cli_free(&opts);

    PARSE("--pomodoro", "4", "--sync");
    CHECK(ok && opts.action == ACTION_START_POMODORO && opts.sync);
    cli_free(&opts);

    PARSE("--sync", "--pomodoro", "2");
    CHECK(ok && opts.sync && opts.pomodoro_count == 2);
    cli_free(&opts);

    PARSE("-t", "10:00", "--sync");
    CHECK(!ok); /* --sync requires --pomodoro */
    cli_free(&opts);

    PARSE("--sync");
    CHECK(!ok);
    cli_free(&opts);
}

static void test_control(void) {
    cli_opts_t opts; bool ok;
    PARSE("--pause");
    CHECK(ok && opts.action == ACTION_PAUSE);
    cli_free(&opts);

    PARSE("--resume");
    CHECK(ok && opts.action == ACTION_RESUME);
    cli_free(&opts);

    PARSE("--stop");
    CHECK(ok && opts.action == ACTION_STOP);
    cli_free(&opts);
}

static void test_help_version(void) {
    cli_opts_t opts; bool ok;
    PARSE("--help");
    CHECK(ok && opts.action == ACTION_HELP);
    cli_free(&opts);

    PARSE("-V");
    CHECK(ok && opts.action == ACTION_VERSION);
    cli_free(&opts);
}

static void test_appearance_overrides(void) {
    cli_opts_t opts; bool ok;
    PARSE("-c", "green", "--alpha", "80", "--size", "15", "--corner", "tl");
    CHECK(ok);
    /* No primary action, but with overrides => RECONFIGURE. */
    CHECK(opts.action == ACTION_RECONFIGURE);
    CHECK(strcmp(opts.cfg.color, "green") == 0);
    CHECK(opts.cfg.alpha == 80);
    CHECK(opts.cfg.size == 15);
    CHECK(opts.cfg.corner == CORNER_TL);
    CHECK(opts.set.color && opts.set.alpha && opts.set.size && opts.set.corner);
    CHECK(!opts.set.font); /* not set */
    cli_free(&opts);
}

static void test_invalid_values(void) {
    cli_opts_t opts; bool ok;
    PARSE("--alpha", "200"); CHECK(!ok); cli_free(&opts);
    PARSE("--size", "-5");   CHECK(!ok); cli_free(&opts);
    PARSE("--corner", "xx"); CHECK(!ok); cli_free(&opts);
    PARSE("-t", "99:99");    CHECK(!ok); cli_free(&opts);
    PARSE("--format", "zzz");CHECK(!ok); cli_free(&opts);
}

static void test_backend_flags(void) {
    cli_opts_t opts; bool ok;
    PARSE("--wayland");
    CHECK(ok && opts.cfg.backend == BACKEND_WAYLAND && opts.set.backend);
    cli_free(&opts);

    PARSE("--x11");
    CHECK(ok && opts.cfg.backend == BACKEND_X11 && opts.set.backend);
    cli_free(&opts);
}

static void test_sound_flags(void) {
    cli_opts_t opts; bool ok;
    PARSE("--no-sound");
    CHECK(ok && opts.cfg.enable_sound == false && opts.set.enable_sound);
    cli_free(&opts);

    PARSE("--sound");
    CHECK(ok && opts.cfg.enable_sound == true && opts.set.enable_sound);
    cli_free(&opts);
}

static void test_none_action(void) {
    cli_opts_t opts; bool ok;
    PARSE();
    CHECK(ok && opts.action == ACTION_NONE);
    cli_free(&opts);
}

static void test_set_message(void) {
    cli_opts_t opts; bool ok;
    PARSE("-c", "#00ff00", "--corner", "br", "--alpha", "50");
    CHECK(ok);
    char buf[1024];
    int n = cli_build_set_message(&opts, buf, sizeof buf);
    CHECK(n == 3);
    CHECK(strncmp(buf, "SET", 3) == 0);
    CHECK(strstr(buf, "color=#00ff00") != NULL);
    CHECK(strstr(buf, "corner=br") != NULL);
    CHECK(strstr(buf, "alpha=50") != NULL);
    cli_free(&opts);

    /* screen becomes a comma-separated list. */
    PARSE("--screen", "0,1,2");
    CHECK(ok);
    n = cli_build_set_message(&opts, buf, sizeof buf);
    CHECK(n == 1);
    CHECK(strstr(buf, "screen=0,1,2") != NULL);
    cli_free(&opts);
}

int main(void) {
    test_countdown();
    test_pomodoro();
    test_control();
    test_help_version();
    test_appearance_overrides();
    test_invalid_values();
    test_backend_flags();
    test_sound_flags();
    test_none_action();
    test_set_message();
    if (failures == 0) { printf("all cli tests passed\n"); return 0; }
    printf("%d cli test(s) failed\n", failures);
    return 1;
}
