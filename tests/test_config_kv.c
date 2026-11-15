/* Tests for the enum conversions and config_set_kv (IPC "SET" protocol). */
#include <stdio.h>
#include <string.h>

#include "../src/config.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); failures++; } \
} while (0)

static void test_corner_enum(void) {
    corner_t c;
    CHECK(corner_from_str("tl", &c) && c == CORNER_TL);
    CHECK(corner_from_str("TR", &c) && c == CORNER_TR); /* case-insensitive */
    CHECK(corner_from_str("bl", &c) && c == CORNER_BL);
    CHECK(corner_from_str("br", &c) && c == CORNER_BR);
    CHECK(!corner_from_str("xx", &c));
    CHECK(strcmp(corner_to_str(CORNER_TL), "tl") == 0);
    CHECK(strcmp(corner_to_str(CORNER_BR), "br") == 0);
}

static void test_format_enum(void) {
    time_format_t f;
    CHECK(format_from_str("mmss", &f) && f == FMT_MMSS);
    CHECK(format_from_str("hhmmss", &f) && f == FMT_HHMMSS);
    CHECK(!format_from_str("bad", &f));
    CHECK(strcmp(format_to_str(FMT_MMSS), "mmss") == 0);
    CHECK(strcmp(format_to_str(FMT_HHMMSS), "hhmmss") == 0);
}

static void test_backend_enum(void) {
    backend_t b;
    CHECK(backend_from_str("auto", &b) && b == BACKEND_AUTO);
    CHECK(backend_from_str("x11", &b) && b == BACKEND_X11);
    CHECK(backend_from_str("wayland", &b) && b == BACKEND_WAYLAND);
    CHECK(backend_from_str("macos", &b) && b == BACKEND_MACOS);
    CHECK(!backend_from_str("vnc", &b));
    CHECK(strcmp(backend_to_str(BACKEND_AUTO), "auto") == 0);
    CHECK(strcmp(backend_to_str(BACKEND_X11), "x11") == 0);
    CHECK(strcmp(backend_to_str(BACKEND_WAYLAND), "wayland") == 0);
    CHECK(strcmp(backend_to_str(BACKEND_MACOS), "macos") == 0);
}

static void test_set_kv_strings(void) {
    config_t c; config_set_defaults(&c);
    CHECK(config_set_kv(&c, "color", "#abcdef") && strcmp(c.color, "#abcdef") == 0);
    CHECK(config_set_kv(&c, "font", "Mono") && strcmp(c.font, "Mono") == 0);
    CHECK(config_set_kv(&c, "pomodoro_color", "blue") && strcmp(c.pomodoro_color, "blue") == 0);
    CHECK(config_set_kv(&c, "break_color", "green") && strcmp(c.break_color, "green") == 0);
    CHECK(config_set_kv(&c, "sounds_dir", "/tmp/s") && strcmp(c.sounds_dir, "/tmp/s") == 0);
    config_free(&c);
}

static void test_set_kv_numbers(void) {
    config_t c; config_set_defaults(&c);
    CHECK(config_set_kv(&c, "alpha", "70") && c.alpha == 70);
    CHECK(config_set_kv(&c, "size", "33") && c.size == 33);
    CHECK(config_set_kv(&c, "pomodoro_minutes", "50") && c.pomodoro_minutes == 50);
    CHECK(config_set_kv(&c, "break_minutes", "10") && c.break_minutes == 10);
    CHECK(config_set_kv(&c, "long_break_every", "4") && c.long_break_every == 4);

    /* out of range => fails and does not change. */
    CHECK(!config_set_kv(&c, "alpha", "150"));
    CHECK(c.alpha == 70);
    CHECK(!config_set_kv(&c, "size", "-1"));
    CHECK(c.size == 33);
    config_free(&c);
}

static void test_set_kv_enums_and_bool(void) {
    config_t c; config_set_defaults(&c);
    CHECK(config_set_kv(&c, "corner", "tl") && c.corner == CORNER_TL);
    CHECK(config_set_kv(&c, "format", "hhmmss") && c.format == FMT_HHMMSS);
    CHECK(config_set_kv(&c, "backend", "x11") && c.backend == BACKEND_X11);
    CHECK(config_set_kv(&c, "enable_sound", "false") && c.enable_sound == false);
    CHECK(config_set_kv(&c, "enable_sound", "on") && c.enable_sound == true);
    CHECK(config_set_kv(&c, "screen", "2,3") && c.n_screens == 2 && c.screens[1] == 3);

    /* unknown key or invalid value. */
    CHECK(!config_set_kv(&c, "nope", "x"));
    CHECK(!config_set_kv(&c, "corner", "zz"));
    CHECK(!config_set_kv(&c, "enable_sound", "maybe"));
    config_free(&c);
}

static void test_copy_is_deep(void) {
    config_t a; config_set_defaults(&a);
    config_set_kv(&a, "color", "magenta");

    config_t b;
    config_copy(&b, &a);
    CHECK(strcmp(b.color, "magenta") == 0);
    CHECK(b.color != a.color); /* distinct pointers (deep copy) */

    /* Changing b does not affect a. */
    config_set_kv(&b, "color", "cyan");
    CHECK(strcmp(a.color, "magenta") == 0);
    CHECK(strcmp(b.color, "cyan") == 0);

    config_free(&a);
    config_free(&b);
}

int main(void) {
    test_corner_enum();
    test_format_enum();
    test_backend_enum();
    test_set_kv_strings();
    test_set_kv_numbers();
    test_set_kv_enums_and_bool();
    test_copy_is_deep();
    if (failures == 0) { printf("all config_kv tests passed\n"); return 0; }
    printf("%d config_kv test(s) failed\n", failures);
    return 1;
}
