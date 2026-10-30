/*
 * SPDX-License-Identifier: MIT
 *
 * Encoder for the Primera LX900e: CUPS raster in, printer stream out.
 *
 * This is the same code the CUPS filter uses; the printer application calls it
 * through the callbacks below instead of reading a raster from stdin.
 */
#ifndef LX900_H
#define LX900_H

#include <cups/raster.h>

#include <stddef.h>
#include <sys/types.h>

/* Plane numbers are fixed by the firmware: 0=C, 1=M, 2=Y, 3=K. */
#define LX900_PLANE_C 0
#define LX900_PLANE_M 1
#define LX900_PLANE_Y 2
#define LX900_PLANE_K 3
#define LX900_MAX_PLANES 4

/* Which ink is used.  The record fan-out always starts at plane 0. */
typedef enum {
    LX900_INK_COLOR = 0,        /* CMY cartridge: planes 0,1,2 */
    LX900_INK_BLACK = 1,        /* black only: planes 0..3, ink on K */
    LX900_INK_COLORBLACK = 2    /* both: planes 0..3, under-colour removal */
} lx900_ink_t;

typedef struct {
    lx900_ink_t ink;
    int         left_offset;    /* registration, dots at 1200 dpi, signed */
    unsigned    top_offset;     /* registration, dots at 1200 dpi */
    unsigned    copies;         /* 1..999; the page is emitted this many times */
    unsigned    tag01;          /* page-info media code, 0 = from the ink mode */
    int         debug;
} lx900_options_t;

/*
 * The sink.  Returns the number of bytes written, or -1 on error; a short
 * write is treated as an error.
 */
typedef ssize_t (*lx900_write_fn)(void *ctx, const void *buf, size_t len);

typedef struct lx900_job lx900_job_t;

/* Create a job.  `opts` is copied; `copies` is clamped to 1..999. */
lx900_job_t *lx900_job_create(const lx900_options_t *opts, lx900_write_fn write,
                              void *ctx);
void lx900_job_destroy(lx900_job_t *job);

/* Last error message, or "" if none.  Never NULL. */
const char *lx900_job_error(const lx900_job_t *job);

/*
 * One page.  page_begin validates the header and emits the prologue and page
 * info; page_row() is called once per raster line in ascending order (rows past
 * the printable area are ignored); page_end() flushes the last band, writes the
 * end-of-page records and emits the page `copies` times.
 *
 * All three return 0 on success and -1 on failure, with the reason available
 * from lx900_job_error().
 */
int lx900_page_begin(lx900_job_t *job, const cups_page_header2_t *hdr);
int lx900_page_row(lx900_job_t *job, const cups_page_header2_t *hdr,
                   const unsigned char *line, unsigned y);
int lx900_page_end(lx900_job_t *job);

/* Nothing to flush today; kept so callers have a symmetric API. */
int lx900_job_end(lx900_job_t *job);

/* Ink mode from a name ("Color", "Black", "ColorBlack"); default is ColorBlack. */
lx900_ink_t lx900_ink_from_name(const char *name);
const char *lx900_ink_name(lx900_ink_t ink);

#endif /* LX900_H */
