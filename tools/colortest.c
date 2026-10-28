/*
 * colortest - write a colour / nozzle diagnostic page as a PNG on stdout.
 *
 *   colortest [width] [height] > page.png
 *   colortest [width] [height] R G B > solid.png
 *   colortest bar [width] [height] [top] > bar.png
 *   colortest purge [width] [height] [c|m|y|k|all] > purge.png
 *   colortest align [width] [height] > align.png
 *
 * The page is laid out in device pixels at 1200 dpi, so with the usual
 * 2400x1200 arguments it is a 2.0 x 1.0 inch label.
 *
 * `bar` mode fills the page with paper white and puts one solid black bar
 * across the page: 3/4 of the width and 1/8 to 1/4 of the height.  It exists so
 * a bring-up test can ask "does this ink channel fire at all?" without dumping
 * a full page of ink onto a head that may be dry.
 *
 * `align` mode draws a registration gauge in the top right corner of the page:
 * a 20 mm line on the top edge and another 10 mm below it, and the same pair
 * down the right edge.
 * Print it through the driver, measure the gap from the physical label edge to
 * the frame, and shift the image until the gap is what you want.  The frames
 * are black; the reference has to be the edge of the media, because printed
 * marks all move together with the offset.
 *
 * `purge` mode is for unclogging: it fills the page with 100 % coverage of the
 * chosen ink or inks, one horizontal band per ink when several are selected.
 * Print it with InkCartridge=ColorBlack so each band lands on its own channel
 * (pure colour is left alone by the under-colour removal, and a black band
 * becomes the K plane).  It wastes a label per page - that is the cost of
 * pulling ink through a nozzle.
 *
 * With `top` the bar sits inside the first band of the page; without it the bar
 * is centred, which leaves the first band empty.  That difference matters: the
 * printer needs the first band of a page to carry ink, and a page that starts
 * with an empty band hangs the firmware.
 *
 * What each part tells you:
 *
 *   primary blocks    six full-strength patches: C, M, Y, R, G, B.  A hue that
 *                     is wrong here is a colour-transform problem; streaks or
 *                     white lines inside a patch are clogged nozzles.
 *   grey wedge        11 steps from paper white to solid black.  Even steps
 *                     mean the tone curve is sane; a coloured cast in the
 *                     neutral patches means the inks are not balanced.  With
 *                     InkCartridge Color + Black these steps are pure black
 *                     ink, so a dead colour channel does not tint them.
 *   stripe bands      one band per ink (C, M, Y, K), alternating single dots.
 *                     A blocked nozzle shows up as a pale line running the
 *                     width of the band.  This is the part to look at after
 *                     cleaning.
 *   flat 50% field    large area of even tone: banding, mottle or feed problems
 *                     show up here much better than in small patches.
 *
 * With an R G B triple instead of the diagnostic layout, the whole page is
 * filled with that one colour.  A solid magenta sheet is the usual purge /
 * prime target for a clogged magenta channel: it asks for maximum ink on
 * every nozzle, and it wastes nothing on the other two.
 *
 * The PNG is written with stored (uncompressed) deflate blocks, so the tool
 * needs no zlib, no libpng and no external image library.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* minimal PNG writer                                                  */
/* ------------------------------------------------------------------ */

static unsigned long adler32(const unsigned char *buf, size_t len)
{
    unsigned long a = 1, b = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        a = (a + buf[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static void put_be32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static unsigned long crc_table[256];
static int crc_ready;

static void crc_init(void)
{
    unsigned long c;
    int n, k;

    for (n = 0; n < 256; n++) {
        c = (unsigned long)n;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static unsigned long crc32_buf(const unsigned char *buf, size_t len)
{
    unsigned long c = 0xffffffffUL;
    size_t i;

    if (!crc_ready)
        crc_init();
    for (i = 0; i < len; i++)
        c = crc_table[(c ^ buf[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffUL;
}

static void emit_chunk(FILE *f, const char *type, const unsigned char *data,
                       size_t len)
{
    unsigned char b[4];
    unsigned char *tmp;
    unsigned long crc;

    put_be32(b, (unsigned long)len);
    fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (len)
        fwrite(data, 1, len, f);

    tmp = malloc(4 + len);
    if (!tmp) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    memcpy(tmp, type, 4);
    if (len)
        memcpy(tmp + 4, data, len);
    crc = crc32_buf(tmp, 4 + len);
    free(tmp);

    put_be32(b, crc);
    fwrite(b, 1, 4, f);
}

/* ------------------------------------------------------------------ */
/* page drawing                                                        */
/* ------------------------------------------------------------------ */

static unsigned W = 2400, H = 1200;
static unsigned char *img;              /* W * H * 3, RGB */

static void px(unsigned x, unsigned y, unsigned r, unsigned g, unsigned b)
{
    unsigned char *p;

    if (x >= W || y >= H)
        return;
    p = img + ((size_t)y * W + x) * 3;
    p[0] = (unsigned char)r;
    p[1] = (unsigned char)g;
    p[2] = (unsigned char)b;
}

static void rect(unsigned x0, unsigned y0, unsigned x1, unsigned y1,
                 unsigned r, unsigned g, unsigned b)
{
    unsigned x, y;

    if (x1 > W)
        x1 = W;
    if (y1 > H)
        y1 = H;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            px(x, y, r, g, b);
}

/* ------------------------------------------------------------------ */
/* tiny 5x7 font, for labels on the test pages                          */
/* ------------------------------------------------------------------ */

static const struct glyph {
    char c;
    unsigned char row[7];
} font5x7[] = {
    { 'A', { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e } },
    { 'C', { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e } },
    { 'D', { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e } },
    { 'E', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f } },
    { 'F', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f } },
    { 'H', { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e } },
    { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } },
    { 'M', { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
    { 'O', { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
    { 'P', { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d } },
    { 'R', { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e } },
    { 'T', { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1b, 0x11 } },
    { 'X', { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04 } },
    { 'Z', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f } },
    { '0', { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e } },
    { '1', { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e } },
    { '2', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f } },
    { '3', { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e } },
    { '4', { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 } },
    { '5', { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e } },
    { '6', { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e } },
    { '7', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e } },
    { '9', { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c } },
    { '-', { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c } },
    { ':', { 0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00 } },
    { '=', { 0x00, 0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00 } },
    { '/', { 0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10 } },
    { '<', { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 } },
    { '>', { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 } },
};

static void text(unsigned x, unsigned y, unsigned sc, const char *str,
                 unsigned char r, unsigned char g, unsigned char b, int rot180)
{
    unsigned n = 0, w = 0, h, i;
    const char *p;

    for (p = str; *p; p++)
        n++;
    if (!n)
        return;
    w = (n * 6 - 1) * sc;
    h = 7 * sc;

    /*
     * rot180 draws the string so that it reads correctly when the label is
     * turned through 180 degrees - which is how it is seen standing in front of
     * the printer, because the top of the page comes out first.
     */
    for (i = 0; i < n; i++) {
        /*
         * The position mapping below already reverses the reading order (slot
         * i lands at the mirrored slot), so take the characters in their
         * normal order.  Reversing them here as well cancels that out and
         * leaves the text left-to-right with each glyph rotated in place.
         */
        char c = str[i];
        size_t gi;

        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        for (gi = 0; gi < sizeof(font5x7) / sizeof(font5x7[0]); gi++) {
            unsigned row, col;

            if (font5x7[gi].c != c)
                continue;
            for (row = 0; row < 7; row++)
                for (col = 0; col < 5; col++) {
                    unsigned gx, gy;
                    unsigned dx0, dy0;

                    if (!(font5x7[gi].row[row] & (0x10u >> col)))
                        continue;
                    if (rot180) {
                        gx = i * 6 * sc + col * sc;
                        gy = row * sc;
                        dx0 = x + w - sc - gx;
                        dy0 = y + h - sc - gy;
                    } else {
                        gx = x + i * 6 * sc + col * sc;
                        gy = y + row * sc;
                        dx0 = gx;
                        dy0 = gy;
                    }
                    rect(dx0, dy0, dx0 + sc, dy0 + sc, r, g, b);
                }
            break;
        }
    }
}

/* Width of a string in dots at the given scale. */
static unsigned text_width(unsigned sc, const char *str)
{
    unsigned n = 0;

    while (*str++)
        n++;
    return n ? (n * 6 - 1) * sc : 0;
}

int main(int argc, char *argv[])
{
    unsigned char *raw, *zp;
    unsigned char ihdr[13];
    size_t rawlen, zplen;
    FILE *f = stdout;
    unsigned i;
    int arg = 1, bar = 0, bartop = 0, purge = 0, align = 0;

    if (argc > 1 && strcmp(argv[1], "bar") == 0) {
        bar = 1;
        arg = 2;
    }
    if (argc > 1 && strcmp(argv[1], "purge") == 0) {
        purge = 1;
        arg = 2;
    }
    if (argc > 1 && strcmp(argv[1], "align") == 0) {
        align = 1;
        arg = 2;
    }
    if (argc > arg)
        W = (unsigned)strtoul(argv[arg], NULL, 0);
    if (argc > arg + 1)
        H = (unsigned)strtoul(argv[arg + 1], NULL, 0);
    if (W < 64 || H < 64 || W > 20000 || H > 20000) {
        fprintf(stderr, "bad size %ux%u\n", W, H);
        return 2;
    }
    arg += 2;

    if (argc > arg && strcmp(argv[arg], "top") == 0)
        bartop = 1;

    img = malloc((size_t)W * H * 3);
    if (!img) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    memset(img, 255, (size_t)W * H * 3);    /* paper white */

    if (align) {
        /*
         * Registration gauge, specified in page coordinates:
         *
         *   - a 20 mm horizontal line on the top edge, starting at the top
         *     right corner and running inwards;
         *   - a 10 mm horizontal line 10 mm below the top edge, ending 10 mm in
         *     from the right end;
         *   - a 20 mm vertical line on the right edge, starting at the top
         *     (0 mm offset);
         *   - a 10 mm vertical line 10 mm in from the right edge, starting
         *     10 mm from the top.
         *
         * The two inner lines meet, so the pair forms an L at the 10 mm corner
         * and the outer pair an L at the page corner.
         *
         * The label is read rotated 180 degrees, so all of this appears at the
         * bottom left from in front of the printer: the right edge becomes the
         * left, the top edge becomes the bottom.
         */
        unsigned l20 = (unsigned)(20.0 * 1200.0 / 25.4 + 0.5);
        unsigned d10 = (unsigned)(10.0 * 1200.0 / 25.4 + 0.5);
        unsigned t = 8;

        if (W < d10 + l20 + 100 || H < l20 + 100) {
            fprintf(stderr, "page too small for the alignment gauge: "
                            "needs at least 35 x 22 mm\n");
            return 2;
        }

        /* top edge, from the top right corner inwards */
        rect(W - l20, 0, W, t, 0, 0, 0);
        /* 10 mm below the top edge, 10 mm long, ending 10 mm from the end */
        rect(W - d10 - d10, d10, W - d10, d10 + t, 0, 0, 0);
        /* right edge, from the top down */
        rect(W - t, 0, W, l20, 0, 0, 0);
        /* 10 mm in from the right edge, 10 mm long, starting 10 mm down */
        rect(W - d10 - t, d10, W - d10, d10 + d10, 0, 0, 0);

        /*
         * Instructions, tucked into the corner next to the gauge and drawn
         * rotated so they read from the front.  Keeping everything in the
         * corner means the whole target fits in a small page, so a check does
         * not eat a full label's worth of A4 or roll stock.
         */
        if (W >= (unsigned)(48.0 * 1200.0 / 25.4) &&
            H >= (unsigned)(27.0 * 1200.0 / 25.4)) {
            static const char *note[5] = {
                "OUTER LINE = 0MM",
                "GAP TO EDGE = OFFSET",
                "SET --LEFT / --TOP",
                "BY THAT GAP IN MM",
                "LX900-ALIGN.SH"
            };
            unsigned nlines = 5, longest = 20, i, sc, lh, y0;
            unsigned inset = (unsigned)(12.0 * 1200.0 / 25.4 + 0.5);
            unsigned wmax = (unsigned)(45.0 * 1200.0 / 25.4 + 0.5);

            /* never wider than the space left of the gauge */
            if (W > inset + 100 && wmax > W - inset - 100)
                wmax = W - inset - 100;

            sc = wmax / (longest * 6);
            if (sc > 14)
                sc = 14;              /* deliberately small: it is a note */
            if (sc < 4)
                sc = 4;

            lh = 10 * sc;
            /*
             * y0 is the unrotated reference position; the block then lands
             * with its top edge `inset` below the page's top edge, which puts
             * it just inside the gauge's inner corner.
             */
            y0 = H - inset - (nlines - 1) * lh - 7 * sc;
            for (i = 0; i < nlines; i++) {
                unsigned w = text_width(sc, note[i]);
                unsigned x0 = W > inset + w ? W - inset - w : 0;

                text(x0, H - y0 - i * lh - 7 * sc, sc, note[i], 0, 0, 0, 1);
            }
        }
    } else if (purge) {    } else if (purge) {
        /* one full-width band per selected ink, 100 % coverage */
        static const struct { char id; unsigned char rgb[3]; } inks[4] = {
            { 'c', {   0, 255, 255 } },
            { 'm', { 255,   0, 255 } },
            { 'y', { 255, 255,   0 } },
            { 'k', {   0,   0,   0 } }
        };
        const char *want = argc > arg ? argv[arg] : "all";
        int sel[4], nsel = 0, k;

        for (k = 0; k < 4; k++) {
            if (!strcmp(want, "all") || strchr(want, inks[k].id))
                sel[nsel++] = k;
        }
        if (!nsel)
            for (k = 0; k < 4; k++)
                sel[nsel++] = k;

        for (k = 0; k < nsel; k++) {
            unsigned y0 = H * (unsigned)k / (unsigned)nsel;
            unsigned y1 = H * (unsigned)(k + 1) / (unsigned)nsel;
            unsigned gap = H / 200 ? H / 200 : 1;

            if (k + 1 < nsel && y1 > y0 + gap)
                y1 -= gap;
            rect(0, y0, W, y1, inks[sel[k]].rgb[0], inks[sel[k]].rgb[1],
                 inks[sel[k]].rgb[2]);
        }
    } else if (bar) {
        /* one solid black bar, 3/4 of the width, 1/8 (top) or 1/4 (centre) tall */
        unsigned y0 = bartop ? H / 16 : H * 3 / 8;
        unsigned y1 = bartop ? y0 + H / 8 : H * 5 / 8;

        rect(W / 8, y0, W * 7 / 8, y1, 0, 0, 0);
    } else if (argc > arg + 2) {
        unsigned long r = strtoul(argv[arg], NULL, 0);
        unsigned long g = strtoul(argv[arg + 1], NULL, 0);
        unsigned long b = strtoul(argv[arg + 2], NULL, 0);
        size_t i;

        for (i = 0; i < (size_t)W * H; i++) {
            img[i * 3] = (unsigned char)r;
            img[i * 3 + 1] = (unsigned char)g;
            img[i * 3 + 2] = (unsigned char)b;
        }
    } else {
        /* six solid patches across the top */
        static const unsigned char patch[6][3] = {
            {   0, 255, 255 },   /* cyan    */
            { 255,   0, 255 },   /* magenta */
            { 255, 255,   0 },   /* yellow  */
            { 255,   0,   0 },   /* red     */
            {   0, 255,   0 },   /* green   */
            {   0,   0, 255 },   /* blue    */
        };
        unsigned pw = (W - 60 - 5 * 20) / 6;
        unsigned y0 = H / 40, y1 = H / 40 + H / 4;

        for (i = 0; i < 6; i++) {
            unsigned x0 = 30 + i * (pw + 20);

            rect(x0, y0, x0 + pw, y1, patch[i][0], patch[i][1], patch[i][2]);
        }

    {
        /* 11-step grey wedge */
        unsigned steps = 11;
        unsigned pw = (W - 60 - (steps - 1) * 12) / steps;
        unsigned y0 = H / 40 + H / 4 + H / 25;
        unsigned y1 = y0 + H / 5;

        for (i = 0; i < steps; i++) {
            unsigned v = 255 - (i * 255) / (steps - 1);
            unsigned x0 = 30 + i * (pw + 12);

            rect(x0, y0, x0 + pw, y1, v, v, v);
        }
    }

    {
        /*
         * One stripe band per ink: single pixel on, single pixel off.  The
         * last band is black and only prints when the black cartridge is
         * installed (InkCartridge Color or Color + Black).
         */
        static const unsigned char band[4][3] = {
            {   0, 255, 255 },   /* cyan    */
            { 255,   0, 255 },   /* magenta */
            { 255, 255,   0 },   /* yellow  */
            {   0,   0,   0 },   /* black   */
        };
        unsigned y = H / 40 + H / 4 + H / 25 + H / 5 + H / 40;
        unsigned bh = H / 30;
        unsigned k, x;

        for (k = 0; k < 4; k++) {
            for (x = 30; x < W - 30; x += 2)
                rect(x, y, x + 1, y + bh, band[k][0], band[k][1], band[k][2]);
            y += bh + H / 50;
        }
    }

    {
        /* flat 50% field with fine horizontal rules, for banding checks */
        unsigned y0 = H / 40 + H / 4 + H / 25 + H / 5 + H / 40 +
                      4 * (H / 30) + 4 * (H / 50) + H / 40;
        unsigned y;

        if (y0 + H / 8 < H) {
            rect(30, y0, W - 30, H - 10, 128, 128, 128);
            for (y = y0; y < H - 10; y += 8)
                rect(W / 2, y, W - 30, y + 1, 0, 0, 0);
        }
    }
    }

    /* PNG stream */
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);

    put_be32(ihdr, W);
    put_be32(ihdr + 4, H);
    ihdr[8] = 8;            /* bit depth */
    ihdr[9] = 2;            /* colour type: truecolour */
    ihdr[10] = 0;           /* deflate */
    ihdr[11] = 0;           /* filter method */
    ihdr[12] = 0;           /* no interlace */
    emit_chunk(f, "IHDR", ihdr, 13);

    /* each scanline is prefixed with filter type 0 */
    rawlen = (size_t)H * (1 + (size_t)W * 3);
    raw = malloc(rawlen);
    if (!raw) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    for (i = 0; i < H; i++) {
        raw[i * (1 + (size_t)W * 3)] = 0;
        memcpy(raw + i * (1 + (size_t)W * 3) + 1, img + (size_t)i * W * 3,
               (size_t)W * 3);
    }

    /* store the zlib stream in memory so it can go out as one IDAT chunk */
    {
        size_t blocks = (rawlen + 65534) / 65535;
        size_t cap = 2 + rawlen + blocks * 5 + 4;
        size_t off = 0, pos = 0;

        zp = malloc(cap);
        if (!zp) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
        zp[pos++] = 0x78;
        zp[pos++] = 0x01;
        while (off < rawlen) {
            size_t n = rawlen - off;

            if (n > 65535)
                n = 65535;
            zp[pos++] = (unsigned char)(off + n == rawlen ? 1 : 0);
            zp[pos++] = (unsigned char)(n & 0xff);
            zp[pos++] = (unsigned char)(n >> 8);
            zp[pos++] = (unsigned char)(~n & 0xff);
            zp[pos++] = (unsigned char)((~n >> 8) & 0xff);
            memcpy(zp + pos, raw + off, n);
            pos += n;
            off += n;
        }
        put_be32(zp + pos, adler32(raw, rawlen));
        pos += 4;
        zplen = pos;

        /* small colour type 2 IDATs are fine, but one chunk is simpler */
        emit_chunk(f, "IDAT", zp, zplen);
    }

    emit_chunk(f, "IEND", NULL, 0);

    free(zp);
    free(raw);
    free(img);
    return ferror(f) ? 1 : 0;
}
