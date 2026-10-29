/*
 * White-box tests for the status helpers in driver/lx900-backend.c.
 *
 * The backend has no library interface, so the tests include the source with
 * main() renamed and call the static functions directly.  The config file path
 * is overridden at build time so the test does not depend on /etc/lx900.conf.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define main backend_main
#include "../driver/lx900-backend.c"
#undef main

static int failures;
static int checks;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void put32(unsigned char *p, unsigned v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

/* A status block with the given per-tank usage percentages. */
static void make_status(unsigned char *s, int c, int m, int y, int k)
{
    memset(s, 0, STATUS_LEN);
    s[OFF_STATE] = 0x49;
    s[OFF_TANK_C] = s[OFF_TANK_M] = s[OFF_TANK_Y] = s[OFF_TANK_K] = TANK_OK;
    put32(s + OFF_COUNTER_C, (unsigned)(1800000000.0 * (100 - c) / 100.0));
    put32(s + OFF_COUNTER_M, (unsigned)(1800000000.0 * (100 - m) / 100.0));
    put32(s + OFF_COUNTER_Y, (unsigned)(1800000000.0 * (100 - y) / 100.0));
    put32(s + OFF_COUNTER_K, (unsigned)(3186000000.0 * (100 - k) / 100.0));
}

static void test_media_present(void)
{
    unsigned char s[STATUS_LEN];

    memset(s, 0, sizeof s);
    s[OFF_PAPER] = 1;
    check(media_present(s), "stock present, no error");
    s[OFF_PAPER] = 0;
    check(!media_present(s), "no stock flagged");
    s[OFF_PAPER] = 1;
    s[OFF_ERROR] = 0x01;
    check(!media_present(s), "loop sensor out of stock");
    s[OFF_ERROR] = 0x36;              /* the power-on transient */
    check(media_present(s), "startup error does not look like a media fault");
}

static void test_conf(void)
{
    FILE *f = fopen(LX900_CONF, "w");

    check(f != NULL, "can write the temporary config");
    if (!f)
        return;
    fputs("# comment\n\npaper_check=0\n"
          "  paper_wait = 45  \nleft=-7\nnot a setting\n", f);
    fclose(f);
    check(conf_int("paper_check", 1) == 0, "conf: paper_check");
    check(conf_int("paper_wait", 120) == 45, "conf: paper_wait with spaces");
    check(conf_int("left", 0) == -7, "conf: negative value");
    check(conf_int("missing", 9) == 9, "conf: default for a missing key");
}

static void test_percentages(void)
{
    unsigned char s[STATUS_LEN];

    make_status(s, 39, 21, 33, 46);
    check(percent_remaining(0, s) == 39, "cyan percentage");
    check(percent_remaining(1, s) == 21, "magenta percentage");
    check(percent_remaining(2, s) == 33, "yellow percentage");
    check(percent_remaining(3, s) == 46, "black percentage");

    s[OFF_TANK_M] = 0x02;
    check(percent_remaining(1, s) == -2, "missing cartridge reports -2");

    make_status(s, 0, 0, 0, 0);
    put32(s + OFF_COUNTER_C, 0xffffffffu);
    check(percent_remaining(0, s) == -1, "impossible counter reports unknown");

    make_status(s, 0, 0, 0, 0);       /* counters at full scale = empty */
    check(percent_remaining(0, s) == 0, "empty cartridge reports 0");
}

static void test_marker_output(void)
{
    unsigned char s[STATUS_LEN];
    char buf[4096];
    int saved = dup(STDERR_FILENO);
    FILE *tmp;
    size_t n;

    if (saved < 0) {
        printf("skip marker output test (cannot dup stderr)\n");
        return;
    }
    tmp = tmpfile();
    if (!tmp) {
        close(saved);
        printf("skip marker output test (no tmpfile)\n");
        return;
    }
    fflush(stderr);
    dup2(fileno(tmp), STDERR_FILENO);

    make_status(s, 39, 46, 33, 46);
    s[OFF_TANK_M] = 0x02;             /* magenta missing */
    put32(s + OFF_COUNTER_Y, (unsigned)(1800000000.0 * 0.95));
    report_levels(s);

    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);

    rewind(tmp);
    n = fread(buf, 1, sizeof buf - 1, tmp);
    buf[n] = 0;
    fclose(tmp);

    check(strstr(buf, "ATTR: marker-levels=39,-2,5,46") != NULL,
          "marker-levels reports the missing tank as -2");
    check(strstr(buf, "ATTR: marker-message=\"Replace the colour cartridge "
                      "(slot 1).") != NULL,
          "marker-message names the colour cartridge");
    check(strstr(buf, "STATE: +marker-supply-empty") != NULL,
          "empty supply raises the state reason");
    check(strstr(buf, "STATE: +marker-supply-low") != NULL,
          "low supply raises the state reason");
    check(strstr(buf, "-marker-supply-empty -marker-supply-low") != NULL,
          "stale reasons are cleared first");
}

int main(void)
{
    test_media_present();
    test_conf();
    test_percentages();
    test_marker_output();

    printf("%d/%d backend checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
