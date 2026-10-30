/*
 * SPDX-License-Identifier: MIT
 *
 * Encoder for the Primera LX900e.
 *
 * Everything that turns a CUPS raster page into the printer's stream lives
 * here: transport framing, the prologue and page-info packets, band records,
 * the CRDRE row codec, halftoning and the ink split.  The CUPS filter
 * (driver/rastertoLX900.c) and the printer application (pappl/lx900-app.c)
 * are both thin wrappers around it.
 */
#include "lx900.h"

#include <cups/cups.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAND_ROWS       240

/* 8.25 in at 1200 dpi: the width the print head covers. */
#define MAX_WIDTH_DOTS  9900

/* 18 in at 1200 dpi: the tallest page the PPD offers. */
#define MAX_HEIGHT_DOTS 21600

/* Plane numbers used by the band writer; the public names are in lx900.h. */
#define PLANE_C         LX900_PLANE_C
#define PLANE_M         LX900_PLANE_M
#define PLANE_Y         LX900_PLANE_Y
#define PLANE_K         LX900_PLANE_K
#define MAX_PLANES      LX900_MAX_PLANES

#ifndef CUPS_CSPACE_SRGB
#define CUPS_CSPACE_SRGB 19
#endif

typedef struct {
    unsigned char *data;        /* BAND_ROWS * wbytes */
    size_t         wbytes;
    unsigned       rows;
    unsigned       counts[BAND_ROWS];
    unsigned char *enc;         /* encoded row scratch, BAND_ROWS * rowbuf */
    unsigned char *tmp;         /* scratch for enc_row, BAND_ROWS * rowtmp */
    size_t         rowbuf;
    size_t         rowtmp;
} plane_t;

/* The four plane buffers, in plane order. */
static const int ids_color[] = { LX900_PLANE_C, LX900_PLANE_M, LX900_PLANE_Y };
static const int ids_both[] = { LX900_PLANE_C, LX900_PLANE_M, LX900_PLANE_Y,
                                LX900_PLANE_K };

struct lx900_job {
    lx900_options_t opt;
    lx900_write_fn  write;
    void           *ctx;
    int             failed;
    char            err[256];

    /* transport framing */
    uint8_t *frame_buf;
    size_t   frame_len;

    /* the page, captured so multi-copy jobs can repeat it */
    unsigned char *page_buf;
    size_t         page_len, page_cap;
    int            capturing;

    /* current page */
    int      page_open;
    int      page_skipped;
    unsigned page_width;
    unsigned usable;
    unsigned band_start;
    unsigned band_rows;
    const int *plane_ids;
    unsigned   nactive;
    plane_t  planes[LX900_MAX_PLANES];
    uint8_t *seed[LX900_MAX_PLANES];
};

static void lx900_fail(lx900_job_t *job, const char *fmt, ...)
{
    va_list ap;

    if (job->failed)
        return;
    job->failed = 1;
    va_start(ap, fmt);
    vsnprintf(job->err, sizeof(job->err), fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Encoder body                                                        */
/* ------------------------------------------------------------------ */

static size_t rle_pairs(const uint8_t *d, size_t n, uint8_t *out)
{
    size_t i = 0, o = 0;

    while (i < n) {
        uint8_t v = d[i];
        unsigned run = 0;

        i++;
        while (i < n && run != 0xff && d[i] == v) {
            run++;
            i++;
        }
        out[o++] = (uint8_t)run;
        out[o++] = v;
    }
    return o;
}

/*
 * Emit one block: skip `skip` bytes (taken from the seed row), then write
 * `cnt` bytes from `data`.  mode 1 = RLE, mode 0 = literal.
 */
static size_t emit_block(const uint8_t *data, size_t cnt, size_t skip, int mode,
                         uint8_t *out)
{
    unsigned mask1 = mode ? 3 : 0xf;
    unsigned shift = mode ? 5 : 3;
    unsigned max1  = mode ? 0x1f : 7;
    uint8_t  base  = mode ? 0x80 : 0x00;
    size_t   o = 0, n1, n2;
    size_t   v2 = cnt - 1;
    uint8_t  head[16];
    size_t   h = 0;
    uint8_t  ctrl = base;

    n1 = (skip < mask1) ? 0 : (skip - mask1) / 0xff + 1;
    n2 = (v2 < max1) ? 0 : (v2 - max1) / 0xff + 1;

    if (n1 == 0) {
        ctrl |= (uint8_t)((skip & 0xff) << shift);
    } else if (n1 == 1) {
        ctrl |= (uint8_t)(mask1 << shift);
        head[h++] = (uint8_t)(skip - mask1);
    } else {
        size_t k;

        ctrl |= (uint8_t)(mask1 << shift);
        for (k = 0; k + 1 < n1; k++)
            head[h++] = 0xff;
        head[h++] = (uint8_t)(n1 + (skip - 1) - mask1);
    }

    if (n2 == 0) {
        ctrl |= (uint8_t)(v2 & 0xff);
    } else if (n2 == 1) {
        ctrl |= (uint8_t)max1;
        head[h++] = (uint8_t)(v2 - max1);
    } else {
        size_t k;

        ctrl |= (uint8_t)max1;
        for (k = 0; k + 1 < n2; k++)
            head[h++] = 0xff;
        head[h++] = (uint8_t)(n2 + (v2 - 1) - max1);
    }

    out[o++] = ctrl;
    memcpy(out + o, head, h);
    o += h;

    if (mode)
        o += rle_pairs(data, cnt, out + o);
    else {
        memcpy(out + o, data, cnt);
        o += cnt;
    }
    return o;
}

/*
 * Encode `cur` relative to `seed`.  Runs of changed bytes are emitted as
 * RLE or literal, whichever is shorter for that run.
 *
 * `out` must hold at least 2*W + 64 bytes.
 */
static size_t enc_row(const uint8_t *seed, const uint8_t *cur, size_t W,
                      uint8_t *out, uint8_t *scratch)
{
    uint8_t *tmpa = scratch;
    uint8_t *tmpc = scratch + 2 * W + 64;
    size_t pos = 0, last = 0, o = 0;

    while (pos < W) {
        size_t i = pos, j;
        size_t skip, la, lc;

        while (i < W && seed[i] == cur[i])
            i++;
        if (i >= W)
            break;
        j = i;
        while (j < W && seed[j] != cur[j])
            j++;
        skip = i - last;
        if (W <= skip)
            break;
        la = emit_block(cur + i, j - i, skip, 1, tmpa);
        lc = emit_block(cur + i, j - i, skip, 0, tmpc);
        if (la <= lc) {
            memcpy(out + o, tmpa, la);
            o += la;
        } else {
            memcpy(out + o, tmpc, lc);
            o += lc;
        }
        last = j;
        pos = j;
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* Stream assembly                                                     */
/* ------------------------------------------------------------------ */

static void put_be16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/*
 * Transport framing.
 *
 * The stream the printer receives is a sequence of frames:
 *
 *   a5 <len:BE16> 00 01 03 <job id:4> 02 00 <data...> 07 08 09 0a 07 08 09 0a
 *
 * where len counts the 9-byte inner header, the data and the 8-byte trailer.
 * The raster stream (page command, band records, page flush) is simply the
 * concatenation of the data sections, so a band record may straddle a frame
 * boundary.  The vendor's own files use 21900-byte data sections, which makes
 * len 0x559d.
 *
 * Command frames (end of document) use 01 in place of 02 and carry no
 * trailer:  a5 00 11 00 01 03 <job id:4> 01 00 <cmd:4> <arg:4>
 */
#define FRAME_MAX 21900

static const uint8_t frame_trailer[8] = {
    0x07, 0x08, 0x09, 0x0a, 0x07, 0x08, 0x09, 0x0a
};

/*
 * Output sink.  A page is assembled into memory first so that it can be
 * repeated for multi-copy jobs: CUPS hands the copy count to the filter in
 * argv[4] and expects the driver to emit the page that many times (the PPD
 * declares *cupsManualCopies: True).  Without this, "lp -n 3" prints one sheet.
 */
static void out_write(lx900_job_t *job, const void *buf, size_t n)
{
    if (job->failed)
        return;

    if (job->capturing) {
        if (job->page_len + n > job->page_cap) {
            size_t cap = job->page_cap ? job->page_cap : 65536;
            unsigned char *nb;

            while (cap < job->page_len + n)
                cap *= 2;
            nb = realloc(job->page_buf, cap);
            if (!nb) {
                lx900_fail(job, "out of memory");
                return;
            }
            job->page_buf = nb;
            job->page_cap = cap;
        }
        memcpy(job->page_buf + job->page_len, buf, n);
        job->page_len += n;
        return;
    }

    if (job->write(job->ctx, buf, n) != (ssize_t)n)
        lx900_fail(job, "short write to the output");
}

static void frame_start(lx900_job_t *job)
{
    job->frame_len = 0;
}

static void frame_flush(lx900_job_t *job)
{
    uint8_t head[12];
    unsigned len;

    if (!job->frame_len)
        return;

    len = (unsigned)(9 + job->frame_len + sizeof(frame_trailer));
    head[0] = 0xa5;
    put_be16(head + 1, len);
    head[3] = 0x00; head[4] = 0x01; head[5] = 0x03;
    head[6] = 0x07; head[7] = 0x08; head[8] = 0x09; head[9] = 0x0a;
    head[10] = 0x02; head[11] = 0x00;

    out_write(job, head, sizeof(head));
    out_write(job, job->frame_buf, job->frame_len);
    out_write(job, frame_trailer, sizeof(frame_trailer));
    job->frame_len = 0;
}

/* Append raster bytes to the current frame, flushing between frames. */
static void frame_put(lx900_job_t *job, const void *buf, size_t n)
{
    const uint8_t *p = buf;

    while (n) {
        size_t room = FRAME_MAX - job->frame_len;
        size_t k = n < room ? n : room;

        memcpy(job->frame_buf + job->frame_len, p, k);
        job->frame_len += k;
        p += k;
        n -= k;
        if (job->frame_len == FRAME_MAX)
            frame_flush(job);
    }
}

/* Raster-stream markers that surround the page data. */
static const uint8_t end_marker[5] = {
    0x1b, 0x23, 0x01, 0x02, 0x00
};

/* "flush page" - the vendor sends this in a frame of its own. */
static const uint8_t flush_page[8] = {
    0x1b, 0x2a, 0x82, 0x00, 0x00, 0x00, 0x00, 0xac
};

/* End-of-document command frame ("end page"). */
static const uint8_t eod_0e[20] = {
    0xa5, 0x00, 0x11, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x01,
    0x01, 0x00, 0x1b, 0x2a, 0x05, 0x0e, 0x00, 0x00, 0x00, 0x01
};

static const uint8_t prologue_tail[13] = {
    0x1b, 0x2a, 0x6d, 0x05, 0x12, 0x02, 0x1b, 0x2a, 0x6d, 0x05, 0x00, 0x03, 0x03
};

/*
 * "Done printing" record.  The vendor filter writes this framed packet to its
 * output after HPEEndDoc; without it the printer keeps the page open, never
 * advances the media and ignores subsequent jobs.
 */
static const uint8_t done_printing[20] = {
    0xa5, 0x00, 0x11, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x01,
    0x01, 0x00, 0x1b, 0x2a, 0x05, 0x0b, 0x00, 0x00, 0x00, 0x01
};

/*
 * Page prologue: start-of-raster commands followed by the 32-byte media block
 * (size in 1/100 inch plus a byte sum checksum over block bytes 5..29).
 */
static void write_prologue(lx900_job_t *job, unsigned width_dots,
                           unsigned height_dots, unsigned xres, unsigned yres)
{
    uint8_t buf[53];
    uint8_t *b = buf + 8;
    unsigned sum = 0;
    unsigned i;

    buf[0] = 0x1b; buf[1] = 0x2a; buf[2] = 0x05; buf[3] = 0x0c;
    buf[4] = 0x1b; buf[5] = 0x2a; buf[6] = 0x05; buf[7] = 0x0d;

    b[0] = 0x1b; b[1] = 0x2a; b[2] = 0x6d; b[3] = 0x06;
    b[4] = 0x81; b[5] = 0x1a;
    b[6] = b[7] = b[8] = b[9] = 0xff;
    /* little endian 16-bit fields */
    b[10] = (uint8_t)((height_dots * 100 / (yres ? yres : 1)) & 0xff);
    b[11] = (uint8_t)(((height_dots * 100 / (yres ? yres : 1)) >> 8) & 0xff);
    b[12] = (uint8_t)((width_dots * 100 / (xres ? xres : 1)) & 0xff);
    b[13] = (uint8_t)(((width_dots * 100 / (xres ? xres : 1)) >> 8) & 0xff);
    b[14] = 0x00; b[15] = 0x80;
    b[16] = 0x00; b[17] = 0x80;
    b[18] = 0x00; b[19] = 0x80;
    b[20] = 0x00; b[21] = 0x80;
    b[22] = 0xff;
    memset(b + 23, 0, 8);
    for (i = 5; i < 30; i++)
        sum += b[i];
    b[31] = (uint8_t)sum;

    memcpy(buf + 40, prologue_tail, 13);
    frame_put(job, buf, sizeof(buf));
}

static void put_tlv(uint8_t **pp, unsigned tag, unsigned val)
{
    uint8_t *p = *pp;

    p[0] = (uint8_t)tag;
    p[1] = 0x02;
    put_be16(p + 2, val & 0xffff);
    p[4] = 0x00;
    *pp = p + 5;
}

static unsigned res_code(unsigned yres)
{
    switch (yres) {
    case 300:  return 0;
    case 600:  return 1;
    case 1200: return 2;
    case 2400: return 3;
    default:   return 1;
    }
}

static void write_page_info(lx900_job_t *job, unsigned width_dots,
                            unsigned height_dots, unsigned xres, unsigned yres,
                            unsigned tag01)
{
    uint8_t buf[165];
    uint8_t *p = buf;
    unsigned i;
    unsigned scale = yres / 600 ? yres / 600 : 1;

    (void)xres;
    int remainder;

    memset(buf, 0, sizeof(buf));
    memcpy(p, "\x1b\x23\x01\x00", 4);
    put_be32(p + 4, 0xa5);
    p[8] = 0x00; p[9] = 0x1c; p[10] = 0x00;
    p += 11;

    put_tlv(&p, 0x01, tag01);
    put_tlv(&p, 0x02, 0);
    put_tlv(&p, 0x03, 0);
    put_tlv(&p, 0x04, 0);
    put_tlv(&p, 0x05, 0);
    put_tlv(&p, 0x06, 2);
    put_tlv(&p, 0x07, 2);
    put_tlv(&p, 0x08, 1);
    put_tlv(&p, 0x09, 2550);
    put_tlv(&p, 0x0a, 0);
    put_tlv(&p, 0x0b, (height_dots * 300 / (yres ? yres : 1)) & 0xffff);
    for (i = 0x0c; i <= 0x11; i++)
        put_tlv(&p, i, 0);
    remainder = (int)(5100 * scale) - (int)width_dots;
    if (remainder < 0)
        remainder = 0;
    put_tlv(&p, 0x12, (unsigned)remainder);
    put_tlv(&p, 0x13, 0);
    put_tlv(&p, 0x14, height_dots & 0xffff);
    put_tlv(&p, 0x15, width_dots & 0xffff);
    put_tlv(&p, 0x16, height_dots & 0xffff);
    put_tlv(&p, 0x17, width_dots & 0xffff);
    put_tlv(&p, 0x18, 0);
    put_tlv(&p, 0x19, 0);
    put_tlv(&p, 0x1a, 1);
    put_tlv(&p, 0x1e, 3);
    put_tlv(&p, 0x22, 0);
    put_tlv(&p, 0x24, BAND_ROWS);
    put_tlv(&p, 0x26, 0);
    *p++ = 0x2c;
    *p++ = 0x02;
    *p++ = 0x00;
    *p++ = (uint8_t)res_code(yres);

    frame_put(job, buf, (size_t)(p - buf));
}

/* ------------------------------------------------------------------ */
/* Band writer                                                         */
/* ------------------------------------------------------------------ */

static int row_is_zero(const uint8_t *r, size_t n)
{
    while (n--)
        if (*r++)
            return 0;
    return 1;
}

/*
 * Compute the band extent shared by all planes: the lowest first-nonzero byte
 * and the highest last-nonzero byte+1 across every installed plane and row of
 * the band.  The vendor driver puts the same pair in every plane header of a
 * band; giving each plane its own extent (as an earlier version did) makes the
 * printer stop responding part-way through the job.
 */
static void band_extent(plane_t *pl, unsigned wbytes, unsigned rows,
                        unsigned *minb_out, unsigned *maxb_out)
{
    unsigned minb = wbytes;
    unsigned maxb = 0;
    unsigned p, r;

    for (p = 0; p < MAX_PLANES; p++) {
        if (!pl[p].data)
            continue;

        for (r = 0; r < rows; r++) {
            const uint8_t *row = pl[p].data + (size_t)r * pl[p].wbytes;
            size_t i;

            for (i = 0; i < wbytes; i++) {
                if (row[i]) {
                    if (i < minb)
                        minb = (unsigned)i;
                    break;
                }
            }
            for (i = wbytes; i > 0; i--) {
                if (row[i - 1]) {
                    if (i > maxb)
                        maxb = (unsigned)i;
                    break;
                }
            }
        }
    }
    if (minb == wbytes)
        minb = 0;
    *minb_out = minb;
    *maxb_out = maxb;
}

/*
 * Emit one band for one plane.  `seed` is the previous raster (wbytes long),
 * updated to the last raster of this band.  `band_min`/`band_max` are the
 * extent shared by all planes of this band (see band_extent).  Returns 0 on
 * success.
 */
static int write_band(lx900_job_t *job, plane_t *pl, const uint8_t *seed_in,
                      unsigned y, unsigned plane, uint8_t *seed_out,
                      int can_skip, unsigned band_min, unsigned band_max)
{
    const uint8_t *seed = seed_in;
    size_t wbytes = pl->wbytes;
    unsigned rows = pl->rows;
    unsigned r;
    int all_zero = 1;
    uint8_t hdr[24];
    size_t lengths[BAND_ROWS];
    size_t total;

    for (r = 0; r < rows; r++) {
        if (!row_is_zero(pl->data + (size_t)r * wbytes, wbytes)) {
            all_zero = 0;
            break;
        }
    }

    if (all_zero && can_skip) {
        /*
         * Empty-band record: no row data at all.  The vendor writes the
         * raster extent as 0/0 here, so mirror that exactly.
         */
        put_be32(hdr + 4, 24);
        hdr[0] = 0x1b; hdr[1] = 0x23; hdr[2] = 0x01; hdr[3] = 0x05;
        put_be32(hdr + 8, y);
        put_be16(hdr + 12, 0);
        put_be16(hdr + 14, rows);
        put_be16(hdr + 16, (unsigned)(wbytes * 8));
        put_be16(hdr + 18, 0);
        put_be16(hdr + 20, 0);
        hdr[22] = 2;
        hdr[23] = (uint8_t)plane;
        frame_put(job, hdr, 24);
        memset(seed_out, 0, wbytes);
        return 0;
    }

    total = 24 + 4 * rows;
    for (r = 0; r < rows; r++) {
        size_t n = enc_row(seed, pl->data + (size_t)r * wbytes, wbytes,
                           pl->enc + (size_t)r * pl->rowbuf,
                           pl->tmp + (size_t)r * pl->rowtmp);
        lengths[r] = n;
        total += 2 + n;
        seed = pl->data + (size_t)r * wbytes;
    }

    {
        unsigned minb = band_min, maxb = band_max;

        hdr[0] = 0x1b; hdr[1] = 0x23; hdr[2] = 0x01; hdr[3] = 0x05;
        put_be32(hdr + 4, (uint32_t)total);
        put_be32(hdr + 8, y);
        put_be16(hdr + 12, 0);
        put_be16(hdr + 14, rows);
        put_be16(hdr + 16, (unsigned)(wbytes * 8));
        /* raster extent in dots: first and one-past-last inked column */
        put_be16(hdr + 18, minb * 8);
        put_be16(hdr + 20, maxb * 8);
        hdr[22] = 2;
        hdr[23] = (uint8_t)plane;
    }

    frame_put(job, hdr, 24);
    {
        uint8_t cnt[4 * BAND_ROWS];
        unsigned i;
        for (i = 0; i < rows; i++)
            put_be32(cnt + 4 * i, pl->counts[i]);
        frame_put(job, cnt, (size_t)rows * 4);
    }
    for (r = 0; r < rows; r++) {
        uint8_t len2[2];
        put_be16(len2, (unsigned)lengths[r]);
        frame_put(job, len2, 2);
        if (lengths[r])
            frame_put(job, pl->enc + (size_t)r * pl->rowbuf, lengths[r]);
    }

    memcpy(seed_out, pl->data + (size_t)(rows - 1) * wbytes, wbytes);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Halftoning                                                          */
/* ------------------------------------------------------------------ */

static const uint8_t bayer8[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 }
};

static const uint8_t bitmask[8] = {
    0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01
};

/*
 * Per-plane threshold phase.
 *
 * All planes cover the same 1200 dpi grid.  If they share one threshold
 * matrix, equal ink demands put every ink on exactly the same dots: a 50%
 * grey then comes out as half the page bare and half the page carrying 300%
 * ink.  That speckles midtones and floods the stock, which is what made
 * photographs look wrong.
 *
 * Shifting each plane's threshold by a different amount of the 64-level ramp
 * makes the ink patterns interleave instead of stack, so partial coverage is
 * spread over the page.  Fully saturated colours are unaffected: at 100%
 * demand every plane is above every threshold anyway.
 */
static const uint8_t plane_phase[MAX_PLANES] = { 0, 21, 43, 32 };

static inline void set_dot(lx900_job_t *job, uint8_t *row, unsigned x,
                           unsigned ink, unsigned y, unsigned plane)
{
    unsigned level;
    unsigned t;
    int xd;

    if (!row)
        return;

    xd = (int)x + job->opt.left_offset;
    if (xd < 0 || (unsigned)xd >= job->page_width)
        return;

    level = (ink * 64u) / 255u;
    /* Index the dither matrix by where the dot lands, not where it started. */
    t = (bayer8[y & 7][(unsigned)xd & 7] + plane_phase[plane]) & 63u;

    if (level > t)
        row[(unsigned)xd >> 3] |= bitmask[(unsigned)xd & 7];
}

/* ------------------------------------------------------------------ */
/* Raster conversion                                                   */
/* ------------------------------------------------------------------ */


static int is_rgb8(const cups_page_header2_t *h)
{
    if (h->cupsBitsPerColor != 8)
        return 0;
    return h->cupsColorSpace == CUPS_CSPACE_RGB ||
           h->cupsColorSpace == CUPS_CSPACE_SRGB ||
           h->cupsColorSpace == CUPS_CSPACE_ADOBERGB;
}

static int is_gray8(const cups_page_header2_t *h)
{
    return h->cupsBitsPerColor == 8 &&
           (h->cupsColorSpace == CUPS_CSPACE_K ||
            h->cupsColorSpace == CUPS_CSPACE_W ||
            h->cupsColorSpace == CUPS_CSPACE_SW ||
            h->cupsColorSpace == CUPS_CSPACE_GOLD ||
            h->cupsColorSpace == 18);
}

/* Ink coverage (0 = paper, 255 = solid) of a colour, per ITU-R 601 luma. */
static inline unsigned luma_ink(unsigned r, unsigned g, unsigned b)
{
    return (299u * (255u - r) + 587u * (255u - g) + 114u * (255u - b) + 500u)
           / 1000u;
}

/*
 * Place one pixel's ink.  The demands are in ink units: c/m/y are the colour
 * cartridge channels, mono is the coverage used when only black is available.
 *
 *   Color       The CMY cartridge has to make black itself, so every drop of
 *               ink goes to cyan, magenta and yellow.
 *
 *   Black       Monochrome output through the black cartridge alone, driven by
 *               luma so that a saturated red still comes out as a mid grey
 *               rather than as nothing.
 *
 *   ColorBlack  Under-colour removal.  The black plane takes the largest ink
 *               demand common to all three colour channels and they lose that
 *               much, so a neutral is printed with black ink instead of a
 *               C+M+Y overprint - which is what makes text crisp and stops
 *               greys from drifting green when the inks are not balanced.
 *               Saturated colours are untouched, since one of their channels
 *               is already at zero.
 */
static inline void put_ink(lx900_job_t *job, unsigned char *pc,
                           unsigned char *pm, unsigned char *py,
                           unsigned char *pk, unsigned x, unsigned c,
                           unsigned m, unsigned y, unsigned mono,
                           unsigned absy)
{
    if (job->opt.ink == LX900_INK_BLACK) {
        set_dot(job, pk, x, mono, absy, PLANE_K);
        return;
    }

    if (job->opt.ink == LX900_INK_COLORBLACK) {
        unsigned k = c < m ? c : m;

        if (y < k)
            k = y;
        set_dot(job, pk, x, k, absy, PLANE_K);
        c -= k;
        m -= k;
        y -= k;
    }

    set_dot(job, pc, x, c, absy, PLANE_C);
    set_dot(job, pm, x, m, absy, PLANE_M);
    set_dot(job, py, x, y, absy, PLANE_Y);
}

/*
 * Convert one input raster line into one 1-bit row per active plane.  Planes
 * that are not installed have no buffer; set_dot() ignores them.
 */
static int convert_line(lx900_job_t *job, const cups_page_header2_t *h,
                        const uint8_t *src, plane_t *pl, unsigned bandrow,
                        unsigned absy)
{
    unsigned x;
    unsigned char *pd[MAX_PLANES];
    unsigned char *pc, *pm, *py, *pk;
    int i;

    for (i = 0; i < MAX_PLANES; i++) {
        pd[i] = pl[i].data ? pl[i].data + (size_t)bandrow * pl[i].wbytes
                           : NULL;
        if (pd[i])
            memset(pd[i], 0, pl[i].wbytes);
    }
    pc = pd[PLANE_C];
    pm = pd[PLANE_M];
    py = pd[PLANE_Y];
    pk = pd[PLANE_K];

    if (is_rgb8(h)) {
        unsigned ncomp = h->cupsNumColors ? h->cupsNumColors : 3;

        if (h->cupsColorOrder == CUPS_ORDER_CHUNKED) {
            for (x = 0; x < h->cupsWidth; x++) {
                const uint8_t *p = src + (size_t)x * ncomp;

                put_ink(job, pc, pm, py, pk, x, 255 - p[0], 255 - p[1],
                        255 - p[2], luma_ink(p[0], p[1], p[2]), absy);
            }
        } else {
            /* planar / banded: three component planes of cupsBytesPerLine */
            const uint8_t *pr = src;
            const uint8_t *pg = src + h->cupsBytesPerLine;
            const uint8_t *pb = src + 2 * (size_t)h->cupsBytesPerLine;

            for (x = 0; x < h->cupsWidth; x++) {
                put_ink(job, pc, pm, py, pk, x, 255 - pr[x], 255 - pg[x],
                        255 - pb[x], luma_ink(pr[x], pg[x], pb[x]), absy);
            }
        }
    } else if (is_gray8(h)) {
        for (x = 0; x < h->cupsWidth; x++) {
            unsigned ink = 255 - src[x];

            put_ink(job, pc, pm, py, pk, x, ink, ink, ink, ink, absy);
        }
    } else if (h->cupsBitsPerColor == 1 &&
               (h->cupsColorSpace == CUPS_CSPACE_K ||
                h->cupsColorSpace == CUPS_CSPACE_W)) {
        unsigned invert = (h->cupsColorSpace == CUPS_CSPACE_W);

        for (x = 0; x < h->cupsWidth; x++) {
            int on = (src[x >> 3] >> (7 - (x & 7))) & 1;
            unsigned ink;

            if (invert)
                on = !on;
            ink = on ? 255 : 0;
            put_ink(job, pc, pm, py, pk, x, ink, ink, ink, ink, absy);
        }
    } else if (h->cupsBitsPerColor == 8 && h->cupsColorSpace == CUPS_CSPACE_CMY) {
        for (x = 0; x < h->cupsWidth; x++) {
            const uint8_t *p = src + (size_t)x * 3;
            unsigned mono = (299u * p[0] + 587u * p[1] + 114u * p[2] + 500u)
                            / 1000u;

            put_ink(job, pc, pm, py, pk, x, p[0], p[1], p[2], mono, absy);
        }
    } else {
        lx900_fail(job, "unsupported raster format: colorspace=%d bpc=%d "
                        "order=%d", h->cupsColorSpace, h->cupsBitsPerColor,
                   h->cupsColorOrder);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

lx900_ink_t lx900_ink_from_name(const char *name)
{
    if (name == NULL)
        return LX900_INK_COLORBLACK;
    if (!strncmp(name, "Black", 5))
        return LX900_INK_BLACK;
    if (!strncmp(name, "ColorBlack", 10))
        return LX900_INK_COLORBLACK;
    if (!strncmp(name, "Color", 5))
        return LX900_INK_COLOR;
    return LX900_INK_COLORBLACK;
}

const char *lx900_ink_name(lx900_ink_t ink)
{
    switch (ink) {
    case LX900_INK_COLOR:
        return "Color";
    case LX900_INK_BLACK:
        return "Black";
    default:
        return "ColorBlack";
    }
}

lx900_job_t *lx900_job_create(const lx900_options_t *opts, lx900_write_fn write,
                              void *ctx)
{
    lx900_job_t *job;

    if (write == NULL)
        return NULL;

    job = calloc(1, sizeof(*job));
    if (job == NULL)
        return NULL;

    if (opts)
        job->opt = *opts;
    else
        job->opt.ink = LX900_INK_COLORBLACK;
    if (job->opt.copies == 0)
        job->opt.copies = 1;
    if (job->opt.copies > 999)
        job->opt.copies = 999;
    if (job->opt.top_offset > MAX_HEIGHT_DOTS)
        job->opt.top_offset = MAX_HEIGHT_DOTS;
    job->write = write;
    job->ctx = ctx;
    job->frame_buf = malloc(FRAME_MAX);
    if (job->frame_buf == NULL) {
        free(job);
        return NULL;
    }

    if (job->opt.ink == LX900_INK_COLOR) {
        job->plane_ids = ids_color;
        job->nactive = 3;
    } else {
        job->plane_ids = ids_both;
        job->nactive = 4;
    }
    return job;
}

void lx900_job_destroy(lx900_job_t *job)
{
    if (job == NULL)
        return;
    free(job->frame_buf);
    free(job->page_buf);
    free(job);
}

const char *lx900_job_error(const lx900_job_t *job)
{
    return job ? job->err : "no job";
}

static void free_page(lx900_job_t *job)
{
    unsigned i, p;

    for (i = 0; i < job->nactive; i++) {
        p = (unsigned)job->plane_ids[i];
        free(job->planes[p].data);
        free(job->planes[p].enc);
        free(job->planes[p].tmp);
        job->planes[p].data = NULL;
        job->planes[p].enc = NULL;
        job->planes[p].tmp = NULL;
        free(job->seed[p]);
        job->seed[p] = NULL;
    }
    job->page_open = 0;
}

/* Does convert_line() handle this format? */
static int format_supported(const cups_page_header2_t *h)
{
    if (is_rgb8(h))
        return 1;
    if (is_gray8(h))
        return 1;
    if (h->cupsBitsPerColor == 1 &&
        (h->cupsColorSpace == CUPS_CSPACE_K ||
         h->cupsColorSpace == CUPS_CSPACE_W))
        return 1;
    if (h->cupsBitsPerColor == 8 && h->cupsColorSpace == CUPS_CSPACE_CMY)
        return 1;
    return 0;
}

int lx900_page_begin(lx900_job_t *job, const cups_page_header2_t *hdr)
{
    unsigned i, p, wbytes;

    if (job == NULL || hdr == NULL)
        return -1;
    if (job->page_open) {
        lx900_fail(job, "a page is already open");
        return -1;
    }
    if (job->failed)
        return -1;

    job->page_width = hdr->cupsWidth;
    if (hdr->cupsWidth == 0 || hdr->cupsWidth > MAX_WIDTH_DOTS) {
        lx900_fail(job, "page is %u dots wide at %u dpi (%.2f in).  The print "
                        "head covers %u dots (%.2f in), so this page cannot be "
                        "printed.  Choose a PageSize no wider than 8.25 in, or "
                        "scale the document onto the label with 'lp -o "
                        "fit-to-page'.",
                   hdr->cupsWidth, hdr->HWResolution[0],
                   (double)hdr->cupsWidth /
                       (hdr->HWResolution[0] ? hdr->HWResolution[0] : 1),
                   MAX_WIDTH_DOTS, (double)MAX_WIDTH_DOTS / 1200.0);
        return -1;
    }
    if (hdr->HWResolution[0] != 1200 || hdr->HWResolution[1] != 1200) {
        lx900_fail(job, "raster is %ux%u dpi, but the LX900e driver only "
                        "supports 1200x1200 dpi; the queue must select "
                        "Resolution=1200dpi",
                   hdr->HWResolution[0], hdr->HWResolution[1]);
        return -1;
    }
    if (hdr->cupsHeight > MAX_HEIGHT_DOTS) {
        lx900_fail(job, "page is %u rows at %u dpi (%.1f in).  The printer "
                        "tops out at %d rows (18 in).",
                   hdr->cupsHeight, hdr->HWResolution[1],
                   (double)hdr->cupsHeight /
                       (hdr->HWResolution[1] ? hdr->HWResolution[1] : 1),
                   MAX_HEIGHT_DOTS);
        return -1;
    }
    if (!format_supported(hdr)) {
        lx900_fail(job, "unsupported raster format: colorspace=%d bpc=%d "
                        "order=%d", hdr->cupsColorSpace, hdr->cupsBitsPerColor,
                   hdr->cupsColorOrder);
        return -1;
    }

    if (hdr->cupsHeight == 0) {
        /* nothing to print; keep the page closed so the caller can skip it */
        job->page_skipped = 1;
        return 0;
    }
    job->page_skipped = 0;

    wbytes = (hdr->cupsWidth + 7) / 8;
    memset(job->planes, 0, sizeof(job->planes));
    for (i = 0; i < job->nactive; i++) {
        p = (unsigned)job->plane_ids[i];
        job->planes[p].wbytes = wbytes;
        job->planes[p].rowbuf = 2 * wbytes + 128;
        job->planes[p].rowtmp = 4 * wbytes + 256;
        job->planes[p].data = calloc((size_t)BAND_ROWS * wbytes, 1);
        job->planes[p].enc = malloc((size_t)BAND_ROWS * job->planes[p].rowbuf);
        job->planes[p].tmp = malloc((size_t)BAND_ROWS * job->planes[p].rowtmp);
        job->seed[p] = calloc(wbytes, 1);
        if (!job->planes[p].data || !job->planes[p].enc ||
            !job->planes[p].tmp || !job->seed[p]) {
            lx900_fail(job, "out of memory");
            free_page(job);
            return -1;
        }
    }

    job->usable = hdr->cupsHeight > job->opt.top_offset
                      ? hdr->cupsHeight - job->opt.top_offset
                      : hdr->cupsHeight;
    job->band_start = 0;
    job->band_rows = job->usable < BAND_ROWS ? job->usable : BAND_ROWS;

    job->page_len = 0;
    job->capturing = 1;
    frame_start(job);
    write_prologue(job, hdr->cupsWidth, hdr->cupsHeight, hdr->HWResolution[0],
                   hdr->HWResolution[1]);
    write_page_info(job, hdr->cupsWidth, hdr->cupsHeight, hdr->HWResolution[0],
                    hdr->HWResolution[1],
                    job->opt.tag01 ? job->opt.tag01
                                   : (job->opt.ink == LX900_INK_COLOR ? 0x0B
                                                                     : 0x0C));
    job->page_open = 1;
    return job->failed ? -1 : 0;
}

int lx900_page_row(lx900_job_t *job, const cups_page_header2_t *hdr,
                   const unsigned char *line, unsigned y)
{
    unsigned i, p, r, wbytes;
    int band_empty = 1;

    if (job == NULL || hdr == NULL)
        return -1;
    if (job->failed)
        return -1;
    if (!job->page_open || job->page_skipped)
        return 0;
    if (y >= job->usable)
        return 0;                       /* rows past the page are dropped */

    r = y - job->band_start;
    if (r >= job->band_rows) {
        lx900_fail(job, "raster row %u arrived out of order", y);
        return -1;
    }

    if (convert_line(job, hdr, line, job->planes, r, y) < 0)
        return -1;

    wbytes = job->planes[job->plane_ids[0]].wbytes;
    for (i = 0; i < job->nactive; i++) {
        unsigned bx;

        p = (unsigned)job->plane_ids[i];
        job->planes[p].counts[r] = 0;
        for (bx = 0; bx < wbytes; bx++) {
            uint8_t v = job->planes[p].data[(size_t)r * wbytes + bx];

            job->planes[p].counts[r] += (unsigned)__builtin_popcount(v);
        }
        job->planes[p].rows = job->band_rows;
    }

    for (i = 0; i < job->nactive && band_empty; i++) {
        unsigned k;

        p = (unsigned)job->plane_ids[i];
        for (k = 0; k < job->band_rows; k++)
            if (job->planes[p].counts[k]) {
                band_empty = 0;
                break;
            }
    }

    if (r + 1 < job->band_rows)
        return 0;

    /* the band is complete: write it */
    {
        unsigned band_min, band_max;

        band_extent(job->planes, wbytes, job->band_rows, &band_min, &band_max);
        for (i = 0; i < job->nactive; i++) {
            int blank_seed = 1;
            size_t k;

            p = (unsigned)job->plane_ids[i];
            for (k = 0; k < wbytes; k++)
                if (job->seed[p][k]) {
                    blank_seed = 0;
                    break;
                }
            if (write_band(job, &job->planes[p], job->seed[p],
                           job->band_start + job->opt.top_offset, p,
                           job->seed[p],
                           blank_seed && !(job->band_start == 0 && band_empty),
                           band_min, band_max) < 0)
                return -1;
        }
    }

    job->band_start += BAND_ROWS;
    job->band_rows = job->usable > job->band_start
                         ? (job->usable - job->band_start < BAND_ROWS
                                ? job->usable - job->band_start
                                : BAND_ROWS)
                         : 0;
    return job->failed ? -1 : 0;
}

int lx900_page_end(lx900_job_t *job)
{
    unsigned c;

    if (job == NULL)
        return -1;
    if (job->failed)
        return -1;
    if (!job->page_open) {
        job->page_skipped = 0;
        return 0;
    }

    if (job->band_start < job->usable) {
        lx900_fail(job, "page ended after %u of %u rows", job->band_start,
                   job->usable);
        free_page(job);
        return -1;
    }

    frame_put(job, end_marker, sizeof(end_marker));
    frame_flush(job);
    frame_put(job, flush_page, sizeof(flush_page));
    frame_flush(job);
    out_write(job, eod_0e, sizeof(eod_0e));
    out_write(job, done_printing, sizeof(done_printing));

    job->capturing = 0;
    for (c = 0; c < job->opt.copies && !job->failed; c++) {
        if (job->write(job->ctx, job->page_buf, job->page_len) !=
            (ssize_t)job->page_len)
            lx900_fail(job, "short write to the output");
    }

    free_page(job);
    job->page_len = 0;
    return job->failed ? -1 : 0;
}

int lx900_job_end(lx900_job_t *job)
{
    if (job == NULL)
        return -1;
    return job->failed ? -1 : 0;
}
