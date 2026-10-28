/*
 * lx900 - CUPS backend for the Primera LX900e.
 *
 * Device URI:  lx900:/usb://Primera/Color_Label_900?serial=00000001
 *
 * The stock CUPS usb backend does the actual job transfer.  This wrapper adds
 * one thing on top: before the job it asks the printer for its status block,
 * turns the four drop counters into percentages, and publishes them to the
 * scheduler as IPP marker attributes:
 *
 *     ATTR: marker-names="Cyan","Magenta","Yellow","Black"
 *     ATTR: marker-levels=39,21,32,46
 *
 * cupsd merges those into the printer object, which is what IPP clients (the
 * printer properties dialog, system-config-printer, `ipptool`, the web
 * interface) read when they show ink levels.
 *
 * Everything here is best effort: a printer that does not answer costs one
 * second and no attributes, and the job still goes ahead.  This is a wrapper
 * around a working backend, not a replacement for it.
 *
 * Where the numbers come from
 * ---------------------------
 * The 144-byte status block carries four little-endian 32-bit drop counters
 * and the vendor driver compares them against fixed capacities:
 *
 *     offset  counter  capacity
 *     0x38    yellow   1,800,000,000
 *     0x3c    magenta  1,800,000,000
 *     0x40    cyan     1,800,000,000
 *     0x44    black    3,186,000,000
 *
 *     percent remaining = 100 * (capacity - counter) / capacity
 *
 * The order looks odd because it is: the vendor filter reads the block as
 * Y, M, C, K while the log labels them C, M, Y, K.  The capacities and the
 * arithmetic are taken from that driver, so the numbers agree with what the
 * vendor software would show.
 *
 * Build: cc -O2 -o lx900 lx900-backend.c -lusb-1.0
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>

#define STATUS_LEN 144

/* Layout of the status block */
#define OFF_COUNTER_Y 0x38
#define OFF_COUNTER_M 0x3c
#define OFF_COUNTER_C 0x40
#define OFF_COUNTER_K 0x44
#define OFF_TANK_C 0x78
#define OFF_TANK_M 0x79
#define OFF_TANK_Y 0x7a
#define OFF_TANK_K 0x7b
#define OFF_STATE 0x2e
#define OFF_ERROR 0x2f
#define OFF_PAPER 0x6b          /* 0 = no label stock detected */

#define TANK_OK 0x0b

/* Drop-counter full scale, from the vendor driver */
static const double capacity[4] = {
    1800000000.0,   /* cyan    */
    1800000000.0,   /* magenta */
    1800000000.0,   /* yellow  */
    3186000000.0    /* black   */
};

static unsigned le32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
           ((unsigned)p[3] << 24);
}

/*
 * A counter above its capacity means the capacity table does not apply (some
 * firmware revision, or a counter we misread), so report "unknown" rather than
 * a negative number.
 */
static int percent_remaining(int tank, const unsigned char *status)
{
    unsigned used;
    double pct;

    switch (tank) {
    case 0: used = le32(status + OFF_COUNTER_C); break;
    case 1: used = le32(status + OFF_COUNTER_M); break;
    case 2: used = le32(status + OFF_COUNTER_Y); break;
    default: used = le32(status + OFF_COUNTER_K); break;
    }

    if (status[OFF_TANK_C + tank] != TANK_OK)
        return -2;              /* slot missing or cartridge not recognised */
    if ((double)used > capacity[tank])
        return -1;              /* counter and capacity disagree; do not guess */

    pct = 100.0 * (capacity[tank] - (double)used) / capacity[tank];
    if (pct < 0.0)
        return 0;
    if (pct > 100.0)
        return 100;
    return (int)(pct + 0.5);
}

static libusb_device_handle *open_printer(void)
{
    libusb_device **list;
    ssize_t n = libusb_get_device_list(NULL, &list);
    libusb_device_handle *h = NULL;

    if (n < 0)
        return NULL;

    for (ssize_t i = 0; i < n && !h; i++) {
        struct libusb_device_descriptor dd;
        struct libusb_config_descriptor *cfg = NULL;
        int is_printer = 0;

        if (libusb_get_device_descriptor(list[i], &dd))
            continue;
        if (dd.idVendor != 0x0f25 || dd.idProduct != 0x0022)
            continue;
        if (libusb_open(list[i], &h)) {
            h = NULL;
            continue;
        }
        if (!libusb_get_active_config_descriptor(list[i], &cfg) && cfg) {
            for (int k = 0; k < cfg->bNumInterfaces && !is_printer; k++)
                for (int a = 0; a < cfg->interface[k].num_altsetting; a++)
                    if (cfg->interface[k].altsetting[a].bInterfaceClass == 7)
                        is_printer = 1;
            libusb_free_config_descriptor(cfg);
        }
        if (!is_printer) {
            libusb_close(h);
            h = NULL;
        }
    }
    libusb_free_device_list(list, 1);
    return h;
}

static int find_endpoints(libusb_device_handle *h, int *ep_out, int *ep_in)
{
    libusb_device *dev = libusb_get_device(h);
    struct libusb_config_descriptor *cfg = NULL;
    int i, a, e;

    *ep_out = -1;
    *ep_in = -1;
    if (libusb_get_active_config_descriptor(dev, &cfg))
        return -1;
    for (i = 0; i < cfg->bNumInterfaces; i++)
        for (a = 0; a < cfg->interface[i].num_altsetting; a++)
            for (e = 0; e < cfg->interface[i].altsetting[a].bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep =
                    &cfg->interface[i].altsetting[a].endpoint[e];

                if ((ep->bmAttributes & 3) != 2)
                    continue;
                if (ep->bEndpointAddress & 0x80) {
                    if (*ep_in < 0)
                        *ep_in = ep->bEndpointAddress;
                } else if (*ep_out < 0) {
                    *ep_out = ep->bEndpointAddress;
                }
            }
    libusb_free_config_descriptor(cfg);
    return (*ep_out < 0 || *ep_in < 0) ? -1 : 0;
}

/* Ask the printer for its status block.  Returns 0 and fills `out` on success. */
static int read_status(unsigned char *out)
{
    static const unsigned char req[5] = { 0x1b, 0x2a, 0x6d, 0x06, 0x82 };
    libusb_context *ctx = NULL;
    libusb_device_handle *h;
    unsigned char buf[512];
    int ep_out, ep_in, total = 0, r;

    if (libusb_init(&ctx))
        return -1;

    h = open_printer();
    if (!h) {
        libusb_exit(ctx);
        return -1;
    }

    /* A printer that came back from a power cycle can be unconfigured. */
    {
        int cfg = -1;

        if (libusb_get_configuration(h, &cfg) == 0 && cfg == 0)
            libusb_set_configuration(h, 1);
    }
    if (libusb_kernel_driver_active(h, 0) == 1)
        libusb_detach_kernel_driver(h, 0);

    if (libusb_claim_interface(h, 0) || find_endpoints(h, &ep_out, &ep_in)) {
        libusb_close(h);
        libusb_exit(ctx);
        return -1;
    }

    r = libusb_bulk_transfer(h, ep_out, (unsigned char *)req, sizeof(req),
                             &total, 2000);
    if (r) {
        libusb_release_interface(h, 0);
        libusb_close(h);
        libusb_exit(ctx);
        return -1;
    }

    total = 0;
    for (int tries = 0; tries < 12 && total < STATUS_LEN; tries++) {
        int n = 0;

        r = libusb_bulk_transfer(h, ep_in, buf, sizeof(buf), &n, 250);
        if (r == 0 && n > 0) {
            if (total + n > STATUS_LEN)
                n = STATUS_LEN - total;
            memcpy(out + total, buf, (size_t)n);
            total += n;
        } else if (r != LIBUSB_ERROR_TIMEOUT) {
            break;
        }
    }

    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return total == STATUS_LEN ? 0 : -1;
}

/*
 * Append to a fixed buffer without ever letting the offset run past the end.
 * snprintf() returns the length it would have written, so a naive "off +="
 * makes the next capacity subtraction underflow.
 */
static int app(char *buf, size_t cap, int off, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (off < 0 || (size_t)off >= cap)
        return (int)cap - 1;
    va_start(ap, fmt);
    n = vsnprintf(buf + off, cap - (size_t)off, fmt, ap);
    va_end(ap);
    if (n < 0)
        return off;
    if ((size_t)off + (size_t)n >= cap)
        return (int)cap - 1;
    return off + n;
}

/*
 * Optional behaviour, from /etc/lx900.conf:
 *
 *     paper_check=1     refuse to print while the printer sees no label stock
 *     paper_wait=120    seconds to wait for stock before giving up
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

static int media_present(const unsigned char *status)
{
    /* Bit 0 of the error byte is the loop sensor: end of the roll. */
    return status[OFF_PAPER] != 0 && !(status[OFF_ERROR] & 1);
}

/*
 * Do not print into thin air.  The printer happily fires the heads with no
 * stock loaded - ink ends up on the platen - so hold the job until it reports
 * media, exactly like the vendor filter does with its "No label stock loaded"
 * recoverable error.  Returns 0 if it is safe to print.
 */
static int wait_for_media(unsigned char *status)
{
    int wait = conf_int("paper_wait", 120);
    int enabled = conf_int("paper_check", 1);
    int waited = 0;

    if (!enabled || status[OFF_STATE] != 0x49 || media_present(status)) {
        fprintf(stderr, "STATE: -media-empty\n");
        return 0;
    }

    fprintf(stderr, "STATE: +media-empty\n");
    fprintf(stderr, "INFO: no label stock loaded, waiting up to %d s\n", wait);
    while (waited < wait) {
        sleep(2);
        waited += 2;
        if (read_status(status) == 0 && media_present(status)) {
            fprintf(stderr, "STATE: -media-empty\n");
            fprintf(stderr, "INFO: label stock loaded, printing\n");
            return 0;
        }
    }
    fprintf(stderr, "ERROR: no label stock loaded after %d s, not printing\n", wait);
    return 1;
}

/* Publish the levels.  Anything cupsd cannot parse it ignores. */
static void report_levels(const unsigned char *status)
{
    static const char *names[] = { "Cyan", "Magenta", "Yellow", "Black" };
    static const char *colours[] = { "#00FFFF", "#FF00FF", "#FFFF00", "#000000" };
    int level[4];
    char msg[160];
    int i, off;

    for (i = 0; i < 4; i++)
        level[i] = percent_remaining(i, status);

    fprintf(stderr, "ATTR: marker-names=\"%s\",\"%s\",\"%s\",\"%s\"\n",
            names[0], names[1], names[2], names[3]);
    fprintf(stderr, "ATTR: marker-colors=%s,%s,%s,%s\n",
            colours[0], colours[1], colours[2], colours[3]);
    fprintf(stderr, "ATTR: marker-types=ink,ink,ink,ink\n");
    fprintf(stderr, "ATTR: marker-levels=%d,%d,%d,%d\n",
            level[0], level[1], level[2], level[3]);
    fprintf(stderr, "ATTR: marker-high-levels=100,100,100,100\n");
    fprintf(stderr, "ATTR: marker-low-levels=10,10,10,10\n");

    /* Cartridge faults, as the standard IPP state reasons clients understand. */
    {
        int colour_bad = 0, black_bad = 0, low = 0;

        for (i = 0; i < 4; i++) {
            if (level[i] == -2) {
                if (i == 3)
                    black_bad = 1;
                else
                    colour_bad = 1;
            }
            if (level[i] >= 0 && level[i] <= 10)
                low = 1;
        }

        off = 0;
        if (colour_bad)
            off = app(msg, sizeof(msg), off,
                      "Replace the colour cartridge (slot 1). ");
        if (black_bad)
            off = app(msg, sizeof(msg), off,
                      "Replace the black cartridge (slot 2). ");
        for (i = 0; i < 4; i++) {
            if (level[i] >= 0)
                off = app(msg, sizeof(msg), off, "%s%s %d%%",
                          i ? ", " : "", names[i], level[i]);
            else
                off = app(msg, sizeof(msg), off, "%s%s %s",
                          i ? ", " : "", names[i],
                          level[i] == -2 ? "missing" : "unknown");
        }

        fprintf(stderr, "STATE: -marker-supply-empty -marker-supply-low\n");
        if (colour_bad || black_bad)
            fprintf(stderr, "STATE: +marker-supply-empty\n");
        if (low)
            fprintf(stderr, "STATE: +marker-supply-low\n");
    }
    fprintf(stderr, "ATTR: marker-message=\"%s\"\n", msg);

    /* The printer answers 0x31 while it is still starting up. */
    if (status[OFF_STATE] != 0x49)
        fprintf(stderr, "STATE: +offline-report\n");
    else
        fprintf(stderr, "STATE: -offline-report\n");
}

/* `lpinfo -v`: advertise the same devices the usb backend finds, wrapped. */
static int discover(void)
{
    char line[1024];
    /* As a normal user the usb backend is not executable (it is root-only),
     * which is the same reason `lpinfo -v` needs root here.  Stay quiet. */
    FILE *p = popen("/usr/lib/cups/backend/usb 2>/dev/null", "r");

    if (!p)
        return 1;
    while (fgets(line, sizeof(line), p)) {
        if (!strncmp(line, "direct usb:", 11)) {
            fputs("direct lx900:/usb:", stdout);
            fputs(line + 11, stdout);
        } else {
            fputs(line, stdout);
        }
    }
    pclose(p);
    return 0;
}

int main(int argc, char **argv)
{
    const char *uri = getenv("DEVICE_URI");
    const char *sbin = getenv("CUPS_SERVERBIN");
    char usb[256];
    unsigned char status[STATUS_LEN];

    if (argc == 1)
        return discover();

    if (read_status(status) == 0) {
        report_levels(status);
        if (wait_for_media(status))
            return 1;
    }

    /*
     * Hand the job to the real backend.  DEVICE_URI loses our "lx900:" scheme
     * so the usb backend sees the URI it knows.
     *
     * The stored form is lx900:/usb://..., i.e. the remainder starts with a
     * slash that belongs to our separator, not to the usb URI.  Dropping the
     * scheme alone would leave "/usb://..." and the usb backend would not
     * parse it, so drop one separator slash as well (but never two: a
     * genuine "//" host part has to survive).
     */
    if (uri && !strncmp(uri, "lx900:", 6)) {
        const char *rest = uri + 6;

        if (rest[0] == '/' && rest[1] != '/')
            rest++;
        setenv("DEVICE_URI", rest, 1);
    }

    if (getenv("LX900_BACKEND_DEBUG"))
        fprintf(stderr, "DEBUG: device URI is %s\n",
                getenv("DEVICE_URI") ? getenv("DEVICE_URI") : "(unset)");

    /* Diagnostic: publish the attributes and stop, do not print anything. */
    if (getenv("LX900_BACKEND_PROBE"))
        return 0;

    snprintf(usb, sizeof(usb), "%s/backend/usb",
             sbin && *sbin ? sbin : "/usr/lib/cups");
    argv[0] = usb;
    execv(usb, argv);

    fprintf(stderr, "ERROR: cannot run %s: %m\n", usb);
    return 1;
}
