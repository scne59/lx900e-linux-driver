/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jochen Schneider <j@lz44.de>
 *
 * Native PAPPL printer application for the Primera LX900e colour label
 * printer.
 *
 * This exposes the printer to CUPS 3 / IPP Everywhere clients through PAPPL.
 * PWG raster arrives through the raster callbacks; PDF and PostScript are
 * converted to CUPS raster with Ghostscript and then fed to the same encoder
 * (src/lx900_encoder.c) the CUPS 2 filter uses.
 *
 * The application deliberately knows nothing about the wire format: if a page
 * prints from the CUPS 2 queue it prints from here, because it is the same
 * encoder underneath.
 */

#include <pappl/pappl.h>

#include <cups/cups.h>
#include <cups/pwg.h>
#include <cups/raster.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "lx900.h"
#include "lx900_status.h"

#ifndef LX900_CONF
#define LX900_CONF "/etc/lx900.conf"
#endif

#define DRIVER_NAME "lx900e"

/* ------------------------------------------------------------------ */
/* Media                                                               */
/* ------------------------------------------------------------------ */

/*
 * The same page sizes as tools/gen-ppd.py, in hundredths of a millimetre as
 * PAPPL wants them.  Keeping the table here (rather than reading the PPD)
 * means the printer application works without the CUPS 2 files installed.
 */
typedef struct {
    const char *name;
    int         width;
    int         length;
} media_size_t;

static const media_size_t media_sizes[] = {
    { "4x3",      10160,  7620 },   /* default, first for that reason */
    { "2x1",       5080,  2540 },
    { "2x2",       5080,  5080 },
    { "3x2",       7620,  5080 },
    { "3x2.5",     7620,  6350 },
    { "3x4",       7620, 10160 },
    { "4x2",      10160,  5080 },
    { "4x4",      10160, 10160 },
    { "4x6",      10160, 15240 },
    { "5x4",      12700, 10160 },
    { "5x5",      12700, 12700 },
    { "6x2",      15240,  5080 },
    { "6x4",      15240, 10160 },
    { "8x4",      20320, 10160 },
    { "8x6",      20320, 15240 },
    { "8x8",      20320, 20320 },
    { "100x76mm", 10000,  7600 },
};

#define NUM_MEDIA (int)(sizeof(media_sizes) / sizeof(media_sizes[0]))

/*
 * Mobile print dialogs, AirPrint in particular, build their paper-size menu
 * from media-ready and expect one size per media source.  There is one
 * physical roll, so the common sizes are exposed as virtual rolls.  PAPPL
 * allows at most PAPPL_MAX_SOURCE (16) sources; the last size is therefore
 * advertised but not pre-loaded.
 */
static const int ready_media_indices[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

#define NUM_READY_MEDIA (int)(sizeof(ready_media_indices) / sizeof(ready_media_indices[0]))

_Static_assert(sizeof(ready_media_indices) / sizeof(ready_media_indices[0]) <=
                   PAPPL_MAX_SOURCE,
               "ready_media_indices has more entries than PAPPL_MAX_SOURCE");

static char media_names[NUM_MEDIA][64];
static char source_names[NUM_MEDIA][32];

static void init_media_names(void)
{
    int i;

    for (i = 0; i < NUM_MEDIA; i++) {
        pwg_media_t *pwg = pwgMediaForSize(media_sizes[i].width,
                                           media_sizes[i].length);

        if (pwg != NULL)
            papplCopyString(media_names[i], pwg->pwg, sizeof(media_names[i]));
        else
            snprintf(media_names[i], sizeof(media_names[i]), "custom_%s",
                     media_sizes[i].name);

        if (i == 0)
            papplCopyString(source_names[i], "main-roll",
                            sizeof(source_names[i]));
        else
            snprintf(source_names[i], sizeof(source_names[i]),
                     "roll-%s", media_sizes[i].name);
    }
}

static void fill_media(const media_size_t *size, pappl_media_col_t *media,
                       const char *source)
{
    memset(media, 0, sizeof(*media));
    papplCopyString(media->size_name, media_names[size - media_sizes],
                    sizeof(media->size_name));
    papplCopyString(media->source, source, sizeof(media->source));
    papplCopyString(media->type, "labels", sizeof(media->type));
    media->size_width = size->width;
    media->size_length = size->length;
    media->left_margin = 0;
    media->right_margin = 0;
    media->top_margin = 0;
    media->bottom_margin = 0;
    media->tracking = PAPPL_MEDIA_TRACKING_MARK;
}

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

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

static const char *vendor_value(pappl_pr_options_t *options, const char *name)
{
    if (options == NULL || options->num_vendor <= 0)
        return NULL;
    return cupsGetOption(name, options->num_vendor, options->vendor);
}

static void options_from_pappl(lx900_options_t *opt,
                               pappl_pr_options_t *options)
{
    const char *ink;
    int top;

    memset(opt, 0, sizeof(*opt));
    opt->copies = options->copies > 0 ? (unsigned)options->copies : 1;

    ink = vendor_value(options, "lx900-ink-cartridge");
    opt->ink = ink ? lx900_ink_from_name(ink) : LX900_INK_COLORBLACK;

    /* Registration is a property of the installed printer, so it comes from
     * the same file the CUPS 2 filter reads. */
    top = env_int("LX900_TOP_OFFSET", conf_int("top", 4));
    opt->left_offset = env_int("LX900_LEFT_OFFSET", conf_int("left", 0));
    opt->top_offset = top > 0 ? (unsigned)top : 0;
    opt->tag01 = 0;                     /* derive from the ink mode */
    opt->debug = getenv("LX900_DEBUG") != NULL;
}

/* ------------------------------------------------------------------ */
/* Raster callbacks                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    lx900_job_t     *job;
    lx900_options_t  opt;
    unsigned         pages;
} lx900_raster_job_t;

static ssize_t device_write_cb(void *context, const void *data, size_t length)
{
    return papplDeviceWrite((pappl_device_t *)context, data, length);
}

static void device_error_cb(const char *message, void *data)
{
    (void)data;
    fprintf(stderr, "lx900e-app: device error: %s\n", message);
}

static bool raster_start_job(pappl_job_t *job, pappl_pr_options_t *options,
                             pappl_device_t *device)
{
    lx900_raster_job_t *rj;

    rj = calloc(1, sizeof(*rj));
    if (rj == NULL)
        return false;

    options_from_pappl(&rj->opt, options);
    rj->job = lx900_job_create(&rj->opt, device_write_cb, device);
    if (rj->job == NULL) {
        free(rj);
        return false;
    }

    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
                "raster job: format=%s, device=%s, copies=%d, ink=%s",
                papplJobGetFormat(job) ? papplJobGetFormat(job) : "(unknown)",
                papplPrinterGetDeviceURI(papplJobGetPrinter(job)) ?
                    papplPrinterGetDeviceURI(papplJobGetPrinter(job)) :
                    "(none)",
                options->copies, lx900_ink_name(rj->opt.ink));

    papplJobSetData(job, rj);
    return true;
}

static bool raster_start_page(pappl_job_t *job, pappl_pr_options_t *options,
                              pappl_device_t *device, unsigned page)
{
    lx900_raster_job_t *rj = papplJobGetData(job);

    (void)device;
    (void)page;

    if (rj == NULL)
        return false;
    if (lx900_page_begin(rj->job, &options->header) < 0) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "%s",
                    lx900_job_error(rj->job));
        return false;
    }
    return true;
}

static bool raster_write_line(pappl_job_t *job, pappl_pr_options_t *options,
                              pappl_device_t *device, unsigned y,
                              const unsigned char *line)
{
    lx900_raster_job_t *rj = papplJobGetData(job);

    (void)device;

    if (rj == NULL)
        return false;
    if (lx900_page_row(rj->job, &options->header, line, y) < 0) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "%s",
                    lx900_job_error(rj->job));
        return false;
    }
    return true;
}

static bool raster_end_page(pappl_job_t *job, pappl_pr_options_t *options,
                            pappl_device_t *device, unsigned page)
{
    lx900_raster_job_t *rj = papplJobGetData(job);

    (void)options;
    (void)device;
    (void)page;

    if (rj == NULL)
        return false;
    if (lx900_page_end(rj->job) < 0) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "%s",
                    lx900_job_error(rj->job));
        return false;
    }
    rj->pages++;
    return true;
}

static bool raster_end_job(pappl_job_t *job, pappl_pr_options_t *options,
                           pappl_device_t *device)
{
    lx900_raster_job_t *rj = papplJobGetData(job);
    bool result = true;

    (void)options;
    (void)device;

    if (rj == NULL)
        return false;
    if (lx900_job_end(rj->job) < 0) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "%s",
                    lx900_job_error(rj->job));
        result = false;
    }
    /* PAPPL counts impressions and copies for raster jobs itself. */
    lx900_job_destroy(rj->job);
    free(rj);
    papplJobSetData(job, NULL);
    return result;
}

/* ------------------------------------------------------------------ */
/* PDF / PostScript                                                    */
/* ------------------------------------------------------------------ */

static int copy_file(const char *source, const char *destination)
{
    int input = open(source, O_RDONLY);
    int output;
    unsigned char buffer[65536];
    ssize_t bytes;

    if (input < 0)
        return -1;

    output = open(destination, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (output < 0) {
        close(input);
        return -1;
    }

    while ((bytes = read(input, buffer, sizeof(buffer))) > 0) {
        ssize_t offset = 0;

        while (offset < bytes) {
            ssize_t written = write(output, buffer + offset,
                                    (size_t)(bytes - offset));

            if (written < 0) {
                if (errno == EINTR)
                    continue;
                close(input);
                close(output);
                return -1;
            }
            offset += written;
        }
    }

    close(input);
    close(output);
    return bytes < 0 ? -1 : 0;
}

static int run_ghostscript(pappl_job_t *job, pappl_pr_options_t *options,
                           const char *input, const char *output)
{
    char resolution[32];
    char width[48];
    char height[48];
    char output_arg[512];
    char *argv[24];
    int argc = 0;
    int dpi = options->printer_resolution[0] > 0 ?
              options->printer_resolution[0] : 1200;
    double width_pt = options->media.size_width * 72.0 / 2540.0;
    double height_pt = options->media.size_length * 72.0 / 2540.0;
    pid_t pid;
    int status;
    time_t deadline;

    if (width_pt <= 0.0)
        width_pt = 288.0;                   /* 4 in */
    if (height_pt <= 0.0)
        height_pt = 216.0;                  /* 3 in */

    snprintf(resolution, sizeof(resolution), "-r%dx%d", dpi, dpi);
    snprintf(width, sizeof(width), "-dDEVICEWIDTHPOINTS=%d",
             (int)lround(width_pt));
    snprintf(height, sizeof(height), "-dDEVICEHEIGHTPOINTS=%d",
             (int)lround(height_pt));
    snprintf(output_arg, sizeof(output_arg), "-sOutputFile=%s", output);

    argv[argc++] = "gs";
    argv[argc++] = "-dQUIET";
    argv[argc++] = "-dSAFER";
    argv[argc++] = "-dNOPAUSE";
    argv[argc++] = "-dBATCH";
    argv[argc++] = "-dNOINTERPOLATE";
    argv[argc++] = "-dFIXEDMEDIA";
    argv[argc++] = "-dPDFFitPage";
    argv[argc++] = "-sDEVICE=cups";
    argv[argc++] = resolution;
    argv[argc++] = width;
    argv[argc++] = height;
    argv[argc++] = "-dcupsBitsPerColor=8";
    argv[argc++] = "-dcupsColorOrder=0";
    argv[argc++] = "-dcupsColorSpace=1";     /* CUPS_CSPACE_RGB */
    argv[argc++] = output_arg;
    argv[argc++] = (char *)input;
    argv[argc] = NULL;

    papplLogJob(job, PAPPL_LOGLEVEL_INFO,
                "running Ghostscript: %dx%d dpi, %.1fx%.1f mm, output '%s'",
                dpi, dpi,
                (double)options->media.size_width / 100.0,
                (double)options->media.size_length / 100.0, output);

    pid = fork();
    if (pid < 0) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                    "cannot fork Ghostscript: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        int nullfd = open("/dev/null", O_WRONLY);

        if (nullfd >= 0) {
            dup2(nullfd, STDOUT_FILENO);
            close(nullfd);
        }
        execvp("gs", argv);
        _exit(127);
    }

    deadline = time(NULL) + 120;
    for (;;) {
        pid_t waited = waitpid(pid, &status, WNOHANG);

        if (waited == pid)
            break;
        if (waited < 0) {
            if (errno == EINTR)
                continue;
            papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                        "cannot wait for Ghostscript: %s", strerror(errno));
            return -1;
        }
        if (time(NULL) >= deadline) {
            papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                        "Ghostscript timed out after 120 seconds");
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                ;
            return -1;
        }
        sleep(1);
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                    "Ghostscript failed for '%s' (status %d)", input, status);
        return -1;
    }
    return 0;
}

/* Feed a CUPS raster file to the encoder.  Used by the PDF path and by any
 * job that arrives as CUPS raster instead of PWG raster. */
static bool encode_raster(pappl_job_t *job, lx900_job_t *lj,
                          const char *filename, unsigned *pages)
{
    cups_raster_t *ras;
    cups_page_header2_t hdr;
    unsigned char *line = NULL;
    unsigned bpl = 0;
    int fd;
    bool result = true;

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                    "cannot open raster file '%s': %s", filename,
                    strerror(errno));
        return false;
    }
    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (ras == NULL) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                    "cannot read raster file '%s'", filename);
        close(fd);
        return false;
    }

    while (cupsRasterReadHeader2(ras, &hdr)) {
        unsigned y;

        if (lx900_page_begin(lj, &hdr) < 0) {
            papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "%s", lx900_job_error(lj));
            result = false;
            break;
        }
        if (hdr.cupsHeight == 0)
            continue;

        if (bpl != hdr.cupsBytesPerLine) {
            unsigned char *nb = realloc(line, hdr.cupsBytesPerLine);

            if (nb == NULL) {
                papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "out of memory");
                result = false;
                break;
            }
            line = nb;
            bpl = hdr.cupsBytesPerLine;
        }
        memset(line, 0, bpl);

        for (y = 0; y < hdr.cupsHeight; y++) {
            if (cupsRasterReadPixels(ras, line, bpl) < bpl) {
                papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                            "short raster read at line %u", y);
                result = false;
                break;
            }
            if (lx900_page_row(lj, &hdr, line, y) < 0) {
                papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "%s",
                            lx900_job_error(lj));
                result = false;
                break;
            }
        }
        if (!result)
            break;

        if (lx900_page_end(lj) < 0) {
            papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "%s", lx900_job_error(lj));
            result = false;
            break;
        }
        (*pages)++;
    }

    free(line);
    cupsRasterClose(ras);
    close(fd);
    return result && lx900_job_end(lj) == 0;
}

static bool pdf_filter(pappl_job_t *job, pappl_device_t *device, void *data)
{
    pappl_pr_options_t *options = NULL;
    lx900_options_t opt;
    lx900_job_t *lj = NULL;
    char tempname[] = "/tmp/lx900e-app-XXXXXX";
    char inputname[] = "/tmp/lx900e-input-XXXXXX";
    int fd = -1;
    int input_fd = -1;
    unsigned pages = 0;
    bool result = false;

    (void)data;

    options = papplJobCreatePrintOptions(job, 0, false);
    if (options == NULL) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "cannot create print options");
        return false;
    }

    fd = mkstemp(tempname);
    if (fd < 0) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                    "cannot create temporary raster file: %s",
                    strerror(errno));
        goto out;
    }
    close(fd);
    fd = -1;

    input_fd = mkstemp(inputname);
    if (input_fd < 0) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                    "cannot create temporary input file: %s",
                    strerror(errno));
        goto out;
    }
    close(input_fd);
    input_fd = -1;

    if (copy_file(papplJobGetFilename(job), inputname) < 0) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                    "cannot copy '%s' to a temporary file: %s",
                    papplJobGetFilename(job), strerror(errno));
        goto out;
    }

    if (run_ghostscript(job, options, inputname, tempname) < 0)
        goto out;

    options_from_pappl(&opt, options);
    lj = lx900_job_create(&opt, device_write_cb, device);
    if (lj == NULL) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "cannot create the encoder");
        goto out;
    }

    if (!encode_raster(job, lj, tempname, &pages))
        goto out;

    if (pages > 0) {
        papplJobSetImpressions(job, pages);
        papplJobSetImpressionsCompleted(job, pages);
    }
    result = true;

out:
    if (lj != NULL)
        lx900_job_destroy(lj);
    if (fd >= 0)
        close(fd);
    if (input_fd >= 0)
        close(input_fd);
    unlink(tempname);
    unlink(inputname);
    if (options != NULL)
        papplJobDeletePrintOptions(options);
    return result;
}

/* ------------------------------------------------------------------ */
/* Status and supplies                                                 */
/* ------------------------------------------------------------------ */

static bool lx900_status(pappl_printer_t *printer)
{
    const char *uri = papplPrinterGetDeviceURI(printer);
    lx900_status_t st;
    pappl_supply_t supplies[4];
    static const pappl_supply_color_t colors[4] = {
        PAPPL_SUPPLY_COLOR_CYAN, PAPPL_SUPPLY_COLOR_MAGENTA,
        PAPPL_SUPPLY_COLOR_YELLOW, PAPPL_SUPPLY_COLOR_BLACK
    };
    static const char * const names[4] = {
        "Cyan ink", "Magenta ink", "Yellow ink", "Black ink"
    };
    pappl_preason_t add = PAPPL_PREASON_NONE;
    pappl_preason_t remove = PAPPL_PREASON_MARKER_SUPPLY_EMPTY |
                             PAPPL_PREASON_MARKER_SUPPLY_LOW |
                             PAPPL_PREASON_MEDIA_EMPTY;
    static time_t last_query;
    time_t now = time(NULL);
    int i;

    /*
     * Only a USB printer can be asked directly, and only while it is idle:
     * the query opens the device and would collide with a job that is being
     * written.  Other transports (file:, socket:) simply keep the last
     * supplies that were reported.
     */
    if (uri == NULL || strncmp(uri, "usb:", 4) != 0)
        return true;
    if (papplPrinterGetState(printer) != IPP_PSTATE_IDLE)
        return true;
    /* The printer is slow to answer and does not like being asked often. */
    if (now - last_query < 10)
        return true;
    last_query = now;
    if (lx900_status_read(&st) < 0)
        return true;

    memset(supplies, 0, sizeof(supplies));
    for (i = 0; i < 4; i++) {
        supplies[i].color = colors[i];
        supplies[i].is_consumed = true;
        supplies[i].type = PAPPL_SUPPLY_TYPE_INK_CARTRIDGE;
        if (st.level[i] == -2) {
            snprintf(supplies[i].description,
                     sizeof(supplies[i].description), "%s (not fitted)",
                     names[i]);
            supplies[i].level = -1;
        } else {
            papplCopyString(supplies[i].description, names[i],
                            sizeof(supplies[i].description));
            supplies[i].level = st.level[i];
            if (st.level[i] == 0)
                add |= PAPPL_PREASON_MARKER_SUPPLY_EMPTY;
            else if (st.level[i] > 0 && st.level[i] <= 10)
                add |= PAPPL_PREASON_MARKER_SUPPLY_LOW;
        }
    }

    if (!st.media_present || st.out_of_stock)
        add |= PAPPL_PREASON_MEDIA_EMPTY;

    papplPrinterSetSupplies(printer, 4, supplies);
    papplPrinterSetReasons(printer, add, remove);
    return true;
}

/* ------------------------------------------------------------------ */
/* Driver registration                                                 */
/* ------------------------------------------------------------------ */

static void add_vendor_attrs(ipp_t *attrs)
{
    static const char * const inks[] = { "Color", "Black", "ColorBlack" };

    ippAddString(attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD,
                 "lx900-ink-cartridge-default", NULL, "ColorBlack");
    ippAddStrings(attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD),
                  "lx900-ink-cartridge-supported", 3, NULL, inks);
}

static bool driver_cb(pappl_system_t *system, const char *driver_name,
                      const char *device_uri, const char *device_id,
                      pappl_pr_driver_data_t *driver_data, ipp_t **driver_attrs,
                      void *data)
{
    int i;

    (void)device_uri;
    (void)device_id;
    (void)data;

    if (driver_name == NULL || driver_data == NULL || driver_attrs == NULL)
        return false;
    if (strcmp(driver_name, DRIVER_NAME) != 0) {
        papplLog(system, PAPPL_LOGLEVEL_ERROR, "Unknown driver '%s'",
                 driver_name);
        return false;
    }

    driver_data->printfile_cb = NULL;
    driver_data->rstartjob_cb = raster_start_job;
    driver_data->rstartpage_cb = raster_start_page;
    driver_data->rwriteline_cb = raster_write_line;
    driver_data->rendpage_cb = raster_end_page;
    driver_data->rendjob_cb = raster_end_job;
    driver_data->status_cb = lx900_status;

    papplCopyString(driver_data->make_and_model,
                    "Primera LX900e colour label printer",
                    sizeof(driver_data->make_and_model));
    driver_data->format = "image/pwg-raster";
    driver_data->kind = PAPPL_KIND_LABEL | PAPPL_KIND_ROLL;
    driver_data->ppm = 1;
    driver_data->ppm_color = 1;

    driver_data->color_supported = PAPPL_COLOR_MODE_COLOR |
                                   PAPPL_COLOR_MODE_MONOCHROME;
    driver_data->color_default = PAPPL_COLOR_MODE_COLOR;
    driver_data->raster_types = PAPPL_PWG_RASTER_TYPE_SRGB_8 |
                                PAPPL_PWG_RASTER_TYPE_SGRAY_8 |
                                PAPPL_PWG_RASTER_TYPE_BLACK_1;
    driver_data->force_raster_type = PAPPL_PWG_RASTER_TYPE_SRGB_8;
    driver_data->orient_default = IPP_ORIENT_NONE;
    driver_data->quality_default = IPP_QUALITY_NORMAL;
    driver_data->scaling_default = PAPPL_SCALING_FIT;
    driver_data->sides_supported = PAPPL_SIDES_ONE_SIDED;
    driver_data->sides_default = PAPPL_SIDES_ONE_SIDED;
    driver_data->input_face_up = true;
    driver_data->output_face_up = true;

    /* The head is a 1200 dpi device; there is nothing to choose. */
    driver_data->num_resolution = 1;
    driver_data->x_resolution[0] = 1200;
    driver_data->y_resolution[0] = 1200;
    driver_data->x_default = 1200;
    driver_data->y_default = 1200;

    driver_data->num_type = 2;
    driver_data->type[0] = "labels";
    driver_data->type[1] = "continuous";
    driver_data->num_bin = 0;

    driver_data->num_media = NUM_MEDIA;
    for (i = 0; i < NUM_MEDIA; i++)
        driver_data->media[i] = media_names[i];

    driver_data->num_source = 0;
    for (i = 0; i < NUM_READY_MEDIA; i++) {
        int index = ready_media_indices[i];
        int n = driver_data->num_source;

        if (index < 0 || index >= NUM_MEDIA)
            continue;
        fill_media(&media_sizes[index], &driver_data->media_ready[n],
                   source_names[index]);
        driver_data->source[n] = source_names[index];
        driver_data->num_source++;
    }
    driver_data->media_default = driver_data->media_ready[0];
    driver_data->left_right = 0;
    driver_data->bottom_top = 0;
    driver_data->tracking_supported = PAPPL_MEDIA_TRACKING_MARK |
                                      PAPPL_MEDIA_TRACKING_CONTINUOUS;

    driver_data->mode_supported = PAPPL_LABEL_MODE_TEAR_OFF;
    driver_data->mode_configured = PAPPL_LABEL_MODE_TEAR_OFF;

    driver_data->has_supplies = true;
    driver_data->num_vendor = 1;
    driver_data->vendor[0] = "lx900-ink-cartridge";

    *driver_attrs = ippNew();
    add_vendor_attrs(*driver_attrs);
    return true;
}

static const char *autoadd_cb(const char *device_info, const char *device_uri,
                              const char *device_id, void *data)
{
    (void)device_info;
    (void)device_uri;
    (void)data;

    if (device_id == NULL)
        return NULL;
    if (strstr(device_id, "Color_Label_900") != NULL ||
        strstr(device_id, "LX900") != NULL ||
        strstr(device_id, "Primera") != NULL)
        return DRIVER_NAME;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static int mkdir_p(const char *path, mode_t mode)
{
    char *copy;
    char *p;
    struct stat st;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode))
            return 0;
        errno = ENOTDIR;
        return -1;
    }

    copy = strdup(path);
    if (copy == NULL)
        return -1;

    for (p = copy + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(copy, mode) < 0 && errno != EEXIST) {
            int saved = errno;

            free(copy);
            errno = saved;
            return -1;
        }
        *p = '/';
    }

    if (mkdir(copy, mode) < 0 && errno != EEXIST) {
        int saved = errno;

        free(copy);
        errno = saved;
        return -1;
    }

    free(copy);
    return 0;
}

static char *default_spool_dir(void)
{
    const char *xdg_state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    const char *tmpdir = getenv("TMPDIR");
    char *path = malloc(1024);

    if (path == NULL)
        return NULL;

    if (geteuid() == 0) {
        snprintf(path, 1024, "/var/spool/lx900e-app");
    } else if (xdg_state != NULL && xdg_state[0] != '\0') {
        snprintf(path, 1024, "%s/lx900e-app", xdg_state);
    } else if (home != NULL && home[0] != '\0') {
        snprintf(path, 1024, "%s/.local/state/lx900e-app", home);
    } else {
        snprintf(path, 1024, "%s/lx900e-app-%lu",
                 tmpdir != NULL && tmpdir[0] != '\0' ? tmpdir : "/tmp",
                 (unsigned long)geteuid());
    }
    return path;
}

typedef struct {
    int port;
    int count;
} printer_list_t;

static void print_printer_cb(pappl_printer_t *printer, void *data)
{
    printer_list_t *list = data;
    char uriname[256];

    papplPrinterGetPath(printer, NULL, uriname, sizeof(uriname));
    fprintf(stderr, "  printer: %s  ipp://localhost:%d/ipp/print/%s\n",
            papplPrinterGetName(printer), list->port,
            uriname[0] == '/' ? uriname + 1 : uriname);
    list->count++;
}

static const char *find_style_file(void)
{
    const char *override = getenv("LX900_APP_CSS");
    static const char * const candidates[] = {
        "pappl/lx900.css",
        "/usr/share/lx900e-app/style.css",
        "/usr/local/share/lx900e-app/style.css",
        NULL
    };
    int i;

    if (override != NULL && override[0] != '\0' && access(override, R_OK) == 0)
        return override;

    for (i = 0; candidates[i] != NULL; i++)
        if (access(candidates[i], R_OK) == 0)
            return candidates[i];
    return NULL;
}

static void usage(FILE *stream)
{
    fprintf(stream,
            "lx900e-app %s\n"
            "PAPPL/IPP Everywhere printer application for the Primera LX900e.\n\n"
            "Usage: lx900e-app [options]\n"
            "  --device URI       Create one printer for this device URI (for example\n"
            "                     usb://Primera/Color_Label_900?serial=00000001)\n"
            "  --printer NAME     Printer name for --device (default: LX900e)\n"
            "  --port PORT        IPP port (default: 8000)\n"
            "  --spool DIR        Spool directory\n"
            "  --log FILE         Log file (default: stderr)\n"
            "  --state FILE       State file (default: SPOOL/state)\n"
            "  --no-state         Do not load or save printer state\n"
            "  --debug            Enable PAPPL debug logging\n"
            "  --check-device     Open the --device URI, report success, and exit\n"
            "  --no-tls           Do not advertise or require IPPS/TLS\n"
            "  --help             Show this help\n\n"
            "Without --device, the application discovers USB printers and also\n"
            "allows adding printers through its web interface.\n",
            LX900_APP_VERSION);
}

int main(int argc, char **argv)
{
    pappl_system_t *system;
    pappl_pr_driver_t driver;
    const char *device_uri = NULL;
    const char *printer_name = NULL;
    const char *spool_dir = NULL;
    char *spool_alloc = NULL;
    const char *log_file = "-";
    const char *state_file = NULL;
    char state_buffer[1024];
    bool state_loaded;
    bool no_state = false;
    bool check_device = false;
    bool no_tls = false;
    pappl_loglevel_t loglevel = PAPPL_LOGLEVEL_INFO;
    int port = 8000;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device_uri = argv[++i];
        } else if (strcmp(argv[i], "--printer") == 0 && i + 1 < argc) {
            printer_name = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
            if (port < 1 || port > 65535)
                port = 8000;
        } else if (strcmp(argv[i], "--spool") == 0 && i + 1 < argc) {
            spool_dir = argv[++i];
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_file = argv[++i];
        } else if (strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
            state_file = argv[++i];
        } else if (strcmp(argv[i], "--no-state") == 0) {
            no_state = true;
        } else if (strcmp(argv[i], "--debug") == 0) {
            loglevel = PAPPL_LOGLEVEL_DEBUG;
        } else if (strcmp(argv[i], "--check-device") == 0) {
            check_device = true;
        } else if (strcmp(argv[i], "--no-tls") == 0) {
            no_tls = true;
        } else {
            fprintf(stderr, "lx900e-app: unknown option '%s'\n", argv[i]);
            usage(stderr);
            return 1;
        }
    }

    if (spool_dir == NULL) {
        spool_alloc = default_spool_dir();
        if (spool_alloc == NULL) {
            fprintf(stderr, "lx900e-app: out of memory\n");
            return 1;
        }
        spool_dir = spool_alloc;
    }

    init_media_names();

    if (mkdir_p(spool_dir, 0700) < 0) {
        fprintf(stderr,
                "lx900e-app: cannot create spool directory '%s': %s\n"
                "Use --spool with a writable directory, for example:\n"
                "  lx900e-app --spool \"$HOME/.local/state/lx900e-app\" ...\n",
                spool_dir, strerror(errno));
        free(spool_alloc);
        return 1;
    }

    if (check_device) {
        pappl_device_t *device;

        if (device_uri == NULL) {
            fprintf(stderr, "lx900e-app: --check-device requires --device\n");
            free(spool_alloc);
            return 1;
        }

        device = papplDeviceOpen(device_uri,
                                 printer_name != NULL ? printer_name : "LX900e",
                                 device_error_cb, NULL);
        if (device == NULL) {
            fprintf(stderr, "lx900e-app: cannot open device '%s'\n",
                    device_uri);
            free(spool_alloc);
            return 1;
        }
        papplDeviceClose(device);
        fprintf(stderr, "lx900e-app: device '%s' opened successfully\n",
                device_uri);
        free(spool_alloc);
        return 0;
    }

    memset(&driver, 0, sizeof(driver));
    driver.name = DRIVER_NAME;
    driver.description = "Primera LX900e";
    driver.device_id = NULL;
    driver.extension = NULL;

    {
        pappl_soptions_t system_options = PAPPL_SOPTIONS_MULTI_QUEUE |
                                          PAPPL_SOPTIONS_WEB_INTERFACE |
                                          PAPPL_SOPTIONS_RAW_SOCKET |
                                          PAPPL_SOPTIONS_DNSSD_HOST;

        if (no_tls)
            system_options |= PAPPL_SOPTIONS_NO_TLS;

        system = papplSystemCreate(system_options, "Primera LX900e", port,
                                   "_universal", spool_dir, log_file, loglevel,
                                   NULL, false);
    }
    if (system == NULL) {
        fprintf(stderr,
                "lx900e-app: cannot create PAPPL system (spool '%s'): %s\n",
                spool_dir, strerror(errno));
        free(spool_alloc);
        return 1;
    }

    {
        const char *style = find_style_file();

        if (style != NULL)
            papplSystemAddResourceFile(system, "/style.css", "text/css", style);
        else
            papplLog(system, PAPPL_LOGLEVEL_WARN,
                     "Cannot find lx900.css; using the PAPPL default stylesheet");
    }

    papplSystemSetFooterHTML(system,
        "lx900e-cups &middot; <a href=\"https://github.com/scne59/lx900e-linux-driver\">project page</a>");

    papplSystemAddLink(system, "Settings", "/config", PAPPL_LOPTIONS_NAVIGATION);
    papplSystemAddLink(system, "Add Printer", "/addprinter",
                       PAPPL_LOPTIONS_NAVIGATION);
    papplSystemAddLink(system, "Printers", "/", PAPPL_LOPTIONS_NAVIGATION);

    if (!papplSystemAddListeners(system, NULL)) {
        fprintf(stderr, "lx900e-app: cannot add listeners\n");
        papplSystemDelete(system);
        free(spool_alloc);
        return 1;
    }

    papplSystemSetPrinterDrivers(system, 1, &driver, autoadd_cb, NULL,
                                 driver_cb, NULL);

    papplSystemAddMIMEFilter(system, "application/pdf", "image/pwg-raster",
                             pdf_filter, NULL);
    papplSystemAddMIMEFilter(system, "application/postscript",
                             "image/pwg-raster", pdf_filter, NULL);
    papplSystemAddMIMEFilter(system, "application/vnd.cups-postscript",
                             "image/pwg-raster", pdf_filter, NULL);

    if (state_file == NULL) {
        snprintf(state_buffer, sizeof(state_buffer), "%s/state", spool_dir);
        state_file = state_buffer;
    }
    if (!no_state) {
        papplSystemSetSaveCallback(system,
                                   (pappl_save_cb_t)papplSystemSaveState,
                                   (void *)state_file);
        state_loaded = papplSystemLoadState(system, state_file);
    } else {
        state_loaded = false;
    }

    if (device_uri != NULL) {
        char default_name[64];
        char resource[256];
        pappl_printer_t *existing;

        if (printer_name == NULL) {
            snprintf(default_name, sizeof(default_name), "LX900e");
            printer_name = default_name;
        }

        snprintf(resource, sizeof(resource), "/ipp/print/%s", printer_name);
        existing = papplSystemFindPrinter(system, resource, 0, NULL);
        if (existing != NULL &&
            papplPrinterGetDeviceURI(existing) != NULL &&
            strcmp(papplPrinterGetDeviceURI(existing), device_uri) != 0) {
            papplLog(system, PAPPL_LOGLEVEL_WARN,
                     "Replacing existing printer '%s' (device '%s') with "
                     "device '%s'.",
                     printer_name, papplPrinterGetDeviceURI(existing),
                     device_uri);
            papplPrinterDelete(existing);
            existing = NULL;
        }
        if (existing != NULL) {
            papplLog(system, PAPPL_LOGLEVEL_WARN,
                     "Printer '%s' already exists (device '%s'); using it.",
                     printer_name,
                     papplPrinterGetDeviceURI(existing) ?
                         papplPrinterGetDeviceURI(existing) : "(none)");
        } else if (papplPrinterCreate(system, 0, printer_name, DRIVER_NAME,
                                      "MFG:Primera;MDL:Color_Label_900;",
                                      device_uri) == NULL) {
            fprintf(stderr,
                    "lx900e-app: cannot create printer '%s' for '%s': %s\n",
                    printer_name, device_uri, strerror(errno));
            papplSystemDelete(system);
            free(spool_alloc);
            return 1;
        }
    } else if (!state_loaded) {
        papplSystemCreatePrinters(system,
                                  PAPPL_DEVTYPE_USB | PAPPL_DEVTYPE_SOCKET |
                                  PAPPL_DEVTYPE_DNS_SD,
                                  NULL, NULL);
    }

    fprintf(stderr,
            "lx900e-app %s\n"
            "  web interface: http://localhost:%d/\n"
            "  spool:         %s\n",
            LX900_APP_VERSION, port, spool_dir);
    {
        printer_list_t list = { port, 0 };

        papplSystemIteratePrinters(system, print_printer_cb, &list);
        if (list.count == 0)
            fprintf(stderr,
                    "  no printers yet; add one through the web interface\n");
    }
    fflush(stderr);

    papplSystemRun(system);
    papplSystemDelete(system);
    free(spool_alloc);
    return 0;
}
