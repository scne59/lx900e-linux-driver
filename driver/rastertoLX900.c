/*
 * rastertoLX900.c - CUPS raster filter for the Primera LX900e colour label
 * printer, implemented from scratch for Linux.
 *
 * Input : CUPS raster (application/vnd.cups-raster), ideally 8-bit RGB.
 * Output: the printer's native command/raster stream on stdout.
 *
 * The encoder itself lives in src/lx900_encoder.c, which the CUPS 3 printer
 * application shares.  This file is only the CUPS filter wrapper: it reads the
 * raster from stdin, maps the PPD options onto lx900_options_t and drives one
 * page at a time.
 *
 * Copyright (c) 2026.  Original work, no vendor code.
 * SPDX-License-Identifier: MIT
 */

#include "lx900.h"

#include <cups/cups.h>
#include <cups/raster.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Registration offsets live in /etc/lx900.conf so that every job sees them
 * without any per-job option:
 *
 *     # dots at 1200 dpi; can be negative
 *     left=0
 *     top=4
 *
 * Environment variables win over the file, which is what the test pages use.
 */
#ifndef LX900_CONF
#define LX900_CONF "/etc/lx900.conf"
#endif

static int debug_on;

static void logmsg(const char *fmt, ...)
{
    va_list ap;

    if (!debug_on)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void fatal(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static int conf_int(const char *key, int dflt)
{
    FILE *f = fopen(LX900_CONF, "r");
    char line[256], k[64];
    int n, v = dflt;

    if (!f)
        return dflt;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#')
            continue;
        if (sscanf(line, " %63[a-zA-Z0-9_] = %d", k, &n) == 2 && !strcmp(k, key))
            v = n;
    }
    fclose(f);
    return v;
}

static int env_int(const char *name, int dflt)
{
    const char *e = getenv(name);

    return e && *e ? atoi(e) : dflt;
}

static unsigned env_uint(const char *name, unsigned dflt)
{
    const char *s = getenv(name);

    if (s && *s) {
        char *end = NULL;
        unsigned long v = strtoul(s, &end, 0);

        if (end != s)
            return (unsigned)v;
    }
    return dflt;
}

/* The encoder's sink: stdout, with short writes treated as errors. */
static ssize_t write_stdout(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    return (ssize_t)fwrite(buf, 1, len, stdout);
}

/*
 * CUPS passes PPD options in argv[5] as "Name=Value" pairs.  Find one.
 */
static const char *option_value(const char *options, const char *name)
{
    size_t n = strlen(name);
    const char *p = options;

    while (p && *p) {
        if (!strncmp(p, name, n) && p[n] == '=')
            return p + n + 1;
        p = strchr(p, ' ');
        if (p)
            while (*p == ' ')
                p++;
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    cups_raster_t *ras;
    cups_page_header2_t hdr;
    lx900_options_t opt;
    lx900_job_t *job;
    const char *ink;
    unsigned char *line;
    int page = 0;

    /*
     * CUPS filter interface: argv[1] job id, [2] user, [3] title,
     * [4] copies, [5] options, [6] input file.
     */
    memset(&opt, 0, sizeof(opt));
    opt.copies = 1;
    if (argc > 4) {
        unsigned long v = strtoul(argv[4], NULL, 0);

        if (v > 0)
            opt.copies = (unsigned)v;
        if (opt.copies > 999)
            opt.copies = 999;
    }

    debug_on = getenv("LX900_DEBUG") != NULL;
    opt.debug = debug_on;
    opt.tag01 = env_uint("LX900_TAG01", 0);   /* 0 = derive from the ink mode */

    {
        int top = env_int("LX900_TOP_OFFSET", conf_int("top", 4));

        opt.left_offset = env_int("LX900_LEFT_OFFSET", conf_int("left", 0));
        opt.top_offset = top > 0 ? (unsigned)top : 0;
        if (debug_on)
            logmsg("DEBUG: registration top=%u left=%d dots\n",
                   opt.top_offset, opt.left_offset);
    }

    /*
     * Which cartridges are fitted.  An unset option means the PPD default,
     * Color + Black.
     */
    ink = argc > 5 ? option_value(argv[5], "InkCartridge") : NULL;
    opt.ink = ink ? lx900_ink_from_name(ink) : LX900_INK_COLORBLACK;

    if (debug_on) {
        static const char *planes[] = { "none", "C", "M", "Y", "K" };
        int used[LX900_MAX_PLANES] = { 0 };
        int i;

        for (i = 0; i < LX900_MAX_PLANES; i++) {
            if (opt.ink == LX900_INK_COLOR && i == LX900_PLANE_K)
                continue;
            used[i] = 1;
        }
        fprintf(stderr, "DEBUG: InkCartridge=%s -> planes",
                lx900_ink_name(opt.ink));
        for (i = 0; i < LX900_MAX_PLANES; i++)
            if (used[i])
                fprintf(stderr, " %s", planes[i + 1]);
        fputc('\n', stderr);
    }

    logmsg("DEBUG: rastertoLX900 starting\n");

    job = lx900_job_create(&opt, write_stdout, NULL);
    if (!job)
        fatal("ERROR: cannot create the encoder");

    ras = cupsRasterOpen(0, CUPS_RASTER_READ);
    if (!ras)
        fatal("ERROR: cannot open CUPS raster stream");

    line = NULL;
    while (cupsRasterReadHeader2(ras, &hdr)) {
        unsigned y, bpl;
        int rc;

        page++;
        if (debug_on)
            fprintf(stderr,
                    "DEBUG: page %d: %ux%u res=%ux%u cs=%d bpc=%d order=%d "
                    "bpl=%u\n",
                    page, hdr.cupsWidth, hdr.cupsHeight,
                    hdr.HWResolution[0], hdr.HWResolution[1],
                    hdr.cupsColorSpace, hdr.cupsBitsPerColor,
                    hdr.cupsColorOrder, hdr.cupsBytesPerLine);

        rc = lx900_page_begin(job, &hdr);
        if (rc < 0)
            fatal("ERROR: %s", lx900_job_error(job));

        /* height 0: page_begin kept it closed, so there is nothing to read */
        if (hdr.cupsHeight == 0)
            continue;

        bpl = hdr.cupsBytesPerLine;
        /* calloc, so a short read can never publish stale heap bytes */
        line = realloc(line, bpl ? bpl : 1);
        if (!line)
            fatal("ERROR: out of memory");
        memset(line, 0, bpl ? bpl : 1);

        for (y = 0; y < hdr.cupsHeight; y++) {
            if (cupsRasterReadPixels(ras, line, bpl) < bpl)
                fatal("ERROR: short raster read at line %u", y);
            if (lx900_page_row(job, &hdr, line, y) < 0)
                fatal("ERROR: %s", lx900_job_error(job));
        }

        if (lx900_page_end(job) < 0)
            fatal("ERROR: %s", lx900_job_error(job));

        if (debug_on && opt.copies > 1)
            fprintf(stderr, "DEBUG: page %d emitted %u times\n", page,
                    opt.copies);
        fflush(stdout);
    }

    if (lx900_job_end(job) < 0)
        fatal("ERROR: %s", lx900_job_error(job));

    free(line);
    lx900_job_destroy(job);
    cupsRasterClose(ras);
    return 0;
}
