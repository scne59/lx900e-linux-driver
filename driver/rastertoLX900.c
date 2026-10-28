/*
 * rastertoLX900.c - CUPS raster filter for the Primera LX900e colour label
 * printer, implemented from scratch for Linux.
 *
 * Input : CUPS raster (application/vnd.cups-raster), ideally 8-bit RGB.
 * Output: the printer's native command/raster stream on stdout.
 *
 * Stream layout (per page):
 *   65-byte prologue  ("ESC * m" setup + media dimensions)
 *  165-byte page info ("ESC # 01 00", TLV body)
 *   n x 24/..-byte band records ("ESC # 01 05", one per plane per band)
 *   61-byte epilogue  ("ESC # 01 02" end page)
 *
 * Band record:
 *   [0..3]   1b 23 01 05
 *   [4..7]   BE32 total record length (header + dot counts + row data)
 *   [8..11]  BE32 first raster line of the band
 *   [12..13] BE16 x start in dots (always 0)
 *   [14..15] BE16 number of rasters in this band
 *   [16..17] BE16 band width in dots
 *   [18..19] min used byte << 3          (13-bit, split across two bytes)
 *   [20..21] BE16 max used dot
 *   [22]     compression flag (2 = CRDRE)
 *   [23]     plane index (0=C, 1=M, 2=Y)
 *   then one BE32 dot count per raster, then per raster: BE16 length + data.
 *   A completely blank band at the start of a plane is just the 24-byte header.
 *
 * Copyright (c) 2026.  Original work, no vendor code.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <cups/cups.h>
#include <cups/raster.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAND_ROWS       240

/*
 * The print head covers 8.25 inches, which is 9900 dots at 1200 dpi.  The
 * vendor's widest sample is 9896 dots, and the PPD offers pages up to 8x8
 * (9600 dots), so anything wider than the head cannot be printed and must be
 * refused here rather than sent and wedged.
 */
#define MAX_WIDTH_DOTS  9900

/*
 * The printer has one raster plane per cartridge channel.  Numbering is fixed
 * by the firmware: 0 = cyan, 1 = magenta, 2 = yellow, 3 = black.
 *
 * Every band must carry a contiguous run of records starting at plane 0.  A
 * band whose first record is plane 3 is accepted over USB and then the
 * firmware hangs - no feed, no head movement, no status reply until the mains
 * is cycled.  Measured on hardware: a blank page with records 0,1,2,3 feeds
 * normally, the same page cut down to plane 3 alone kills the printer, and the
 * vendor driver never emits a band that does not start at plane 0 either.
 *
 * So the *record fan-out* is always 0..N-1, while the *ink* that actually
 * lands on each plane is controlled separately:
 *
 *   Color       CMY cartridge        -> records 0,1,2   C/M/Y ink
 *   Black       black cartridge      -> records 0,1,2,3 K ink only
 *   ColorBlack  both cartridges      -> records 0,1,2,3 C/M/Y + K ink
 *
 * Black therefore emits three extra empty records that the older code omitted.
 * They cost 24 bytes per band and are what the firmware expects.
 */
#define PLANE_C         0
#define PLANE_M         1
#define PLANE_Y         2
#define PLANE_K         3
#define MAX_PLANES      4

#define INK_COLOR       0
#define INK_BLACK       1
#define INK_COLORBLACK  2

static int ink_mode = INK_COLORBLACK;

/*
 * Registration offsets, in dots at 1200 dpi.  `left` moves the whole image
 * horizontally and is applied in set_dot(); `page_width` is needed there to
 * clip.  The vendor software calls these HorzOffset and TOFOffset and converts
 * them to 1200 dpi the same way.
 */
static int left_offset;
static unsigned page_width;
static const int *plane_ids;
static unsigned nactive;

#ifndef CUPS_CSPACE_SRGB
#define CUPS_CSPACE_SRGB 19
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

/* ------------------------------------------------------------------ */
/* CRDRE row compressor                                                */
/* ------------------------------------------------------------------ */

/*
 * RLE body: one (repeat count - 1, value) byte pair per run, repeat
 * counts saturate at 0xff (i.e. 256 identical bytes per pair).
 */
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

static uint8_t frame_buf[FRAME_MAX];
static size_t frame_len;

/*
 * Output sink.  A page is assembled into memory first so that it can be
 * repeated for multi-copy jobs: CUPS hands the copy count to the filter in
 * argv[4] and expects the driver to emit the page that many times (the PPD
 * declares *cupsManualCopies: True).  Without this, "lp -n 3" prints one sheet.
 */
static unsigned char *page_buf;
static size_t page_len, page_cap;
static int page_capture;

static void out_write(const void *buf, size_t n)
{
    if (!page_capture) {
        fwrite(buf, 1, n, stdout);
        return;
    }

    if (page_len + n > page_cap) {
        size_t cap = page_cap ? page_cap : 65536;
        unsigned char *nb;

        while (cap < page_len + n)
            cap *= 2;
        nb = realloc(page_buf, cap);
        if (!nb)
            fatal("ERROR: out of memory");
        page_buf = nb;
        page_cap = cap;
    }
    memcpy(page_buf + page_len, buf, n);
    page_len += n;
}

static void frame_start(void)
{
    frame_len = 0;
}

static void frame_flush(void)
{
    uint8_t head[12];
    unsigned len;

    if (!frame_len)
        return;

    len = (unsigned)(9 + frame_len + sizeof(frame_trailer));
    head[0] = 0xa5;
    put_be16(head + 1, len);
    head[3] = 0x00; head[4] = 0x01; head[5] = 0x03;
    head[6] = 0x07; head[7] = 0x08; head[8] = 0x09; head[9] = 0x0a;
    head[10] = 0x02; head[11] = 0x00;

    out_write(head, sizeof(head));
    out_write(frame_buf, frame_len);
    out_write(frame_trailer, sizeof(frame_trailer));
    frame_len = 0;
}

/* Append raster bytes to the current frame, flushing between frames. */
static void frame_put(const void *buf, size_t n)
{
    const uint8_t *p = buf;

    while (n) {
        size_t room = FRAME_MAX - frame_len;
        size_t k = n < room ? n : room;

        memcpy(frame_buf + frame_len, p, k);
        frame_len += k;
        p += k;
        n -= k;
        if (frame_len == FRAME_MAX)
            frame_flush();
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
static void write_prologue(unsigned width_dots, unsigned height_dots,
                           unsigned xres, unsigned yres)
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
    frame_put(buf, sizeof(buf));
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

static void write_page_info(unsigned width_dots, unsigned height_dots,
                            unsigned xres, unsigned yres, unsigned tag01)
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

    frame_put(buf, (size_t)(p - buf));
}

/* ------------------------------------------------------------------ */
/* Band writer                                                         */
/* ------------------------------------------------------------------ */

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
static int write_band(plane_t *pl, const uint8_t *seed_in, unsigned y,
                      unsigned plane, uint8_t *seed_out, int can_skip,
                      unsigned band_min, unsigned band_max)
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
        frame_put(hdr, 24);
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

    frame_put(hdr, 24);
    {
        uint8_t cnt[4 * BAND_ROWS];
        unsigned i;
        for (i = 0; i < rows; i++)
            put_be32(cnt + 4 * i, pl->counts[i]);
        frame_put(cnt, (size_t)rows * 4);
    }
    for (r = 0; r < rows; r++) {
        uint8_t len2[2];
        put_be16(len2, (unsigned)lengths[r]);
        frame_put(len2, 2);
        if (lengths[r])
            frame_put(pl->enc + (size_t)r * pl->rowbuf, lengths[r]);
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

static inline void set_dot(uint8_t *row, unsigned x, unsigned ink,
                           unsigned y, unsigned plane)
{
    unsigned level;
    unsigned t;
    int xd;

    if (!row)
        return;

    xd = (int)x + left_offset;
    if (xd < 0 || (unsigned)xd >= page_width)
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
static inline void put_ink(unsigned char *pc, unsigned char *pm,
                           unsigned char *py, unsigned char *pk,
                           unsigned x, unsigned c, unsigned m, unsigned y,
                           unsigned mono, unsigned absy)
{
    if (ink_mode == INK_BLACK) {
        set_dot(pk, x, mono, absy, PLANE_K);
        return;
    }

    if (ink_mode == INK_COLORBLACK) {
        unsigned k = c < m ? c : m;

        if (y < k)
            k = y;
        set_dot(pk, x, k, absy, PLANE_K);
        c -= k;
        m -= k;
        y -= k;
    }

    set_dot(pc, x, c, absy, PLANE_C);
    set_dot(pm, x, m, absy, PLANE_M);
    set_dot(py, x, y, absy, PLANE_Y);
}

/*
 * Convert one input raster line into one 1-bit row per active plane.  Planes
 * that are not installed have no buffer; set_dot() ignores them.
 */
static void convert_line(const cups_page_header2_t *h, const uint8_t *src,
                         plane_t *pl, unsigned bandrow, unsigned absy)
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

                put_ink(pc, pm, py, pk, x, 255 - p[0], 255 - p[1],
                        255 - p[2], luma_ink(p[0], p[1], p[2]), absy);
            }
        } else {
            /* planar / banded: three component planes of cupsBytesPerLine */
            const uint8_t *pr = src;
            const uint8_t *pg = src + h->cupsBytesPerLine;
            const uint8_t *pb = src + 2 * (size_t)h->cupsBytesPerLine;

            for (x = 0; x < h->cupsWidth; x++) {
                put_ink(pc, pm, py, pk, x, 255 - pr[x], 255 - pg[x],
                        255 - pb[x], luma_ink(pr[x], pg[x], pb[x]), absy);
            }
        }
    } else if (is_gray8(h)) {
        for (x = 0; x < h->cupsWidth; x++) {
            unsigned ink = 255 - src[x];

            put_ink(pc, pm, py, pk, x, ink, ink, ink, ink, absy);
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
            put_ink(pc, pm, py, pk, x, ink, ink, ink, ink, absy);
        }
    } else if (h->cupsBitsPerColor == 8 && h->cupsColorSpace == CUPS_CSPACE_CMY) {
        for (x = 0; x < h->cupsWidth; x++) {
            const uint8_t *p = src + (size_t)x * 3;
            unsigned mono = (299u * p[0] + 587u * p[1] + 114u * p[2] + 500u)
                            / 1000u;

            put_ink(pc, pm, py, pk, x, p[0], p[1], p[2], mono, absy);
        }
    } else {
        fatal("ERROR: unsupported raster format: colorspace=%d bpc=%d order=%d",
              h->cupsColorSpace, h->cupsBitsPerColor, h->cupsColorOrder);
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

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

int main(int argc, char *argv[])
{
    cups_raster_t *ras;
    cups_page_header2_t hdr;
    unsigned tag01, top_offset;
    unsigned copies = 1;
    int page = 0;

    /*
     * CUPS filter interface: argv[1] job id, [2] user, [3] title,
     * [4] copies, [5] options, [6] input file.
     */
    if (argc > 4) {
        unsigned long v = strtoul(argv[4], NULL, 0);

        if (v > 0)
            copies = (unsigned)v;
        if (copies > 999)
            copies = 999;
    }

    debug_on = getenv("LX900_DEBUG") != NULL;
    tag01 = env_uint("LX900_TAG01", 0);   /* 0 = derive from the ink mode */

    {
        int top = env_int("LX900_TOP_OFFSET", conf_int("top", 4));

        left_offset = env_int("LX900_LEFT_OFFSET", conf_int("left", 0));
        top_offset = top > 0 ? (unsigned)top : 0;
        if (debug_on)
            logmsg("DEBUG: registration top=%u left=%d dots\n",
                   top_offset, left_offset);
    }

    /*
     * Which cartridges are fitted.  CUPS passes PPD options in argv[5] as
     * "Name=Value" pairs.  An unset option means the PPD default, Color + Black.
     */
    {
        static const int ids_color[] = { PLANE_C, PLANE_M, PLANE_Y };
        static const int ids_both[] = { PLANE_C, PLANE_M, PLANE_Y, PLANE_K };
        const char *s = NULL;
        int i;

        if (argc > 5) {
            static const char key[] = "InkCartridge=";
            const char *p = strstr(argv[5], key);

            if (p && (p == argv[5] || p[-1] == ' '))
                s = p + sizeof(key) - 1;
        }

        if (s && !strncmp(s, "Black", 5)) {
            /*
             * Ink: black only.  Records: all four, because the firmware hangs
             * on a band that starts at plane 3 (see the plane comment above).
             * put_ink() writes only to the K buffer in this mode, so the C/M/Y
             * records stay empty.
             */
            ink_mode = INK_BLACK;
            plane_ids = ids_both;
            nactive = 4;
        } else if (s && !strncmp(s, "ColorBlack", 10)) {
            ink_mode = INK_COLORBLACK;
            plane_ids = ids_both;
            nactive = 4;
        } else if (s && !strncmp(s, "Color", 5)) {
            ink_mode = INK_COLOR;
            plane_ids = ids_color;
            nactive = 3;
        } else {
            /* no option, or a value we do not know: the PPD default */
            ink_mode = INK_COLORBLACK;
            plane_ids = ids_both;
            nactive = 4;
        }

        /*
         * Page-info tag 0x01 is the media type code, and in the vendor engine
         * it is chosen from the cartridge configuration: their three-plane
         * capture (Colour cartridge only) uses 0x0b and their four-plane
         * capture (both cartridges) uses 0x0c.  It is the only field in the
         * page-info that is not derived from the page geometry, so it is the
         * one thing that differs between a page the printer accepts and a page
         * it refuses.  Match the vendor and never send a K-capable page with
         * the three-plane code.
         */
        if (!tag01)
            tag01 = ink_mode == INK_COLOR ? 0x0b : 0x0c;

        if (debug_on) {
            static const char *names[] = { "Color", "Black", "Color+Black" };
            static const char *planes[] = { "none", "C", "M", "Y", "K" };
            int used[MAX_PLANES] = { 0 };

            for (i = 0; i < (int)nactive; i++)
                used[plane_ids[i]] = 1;
            fprintf(stderr, "DEBUG: InkCartridge=%s -> planes", names[ink_mode]);
            for (i = 0; i < MAX_PLANES; i++)
                if (used[i])
                    fprintf(stderr, " %s", planes[i + 1]);
            fputc('\n', stderr);
        }
    }

    logmsg("DEBUG: rastertoLX900 starting\n");

    ras = cupsRasterOpen(0, CUPS_RASTER_READ);
    if (!ras)
        fatal("ERROR: cannot open CUPS raster stream");

    while (cupsRasterReadHeader2(ras, &hdr)) {
        plane_t pl[MAX_PLANES];
        uint8_t *seed[MAX_PLANES];
        unsigned wbytes, y, usable, skip;
        unsigned char *line;
        int p, i;

        page++;
        wbytes = (hdr.cupsWidth + 7) / 8;
        page_width = hdr.cupsWidth;
        if (hdr.cupsWidth == 0 || hdr.cupsWidth > MAX_WIDTH_DOTS)
            fatal("ERROR: page is %u dots wide at %u dpi (%.2f in). "
                  "The print head covers %u dots (%.2f in), so this page "
                  "cannot be printed.  Choose a PageSize no wider than "
                  "8.25 in, or scale the document onto the label with "
                  "'lp -o fit-to-page'.",
                  hdr.cupsWidth, hdr.HWResolution[0],
                  (double)hdr.cupsWidth / (hdr.HWResolution[0] ? hdr.HWResolution[0] : 1),
                  MAX_WIDTH_DOTS,
                  (double)MAX_WIDTH_DOTS / 1200.0);
        /*
         * The print head is a 1200 dpi device and this filter emits a single
         * 1200 dpi pass.  A 600/300 dpi raster would produce page-info values
         * and band widths the firmware does not expect; it resets the printer
         * rather than rejecting the job, so refuse to send it.
         */
        if (hdr.HWResolution[0] != 1200 || hdr.HWResolution[1] != 1200)
            fatal("ERROR: raster is %ux%u dpi, but the LX900e driver only "
                  "supports 1200x1200 dpi; the queue must select "
                  "Resolution=1200dpi",
                  hdr.HWResolution[0], hdr.HWResolution[1]);
        if (hdr.cupsHeight == 0)
            continue;

        if (debug_on)
            fprintf(stderr,
                    "DEBUG: page %d: %ux%u res=%ux%u cs=%d bpc=%d order=%d "
                    "bpl=%u\n",
                    page, hdr.cupsWidth, hdr.cupsHeight,
                    hdr.HWResolution[0], hdr.HWResolution[1],
                    hdr.cupsColorSpace, hdr.cupsBitsPerColor,
                    hdr.cupsColorOrder, hdr.cupsBytesPerLine);

        line = malloc(hdr.cupsBytesPerLine ? hdr.cupsBytesPerLine : 1);
        if (!line)
            fatal("ERROR: out of memory");

        memset(pl, 0, sizeof(pl));
        for (i = 0; i < (int)nactive; i++) {
            p = plane_ids[i];
            pl[p].wbytes = wbytes;
            pl[p].rowbuf = 2 * wbytes + 128;
            pl[p].rowtmp = 4 * wbytes + 256;
            pl[p].data = calloc((size_t)BAND_ROWS * wbytes, 1);
            pl[p].enc = malloc((size_t)BAND_ROWS * pl[p].rowbuf);
            pl[p].tmp = malloc((size_t)BAND_ROWS * pl[p].rowtmp);
            seed[p] = calloc(wbytes, 1);
            if (!pl[p].data || !pl[p].enc || !pl[p].tmp || !seed[p])
                fatal("ERROR: out of memory");
        }

        /*
         * Buffer this page so it can be repeated for multi-copy jobs.  A page
         * ends with the "done printing" record, so replaying the whole block
         * is exactly equivalent to printing the same page again.
         */
        page_len = 0;
        page_capture = 1;
        frame_start();
        write_prologue(hdr.cupsWidth, hdr.cupsHeight,
                       hdr.HWResolution[0], hdr.HWResolution[1]);
        write_page_info(hdr.cupsWidth, hdr.cupsHeight,
                        hdr.HWResolution[0], hdr.HWResolution[1], tag01);

        /*
         * The printer's raster origin is `top_offset` rows into the page: a
         * band whose yStart is 0 makes the firmware stop responding.  The
         * vendor stream does exactly this - first band at y=top_offset, last
         * band truncated so that the bands end on the page height.  The
         * trailing top_offset source rows therefore fall off the page, which
         * is what the vendor driver does too.
         */
        usable = hdr.cupsHeight > top_offset ? hdr.cupsHeight - top_offset
                                             : hdr.cupsHeight;

        for (y = 0; y < usable; y += BAND_ROWS) {
            unsigned n = usable - y;
            unsigned r;

            if (n > BAND_ROWS)
                n = BAND_ROWS;

            for (r = 0; r < n; r++) {
                if (cupsRasterReadPixels(ras, line, hdr.cupsBytesPerLine) == 0)
                    fatal("ERROR: short raster read at line %u", y + r);
                for (i = 0; i < (int)nactive; i++)
                    pl[plane_ids[i]].rows = n;
                convert_line(&hdr, line, pl, r, y + r);
                for (i = 0; i < (int)nactive; i++) {
                    unsigned bx;

                    p = plane_ids[i];
                    pl[p].counts[r] = 0;
                    for (bx = 0; bx < wbytes; bx++) {
                        uint8_t v = pl[p].data[(size_t)r * wbytes + bx];

                        pl[p].counts[r] += (unsigned)__builtin_popcount(v);
                    }
                }
            }

            {
                unsigned band_min, band_max;
                int band_empty = 1;

                /*
                 * Is there any ink anywhere in this band?  counts[] is already
                 * the per-row popcount of every plane, so this is free.
                 */
                for (i = 0; i < (int)nactive && band_empty; i++) {
                    p = plane_ids[i];
                    for (r = 0; r < n; r++)
                        if (pl[p].counts[r]) {
                            band_empty = 0;
                            break;
                        }
                }

                band_extent(pl, wbytes, n, &band_min, &band_max);
                for (i = 0; i < (int)nactive; i++) {
                    int blank_seed = 1;
                    size_t k;

                    p = plane_ids[i];
                    for (k = 0; k < wbytes; k++)
                        if (seed[p][k]) {
                            blank_seed = 0;
                            break;
                        }

                    /*
                     * If the first band of the page carries no ink at all,
                     * write its records as real records with zero-length rows
                     * instead of skip records.  A page that starts with nothing
                     * but skip records is accepted over USB and then the
                     * printer never starts the page: no feed, no head movement
                     * and no status reply until the mains is cycled.  Every
                     * page that has printed had ink in its first band, and the
                     * vendor stream never contains a band with no data at all,
                     * so an empty first band is a path the firmware is
                     * probably never exercised on.
                     *
                     * Empty *planes* inside a band that does have ink stay
                     * skip records, which is what the vendor stream does.
                     */
                    write_band(&pl[p], seed[p], y + top_offset, (unsigned)p,
                               seed[p],
                               blank_seed && !(y == 0 && band_empty),
                               band_min, band_max);
                }
            }
        }

        /* consume the source rows that fall past the end of the page */
        for (skip = usable; skip < hdr.cupsHeight; skip++) {
            if (cupsRasterReadPixels(ras, line, hdr.cupsBytesPerLine) == 0)
                fatal("ERROR: short raster read at line %u", skip);
        }

        frame_put(end_marker, sizeof(end_marker));
        frame_flush();
        frame_put(flush_page, sizeof(flush_page));
        frame_flush();
        out_write(eod_0e, sizeof(eod_0e));
        out_write(done_printing, sizeof(done_printing));

        page_capture = 0;
        {
            unsigned c;

            for (c = 0; c < copies; c++)
                fwrite(page_buf, 1, page_len, stdout);
        }
        if (debug_on && copies > 1)
            fprintf(stderr, "DEBUG: page %d emitted %u times\n", page, copies);
        fflush(stdout);

        for (i = 0; i < (int)nactive; i++) {
            p = plane_ids[i];
            free(pl[p].data);
            free(pl[p].enc);
            free(pl[p].tmp);
            free(seed[p]);
        }
        free(line);
    }

    cupsRasterClose(ras);
    return 0;
}
