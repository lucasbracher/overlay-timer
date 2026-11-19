/* Simple tests for src/config.c (no external framework). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* No /tmp on native Windows: create the temp file in the current dir. */
#ifdef _WIN32
#define TMP_TEMPLATE "overlay-timer-test-XXXXXX"
#else
#define TMP_TEMPLATE "/tmp/overlay-timer-test-XXXXXX"
#endif

#include "../src/config.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); failures++; } \
} while (0)

static void test_defaults(void) {
    config_t c;
    config_set_defaults(&c);
    CHECK(strcmp(c.color, "red") == 0);
    CHECK(c.alpha == 20);
    CHECK(c.size == 20);
    CHECK(c.corner == CORNER_BR);
    CHECK(c.n_screens == 1 && c.screens[0] == 0);
    CHECK(c.format == FMT_MMSS);
    CHECK(c.pomodoro_minutes == 25);
    CHECK(c.break_minutes == 5);
    CHECK(strcmp(c.break_color, "green") == 0);
    CHECK(c.enable_sound == true);
    CHECK(c.backend == BACKEND_AUTO);
    config_free(&c);
}

static void test_roundtrip(void) {
    config_t c;
    config_set_defaults(&c);
    free(c.color); c.color = strdup("#123456");
    c.alpha = 55;
    c.corner = CORNER_TL;
    c.n_screens = 2; c.screens[0] = 0; c.screens[1] = 1;
    c.format = FMT_HHMMSS;
    c.enable_sound = false;
    c.backend = BACKEND_X11;

    char tmpl[] = TMP_TEMPLATE;
    int fd = mkstemp(tmpl);
    CHECK(fd >= 0);
    close(fd);

    CHECK(config_save_file(&c, tmpl));
    config_free(&c);

    config_t d;
    config_set_defaults(&d);
    CHECK(config_load_file(&d, tmpl));
    CHECK(strcmp(d.color, "#123456") == 0);
    CHECK(d.alpha == 55);
    CHECK(d.corner == CORNER_TL);
    CHECK(d.n_screens == 2 && d.screens[1] == 1);
    CHECK(d.format == FMT_HHMMSS);
    CHECK(d.enable_sound == false);
    CHECK(d.backend == BACKEND_X11);
    config_free(&d);

    unlink(tmpl);
}

static void test_merge(void) {
    /* load over defaults only changes keys present in the file. */
    char tmpl[] = TMP_TEMPLATE;
    int fd = mkstemp(tmpl);
    CHECK(fd >= 0);
    FILE *fp = fdopen(fd, "w");
    fputs("[appearance]\ncolor = blue\n", fp);
    fclose(fp);

    config_t c;
    config_set_defaults(&c);
    CHECK(config_load_file(&c, tmpl));
    CHECK(strcmp(c.color, "blue") == 0); /* alterado */
    CHECK(c.alpha == 20);                /* preservado */
    CHECK(c.pomodoro_minutes == 25);     /* preservado */
    config_free(&c);
    unlink(tmpl);
}

static void test_screen_list(void) {
    int out[MAX_SCREENS], n;
    CHECK(parse_screen_list("0,1,2", out, MAX_SCREENS, &n) && n == 3 && out[2] == 2);
    CHECK(parse_screen_list("3", out, MAX_SCREENS, &n) && n == 1 && out[0] == 3);
    CHECK(!parse_screen_list("", out, MAX_SCREENS, &n));
    CHECK(!parse_screen_list("a,b", out, MAX_SCREENS, &n));
}

int main(void) {
    test_defaults();
    test_roundtrip();
    test_merge();
    test_screen_list();
    if (failures == 0) { printf("all config tests passed\n"); return 0; }
    printf("%d config test(s) failed\n", failures);
    return 1;
}
