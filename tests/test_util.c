/* Simple tests for src/util.c (no external framework). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/util.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); failures++; } \
} while (0)

static void test_parse_time(void) {
    long s;
    CHECK(parse_time("25:00", &s) && s == 1500);
    CHECK(parse_time("1:02:03", &s) && s == 3723);
    CHECK(parse_time("90", &s) && s == 90);
    CHECK(parse_time("00:59", &s) && s == 59);
    CHECK(!parse_time("25:60", &s));   /* invalid seconds */
    CHECK(!parse_time("1:60:00", &s)); /* invalid minutes */
    CHECK(!parse_time("abc", &s));
    CHECK(!parse_time("12:", &s));
    CHECK(!parse_time("", &s));
}

static void test_format_time(void) {
    char buf[16];
    CHECK(strcmp(format_time(1500, false, buf, sizeof buf), "25:00") == 0);
    CHECK(strcmp(format_time(3723, false, buf, sizeof buf), "01:02:03") == 0);
    CHECK(strcmp(format_time(59, true, buf, sizeof buf), "00:00:59") == 0);
    CHECK(strcmp(format_time(59, false, buf, sizeof buf), "00:59") == 0);
}

static void test_parse_color(void) {
    rgb_t c;
    CHECK(parse_color("red", &c) && c.r == 0xff && c.g == 0 && c.b == 0);
    CHECK(parse_color("#00ff00", &c) && c.r == 0 && c.g == 0xff && c.b == 0);
    CHECK(parse_color("#abc", &c) && c.r == 0xaa && c.g == 0xbb && c.b == 0xcc);
    CHECK(parse_color("GREEN", &c));
    CHECK(!parse_color("#xyz123", &c));
    CHECK(!parse_color("notacolor", &c));
}

static void test_trim(void) {
    char a[] = "  hello  ";
    CHECK(strcmp(str_trim(a), "hello") == 0);
    char b[] = "\t x \n";
    CHECK(strcmp(str_trim(b), "x") == 0);
}

int main(void) {
    test_parse_time();
    test_format_time();
    test_parse_color();
    test_trim();
    if (failures == 0) { printf("all util tests passed\n"); return 0; }
    printf("%d util test(s) failed\n", failures);
    return 1;
}
