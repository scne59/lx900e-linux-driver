/*
 * lx900-send - write a raw Primera LX900e print stream to the printer over USB.
 *
 * Usage:
 *   lx900-send [-v] [-d] [-w sec] [-o log] FILE   send FILE
 *   -d                              append the end-of-document packets
 *   -w sec                          linger after the write (default 0.5)
 *   -o FILE                         raw copy of everything the printer sends
 *   lx900-send -l                   list candidate USB printers
 *   lx900-send -i                   read the IEEE-1284 device ID string
 *   lx900-send -b                   drain the bulk-IN status channel for 5 s
 *   lx900-send -q                   send a status request, dump the 144-byte reply
 *   lx900-send -c [-T sec]          guided cartridge change: say which one, then
 *                                   watch until the printer accepts the new one
 *   lx900-send -r                   USB printer-class soft reset
 *   lx900-send -R                   USB bus reset (recover a wedged device)
 *   lx900-send -n                   do not detach the kernel usblp driver
 *
 * The LX900e is a USB printer-class device (interface class 7, two bulk
 * endpoints).  The printer talks back on its bulk-IN endpoint while a job is
 * running; the CUPS usb backend drains that endpoint in a reader thread.  If
 * nobody reads it the printer can drop off the bus part-way through a job, so
 * this tool runs the same kind of reader.
 *
 * Build: cc -O2 -o lx900-send lx900-send.c -lusb-1.0 -lpthread
 */
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define CHUNK (16 * 1024)

/* Offsets inside the 144-byte status block */
#define OFF_STATE 0x2e
#define OFF_ERROR 0x2f
#define OFF_PAPER 0x6b

static int verbose;
static int send_eod;
static int linger_ms = 500;
static FILE *inlog;                 /* raw copy of everything the printer says */
static volatile int reader_stop;
static volatile unsigned long long reader_bytes;

struct reader_arg {
    libusb_device_handle *h;
    unsigned char ep;
};

static void *reader_thread(void *arg)
{
    struct reader_arg *ra = arg;
    unsigned char buf[4096];

    while (!reader_stop) {
        int n = 0;
        int r = libusb_bulk_transfer(ra->h, ra->ep, buf, sizeof(buf), &n, 200);

        if (r == 0 && n > 0) {
            reader_bytes += (unsigned)n;
            if (inlog) {
                fwrite(buf, 1, (size_t)n, inlog);
                fflush(inlog);
            }
            if (verbose) {
                int k;

                fprintf(stderr, "IN %d bytes:\n", n);
                for (k = 0; k < n; k++) {
                    fprintf(stderr, " %02x", buf[k]);
                    if ((k & 31) == 31)
                        fprintf(stderr, "\n");
                }
                if (n & 31)
                    fprintf(stderr, "\n");
            }
        } else if (r == LIBUSB_ERROR_NO_DEVICE) {
            fprintf(stderr, "reader: device disconnected\n");
            return NULL;
        } else if (r == LIBUSB_ERROR_PIPE) {
            libusb_clear_halt(ra->h, ra->ep);
        }
    }
    return NULL;
}

static libusb_device_handle *open_printer(int vid, int pid)
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
        if (vid && (dd.idVendor != vid || dd.idProduct != pid))
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

    *ep_out = -1;
    *ep_in = -1;
    if (libusb_get_active_config_descriptor(dev, &cfg))
        return -1;

    for (int k = 0; k < cfg->bNumInterfaces; k++)
        for (int a = 0; a < cfg->interface[k].num_altsetting; a++) {
            const struct libusb_interface_descriptor *id =
                &cfg->interface[k].altsetting[a];

            if (id->bInterfaceClass != 7)
                continue;
            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ed = &id->endpoint[e];

                if ((ed->bmAttributes & 3) != 2)
                    continue;
                if (ed->bEndpointAddress & 0x80) {
                    if (*ep_in < 0)
                        *ep_in = ed->bEndpointAddress;
                } else if (*ep_out < 0) {
                    *ep_out = ed->bEndpointAddress;
                }
            }
        }
    libusb_free_config_descriptor(cfg);
    return (*ep_out < 0) ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Status block                                                        */
/* ------------------------------------------------------------------ */

static unsigned le32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
           ((unsigned)p[3] << 24);
}

static const char *const tank_name[4] = {
    "cyan", "magenta", "yellow", "black"
};
static const char *const tank_label[4] = {
    "Cyan", "Magenta", "Yellow", "Black"
};
/*
 * Drop-counter offset per tank, in C/M/Y/K order.  The offsets look scrambled
 * because they are: the vendor driver reads the block as Y, M, C, K.
 */
static const unsigned tank_counter[4] = { 0x40, 0x3c, 0x38, 0x44 };
static const double tank_capacity[4] = {
    1800000000.0, 1800000000.0, 1800000000.0, 3186000000.0
};

#define TANK_OK 0x0b
#define TANK_MISSING 0x02

/* Percentage of full scale left, or -2 for absent, -1 for "cannot tell". */
static int tank_percent(int i, const unsigned char *acc)
{
    unsigned used = le32(acc + tank_counter[i]);

    if (acc[0x78 + i] != TANK_OK)
        return -2;
    if ((double)used > tank_capacity[i])
        return -1;
    return (int)(100.0 * (tank_capacity[i] - (double)used) / tank_capacity[i] + 0.5);
}

static const char *tank_state(int i, const unsigned char *acc)
{
    unsigned s = acc[0x78 + i];

    return s == TANK_OK ? "ok" : s == TANK_MISSING ? "MISSING" : "INVALID";
}

/*
 * Hardware status query: the vendor filter writes "ESC * m 06 82" (5 bytes) and
 * then reads a 144-byte status block back over the back-channel.
 */
static int fetch_status(libusb_device_handle *h, unsigned char *acc, int *out_total)
{
    static const unsigned char req[5] = { 0x1b, 0x2a, 0x6d, 0x06, 0x82 };
    unsigned char buf[512];
    int ep_out, ep_in, total = 0, n = 0;

    *out_total = 0;
    if (find_endpoints(h, &ep_out, &ep_in) || ep_in < 0) {
        fprintf(stderr, "need both bulk endpoints\n");
        return 1;
    }
    {
        int r = libusb_bulk_transfer(h, ep_out, (unsigned char *)req, sizeof(req),
                                     &n, 5000);
        if (r) {
            fprintf(stderr, "status request write failed: %s\n",
                    libusb_error_name(r));
            return 1;
        }
    }
    {
        struct timespec t0, now;
        long elapsed_ms = 0;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (total < 144 && elapsed_ms < 3000) {
            int r = libusb_bulk_transfer(h, ep_in, buf, sizeof(buf), &n, 250);

            clock_gettime(CLOCK_MONOTONIC, &now);
            elapsed_ms = (now.tv_sec - t0.tv_sec) * 1000 +
                         (now.tv_nsec - t0.tv_nsec) / 1000000;
            if (r == LIBUSB_ERROR_TIMEOUT)
                continue;
            if (r) {
                fprintf(stderr, "status read failed: %s\n", libusb_error_name(r));
                break;
            }
            if (n <= 0)
                continue;
            if (total + n > 144)
                n = 144 - total;
            memcpy(acc + total, buf, (size_t)n);
            total += n;
        }
    }
    *out_total = total;
    return total == 0;
}

static void print_levels(const unsigned char *acc)
{
    int i;

    printf("  tanks:");
    for (i = 0; i < 4; i++)
        printf(" %s=%s", tank_name[i], tank_state(i, acc));
    printf("\n  ink:  ");
    for (i = 0; i < 4; i++) {
        int pct = tank_percent(i, acc);

        if (pct == -2)
            printf(" %s=%s", tank_name[i],
                   acc[0x78 + i] == TANK_MISSING ? "missing" : "invalid");
        else if (pct < 0)
            printf(" %s=?", tank_name[i]);
        else
            printf(" %s=%d%%", tank_name[i], pct);
    }
    /* Raw drop counters: coarse percentages hide small jobs and purges. */
    printf("\n  used: ");
    for (i = 0; i < 4; i++)
        printf(" %s=%u", tank_name[i], le32(acc + tank_counter[i]));
    printf("\n");
}

static int query_status(libusb_device_handle *h)
{
    unsigned char acc[144];
    int total = 0, i;

    if (fetch_status(h, acc, &total))
        return 1;

    printf("status reply: %d bytes\n", total);
    for (i = 0; i < total; i += 16) {
        int k, lim = total - i < 16 ? total - i : 16;

        printf("  %03x:", i);
        for (k = 0; k < lim; k++)
            printf(" %02x", acc[i + k]);
        for (; k < 16; k++)
            printf("   ");
        printf("  |");
        for (k = 0; k < lim; k++) {
            unsigned char c = acc[i + k];

            putchar(c >= 32 && c < 127 ? c : '.');
        }
        printf("|\n");
    }
    if (total >= 0x30)
        printf("  type=0x%02x error=0x%02x\n", acc[OFF_STATE], acc[OFF_ERROR]);
    if (total >= 0x7c) {
        print_levels(acc);
        printf("  paper=%s%s\n", acc[OFF_PAPER] ? "loaded" : "NONE",
               (acc[OFF_ERROR] & 1) ? "  (loop sensor: out of stock)" : "");
    }
    if (total >= 0x60) {
        double page_w = (double)(acc[0x5c] | acc[0x5d] << 8) / 100.0;
        double page_h = (double)(acc[0x5e] | acc[0x5f] << 8) / 100.0;

        printf("  present=%u sensor=%u  last page %.2f x %.2f in  job=%u\n",
               acc[0x58], acc[0x5a], page_w, page_h, acc[0x03]);
    }
    if (total == 0)
        fprintf(stderr,
                "the printer accepted the request but sent nothing back: the "
                "USB side is alive,\nbut the firmware is not answering.  Power "
                "it off at the mains, wait ten\nseconds, switch it on again and "
                "wait until it has finished starting up.\n");
    return total < 144;
}

/* ------------------------------------------------------------------ */
/* Cartridge change                                                    */
/* ------------------------------------------------------------------ */

/* Which physical cartridge carries this plane. */
static const char *tank_slot(int i)
{
    return i == 3 ? "the black cartridge (slot 2)"
                  : "the colour cartridge (slot 1, holds C/M/Y)";
}

/*
 * Guided cartridge change.
 *
 * The printer does the mechanical part itself: the two top buttons move the
 * carriage to the change position, and a button press afterwards completes the
 * install.  The vendor software only drives a state machine over the print
 * stream to get there, and those commands are undocumented; guessing them on
 * firmware that has already wedged several times is not worth it.  What is
 * missing on Linux is the part around it - knowing which cartridge, and
 * knowing when the printer has accepted the new one - and that is what this
 * does, by watching the status block.
 */
/*
 * Print the levels and, if anything is wrong, which cartridge to replace and
 * the front-panel steps.  Returns how many tanks need attention.
 */
static int cartridge_advice(const unsigned char *acc)
{
    int i, need = 0;

    print_levels(acc);
    printf("\n");

    for (i = 0; i < 4; i++) {
        if (acc[0x78 + i] != TANK_OK) {
            printf("%s reports %s.\n", tank_label[i], tank_state(i, acc));
            printf("Replace %s.\n\n", tank_slot(i));
            need++;
        }
    }

    if (need == 0) {
        int colour_low = 0, black_low = 0;

        for (i = 0; i < 3; i++)
            if (tank_percent(i, acc) >= 0 && tank_percent(i, acc) < 15)
                colour_low = 1;
        if (tank_percent(3, acc) >= 0 && tank_percent(3, acc) < 15)
            black_low = 1;

        if (colour_low) {
            printf("The colour cartridge (slot 1) is low:");
            for (i = 0; i < 3; i++)
                if (tank_percent(i, acc) >= 0 && tank_percent(i, acc) < 15)
                    printf(" %s %d%%", tank_name[i], tank_percent(i, acc));
            printf("\n");
        }
        if (black_low)
            printf("The black cartridge (slot 2) is low: black %d%%\n",
                   tank_percent(3, acc));

        printf("Nothing needs changing right now.\n");
        return 0;
    }

    printf("On the printer:\n");
    printf("  1. Press the top two buttons together to move the carriage out.\n");
    printf("  2. Take out the cartridge and fit the new one.\n");
    printf("  3. Press any button to complete the install.\n\n");
    return need;
}

static int cartridge_change(libusb_device_handle *h, int iface, int timeout_s)
{
    unsigned char acc[144];
    unsigned first_used[4];
    int total = 0, i, need = 0, waited = 0, before[4];

    if (fetch_status(h, acc, &total)) {
        fprintf(stderr, "the printer did not answer the status request\n");
        return 1;
    }

    need = cartridge_advice(acc);
    if (need == 0)
        return 0;

    for (i = 0; i < 4; i++) {
        before[i] = tank_percent(i, acc);
        first_used[i] = le32(acc + tank_counter[i]);
    }

    printf("Waiting for the printer to accept it (up to %d s, Ctrl-C to stop)...\n",
           timeout_s);
    fflush(stdout);

    /* Do not sit on the USB interface while the operator works. */
    libusb_release_interface(h, iface);

    while (waited < timeout_s) {
        int ok = 1;

        sleep(2);
        waited += 2;

        if (libusb_claim_interface(h, iface)) {
            fprintf(stderr, "\ncannot claim the printer; is a job running?\n");
            return 1;
        }
        if (fetch_status(h, acc, &total)) {
            libusb_release_interface(h, iface);
            printf("  ... printer not answering\n");
            continue;
        }
        libusb_release_interface(h, iface);

        for (i = 0; i < 4; i++) {
            if (acc[0x78 + i] != TANK_OK) {
                ok = 0;
                printf("  ... %s still %s\n", tank_name[i], tank_state(i, acc));
            }
        }
        if (ok) {
            printf("\nThe printer has accepted the new cartridge.\n");
            print_levels(acc);
            for (i = 0; i < 4; i++) {
                int now = tank_percent(i, acc);

                if (now >= 0 && before[i] >= 0 && now > before[i] + 5)
                    printf("  %s: %d%% -> %d%% (new cartridge)\n",
                           tank_label[i], before[i], now);
                else if (first_used[i] && le32(acc + tank_counter[i]) < first_used[i] / 2)
                    printf("  %s: drop counter reset (new cartridge)\n",
                           tank_label[i]);
            }
            printf("\nPrint a diagnostic page to check the nozzles:\n");
            printf("  lp -d LX900e -o InkCartridge=Color build/colortest-2x1.png\n");
            return 0;
        }
    }

    fprintf(stderr, "\ntimed out after %d s; the printer still reports a "
                    "cartridge fault\n", timeout_s);
    return 1;
}

/*
 * End-of-document packets.  The vendor filter (rastertoCL900) writes these
 * two framed records to its output after HPEEndDoc, and the printer does not
 * advance the media until it has seen them.  Both carry job id 1.
 */
static const unsigned char eod_0e[20] = {
    0xa5, 0x00, 0x11, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x01,
    0x01, 0x00, 0x1b, 0x2a, 0x05, 0x0e, 0x00, 0x00, 0x00, 0x01
};
static const unsigned char eod_0b[20] = {
    0xa5, 0x00, 0x11, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x01,
    0x01, 0x00, 0x1b, 0x2a, 0x05, 0x0b, 0x00, 0x00, 0x00, 0x01
};

static int send_file(libusb_device_handle *h, const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    pthread_t tid;
    struct reader_arg ra = { h, 0x83 };
    int have_reader = 0;
    int ep_out, ep_in;
    long total, sent = 0;
    int rc = 0;
    int file_has_0e = 0;
    int file_has_0b = 0;

    if (!f) {
        perror(path);
        return 1;
    }
    if (find_endpoints(h, &ep_out, &ep_in)) {
        fprintf(stderr, "no bulk OUT endpoint\n");
        fclose(f);
        return 1;
    }
    if (ep_in >= 0)
        ra.ep = (unsigned char)ep_in;
    if (verbose)
        fprintf(stderr, "bulk OUT 0x%02x, bulk IN 0x%02x\n", ep_out, ra.ep);

    fseek(f, 0, SEEK_END);
    total = ftell(f);
    fseek(f, 0, SEEK_SET);

    /*
     * Work out which end-of-document packets the stream already contains, so
     * that the same packet is never sent twice: two 0x0e packets make the
     * printer advance the media twice.  Vendor .prn files end with 0x0e only;
     * our own filter ends with 0x0e followed by 0x0b.
     */
    if (total >= 20) {
        unsigned char tail[40];
        size_t want = total >= 40 ? 40 : (size_t)total;

        if (fseek(f, total - (long)want, SEEK_SET) == 0 &&
            fread(tail, 1, want, f) == want) {
            /*
             * Compare all 20 bytes: the two packets differ only in the byte
             * before the trailing 0x00000001, so a 16-byte compare would
             * always claim both are present and -d would append nothing.
             */
            if (want >= 20 && memcmp(tail + want - 20, eod_0e, 20) == 0)
                file_has_0e = 1;
            if (want >= 20 && memcmp(tail + want - 20, eod_0b, 20) == 0)
                file_has_0b = 1;
            if (want >= 40 && memcmp(tail + want - 40, eod_0e, 20) == 0)
                file_has_0e = 1;
            if (want >= 40 && memcmp(tail + want - 40, eod_0b, 20) == 0)
                file_has_0b = 1;
        }
        fseek(f, 0, SEEK_SET);
    }

    if (ep_in >= 0 && pthread_create(&tid, NULL, reader_thread, &ra) == 0)
        have_reader = 1;

    buf = malloc(CHUNK);
    if (!buf) {
        rc = 1;
        goto out;
    }

    while (sent < total) {
        size_t want = (size_t)((total - sent) > CHUNK ? CHUNK : (total - sent));
        size_t got = fread(buf, 1, want, f);
        int done = 0;

        if (got == 0) {
            fprintf(stderr, "short read at %ld\n", sent);
            rc = 1;
            break;
        }
        while (done < (int)got) {
            int n = 0;
            int r = libusb_bulk_transfer(h, ep_out, buf + done,
                                         (int)got - done, &n, 30000);

            if (r == LIBUSB_ERROR_PIPE) {
                libusb_clear_halt(h, ep_out);
                continue;
            }
            if (r) {
                fprintf(stderr, "bulk write failed at %ld/%ld: %s\n",
                        sent + done, total, libusb_error_name(r));
                rc = 1;
                goto out;
            }
            done += n;
        }
        sent += (long)got;
    }

    if (verbose)
        fprintf(stderr, "\rsent %ld bytes, status channel %llu bytes\n", sent,
                reader_bytes);

    if (send_eod) {
        const unsigned char *pk[2];
        int k, npk = 0;

        if (!file_has_0e)
            pk[npk++] = eod_0e;
        if (!file_has_0b)
            pk[npk++] = eod_0b;
        for (k = 0; k < npk; k++) {
            int done = 0;

            while (done < 20) {
                int n = 0;
                int r = libusb_bulk_transfer(h, ep_out,
                                             (unsigned char *)pk[k] + done,
                                             20 - done, &n, 30000);

                if (r == LIBUSB_ERROR_PIPE) {
                    libusb_clear_halt(h, ep_out);
                    continue;
                }
                if (r) {
                    fprintf(stderr, "end-of-document write failed: %s\n",
                            libusb_error_name(r));
                    rc = 1;
                    goto out;
                }
                done += n;
            }
        }
        if (verbose) {
            if (npk == 0)
                fprintf(stderr, "stream already ends with the end-of-document packets\n");
            else
                fprintf(stderr, "sent %d end-of-document packet(s)%s%s\n", npk,
                        file_has_0e ? "" : " 0x0e",
                        file_has_0b ? "" : " 0x0b");
        }
    }

    /* let the printer finish before we release the interface */
    usleep((useconds_t)linger_ms * 1000);

out:
    free(buf);
    reader_stop = 1;
    if (have_reader)
        pthread_join(tid, NULL);
    if (verbose || inlog)
        fprintf(stderr, "bulk IN total: %llu bytes\n",
                (unsigned long long)reader_bytes);
    if (inlog)
        fclose(inlog);
    fclose(f);
    return rc;
}

int main(int argc, char **argv)
{
    int do_list = 0, do_reset = 0, do_id = 0, do_chan = 0, do_query = 0, opt;
    int do_cartridge = 0, change_timeout = 300;
    int do_bus_reset = 0, no_detach = 0;
    int iface = 0, rc = 0;
    int vid = 0, pid = 0;
    const char *env;
    libusb_context *ctx;
    libusb_device_handle *h;

    while ((opt = getopt(argc, argv, "vlribqRcndw:o:T:")) != -1) {
        switch (opt) {
        case 'v': verbose = 1; break;
        case 'l': do_list = 1; break;
        case 'r': do_reset = 1; break;
        case 'i': do_id = 1; break;
        case 'b': do_chan = 1; break;
        case 'q': do_query = 1; break;
        case 'R': do_bus_reset = 1; break;
        case 'n': no_detach = 1; break;
        case 'd': send_eod = 1; break;
        case 'c': do_cartridge = 1; break;
        case 'T': change_timeout = atoi(optarg); break;
        case 'w': linger_ms = atoi(optarg) * 1000; break;
        case 'o':
            inlog = fopen(optarg, "ab");
            if (!inlog) {
                perror(optarg);
                return 1;
            }
            break;
        default:
            fprintf(stderr, "usage: %s [-v] [-d] [-w sec] [-o log] FILE"
                    " | -l | -i | -b | -q | -c [-T sec] | -r | -R\n",
                    argv[0]);
            return 2;
        }
    }

    env = getenv("LX900_VID");
    if (env)
        vid = (int)strtol(env, NULL, 16);
    env = getenv("LX900_PID");
    if (env)
        pid = (int)strtol(env, NULL, 16);

    if (libusb_init(&ctx)) {
        fprintf(stderr, "libusb_init failed\n");
        return 1;
    }
    h = open_printer(vid, pid);
    if (!h) {
        fprintf(stderr, "no USB printer found (set LX900_VID/LX900_PID)\n");
        libusb_exit(ctx);
        return 1;
    }

    struct libusb_device_descriptor dd;

    libusb_get_device_descriptor(libusb_get_device(h), &dd);
    if (verbose)
        fprintf(stderr, "opened %04x:%04x\n", dd.idVendor, dd.idProduct);

    if (do_list) {
        libusb_close(h);
        libusb_exit(ctx);
        return 0;
    }

    if (do_id) {
        unsigned char idbuf[1024];
        int r = libusb_control_transfer(h, 0xa1, 0, 0, 0, idbuf, sizeof(idbuf),
                                        5000);

        if (r < 0) {
            fprintf(stderr, "GET_DEVICE_ID failed: %s\n", libusb_error_name(r));
        } else if (r < 2) {
            fprintf(stderr, "GET_DEVICE_ID returned %d bytes\n", r);
        } else {
            int len = (idbuf[0] << 8) | idbuf[1];

            if (len > r - 2)
                len = r - 2;
            printf("%.*s\n", len, (char *)idbuf + 2);
        }
        libusb_close(h);
        libusb_exit(ctx);
        return r < 0;
    }

    if (do_bus_reset) {
        int r = libusb_reset_device(h);

        fprintf(stderr, "bus reset -> %s\n", r ? libusb_error_name(r) : "ok");
        libusb_close(h);
        libusb_exit(ctx);
        return r != 0;
    }

    /*
     * After a power cycle the printer can come back unconfigured: no
     * configuration is selected, so there is no active interface to claim and
     * libusb_claim_interface() fails with BUSY.  The CUPS usb backend does the
     * same thing for itself ("Switching USB device configuration: 0 -> 1"),
     * which is why CUPS can print while this tool cannot.
     */
    {
        int cfg = -1;
        int r = libusb_get_configuration(h, &cfg);

        if (r == 0) {
            if (verbose)
                fprintf(stderr, "active configuration: %d\n", cfg);
            if (cfg == 0) {
                r = libusb_set_configuration(h, 1);
                if (verbose)
                    fprintf(stderr, "set configuration 1 -> %s\n",
                            r ? libusb_error_name(r) : "ok");
            }
        }
    }

    /* take the interface away from usblp */
    {
        int active = libusb_kernel_driver_active(h, iface);

        if (verbose)
            fprintf(stderr, "kernel driver on interface %d: %s\n", iface,
                    active == 1 ? "yes (usblp)" : active == 0 ? "no" : "unknown");
        if (active == 1) {
            if (no_detach) {
                fprintf(stderr, "kernel driver is bound and -n was given; "
                                "cannot claim the interface\n");
                libusb_close(h);
                libusb_exit(ctx);
                return 1;
            }
            libusb_detach_kernel_driver(h, iface);
            if (verbose)
                fprintf(stderr, "detached usblp\n");
        }
    }
    if (libusb_claim_interface(h, iface)) {
        fprintf(stderr, "claim failed: interface %d is busy.  Another process\n"
                        "owns the printer - usually the CUPS usb backend while a\n"
                        "job is running.  Check 'lpstat -o' and let the job finish,\n"
                        "or cancel it.  Firmware wedges are different: those answer\n"
                        "the status request with 0 bytes.\n", iface);
        libusb_close(h);
        libusb_exit(ctx);
        return 1;
    }

    if (do_cartridge) {
        rc = cartridge_change(h, iface,
                              change_timeout > 0 ? change_timeout : 300);
    } else if (do_query) {
        rc = query_status(h);
    } else if (do_chan) {
        unsigned char buf[4096];
        int total = 0;

        fprintf(stderr, "reading status channel for 5 s\n");
        for (int sec = 0; sec < 5; sec++) {
            int n = 0;
            int r = libusb_bulk_transfer(h, 0x83, buf, sizeof(buf), &n, 1000);

            if (r == 0 && n > 0) {
                int k, lim = n < 64 ? n : 64;

                printf("IN %d bytes:", n);
                for (k = 0; k < lim; k++)
                    printf(" %02x", buf[k]);
                printf("\n");
                total += n;
            } else if (r != LIBUSB_ERROR_TIMEOUT) {
                fprintf(stderr, "read -> %s\n", libusb_error_name(r));
                break;
            }
        }
        fprintf(stderr, "total %d bytes\n", total);
    } else if (do_reset) {
        int r = libusb_control_transfer(h, 0x21, 2 /* SOFT_RESET */, 0, 0, NULL,
                                        0, 5000);

        fprintf(stderr, "soft reset -> %d\n", r);
        rc = r < 0;
    } else if (optind >= argc) {
        fprintf(stderr, "no input file\n");
        rc = 2;
    } else {
        rc = send_file(h, argv[optind]);
    }

    libusb_release_interface(h, iface);
    libusb_close(h);
    libusb_exit(ctx);
    return rc;
}
