/* mkraster - generate a CUPS RGB raster test page on stdout. */
#include <cups/raster.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    unsigned w = argc > 1 ? (unsigned)atoi(argv[1]) : 2400;
    unsigned h = argc > 2 ? (unsigned)atoi(argv[2]) : 1800;
    unsigned dpi = argc > 3 ? (unsigned)atoi(argv[3]) : 1200;
    int blank = argc > 4 && !strcmp(argv[4], "blank");
    unsigned x, y;
    cups_raster_t *r;
    cups_page_header2_t hdr;
    unsigned char *line = malloc((size_t)w * 3);

    if (!line)
        return 1;

    memset(&hdr, 0, sizeof(hdr));
    hdr.HWResolution[0] = dpi;
    hdr.HWResolution[1] = dpi;
    hdr.cupsWidth = w;
    hdr.cupsHeight = h;
    hdr.cupsBitsPerColor = 8;
    hdr.cupsBitsPerPixel = 24;
    hdr.cupsColorSpace = CUPS_CSPACE_RGB;
    hdr.cupsColorOrder = CUPS_ORDER_CHUNKED;
    hdr.cupsNumColors = 3;
    hdr.cupsBytesPerLine = w * 3;
    hdr.cupsInteger[0] = 0;
    hdr.cupsInteger[1] = 1;
    hdr.cupsInteger[2] = 0;
    hdr.cupsInteger[3] = 0;
    hdr.cupsInteger[4] = 0;

    r = cupsRasterOpen(1, CUPS_RASTER_WRITE);
    if (!r)
        return 1;
    if (!cupsRasterWriteHeader2(r, &hdr))
        return 1;

    for (y = 0; y < h; y++) {
        if (blank) {
            memset(line, 255, (size_t)w * 3);
            if (!cupsRasterWritePixels(r, line, w * 3))
                return 1;
            continue;
        }
        for (x = 0; x < w; x++) {
            unsigned char *p = line + (size_t)x * 3;
            unsigned band = (x * 8) / (w ? w : 1);
            unsigned v;

            if (y < h / 8) {                    /* colour bars */
                static const unsigned char bars[8][3] = {
                    {255,255,255},{255,255,0},{0,255,255},{0,255,0},
                    {255,0,255},{255,0,0},{0,0,255},{0,0,0}
                };
                p[0] = bars[band][0]; p[1] = bars[band][1]; p[2] = bars[band][2];
            } else if (y < h / 2) {             /* horizontal gradient */
                v = (unsigned)((x * 255) / (w ? w - 1 : 1));
                p[0] = (unsigned char)v;
                p[1] = (unsigned char)(255 - v);
                p[2] = (unsigned char)((v * 2) & 0xff);
            } else if (y < (h * 3) / 4) {       /* checkerboard */
                v = (((x / 37) + (y / 37)) & 1) ? 32 : 224;
                p[0] = p[1] = p[2] = (unsigned char)v;
            } else {                            /* vertical ramp bands */
                v = (unsigned)((y * 255) / (h ? h - 1 : 1));
                p[0] = (unsigned char)v;
                p[1] = (unsigned char)(255 - v);
                p[2] = 128;
            }
        }
        if (!cupsRasterWritePixels(r, line, w * 3))
            return 1;
    }
    cupsRasterClose(r);
    free(line);
    return 0;
}
