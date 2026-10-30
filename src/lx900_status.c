/*
 * SPDX-License-Identifier: MIT
 *
 * See include/lx900_status.h.
 */
#include "lx900_status.h"

#include <stdio.h>
#include <string.h>

/* Status layout (docs/PROTOCOL.md section 5). */
#define OFF_STATE     0x2e
#define OFF_ERROR     0x2f
#define OFF_COUNTER_Y 0x38
#define OFF_COUNTER_M 0x3c
#define OFF_COUNTER_C 0x40
#define OFF_COUNTER_K 0x44
#define OFF_PAPER     0x6b
#define OFF_TANK_C    0x78
#define OFF_TANK_M    0x79
#define OFF_TANK_Y    0x7a
#define OFF_TANK_K    0x7b

#define TANK_OK 0x0b

#ifdef HAVE_LIBUSB

/* Drop-counter full scale, from the vendor driver. */
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

static void decode(lx900_status_t *st)
{
    int tank;

    for (tank = 0; tank < 4; tank++) {
        unsigned used;
        double pct;

        switch (tank) {
        case 0: used = le32(st->raw + OFF_COUNTER_C); break;
        case 1: used = le32(st->raw + OFF_COUNTER_M); break;
        case 2: used = le32(st->raw + OFF_COUNTER_Y); break;
        default: used = le32(st->raw + OFF_COUNTER_K); break;
        }

        if (st->raw[OFF_TANK_C + tank] != TANK_OK) {
            st->level[tank] = -2;       /* slot empty or not recognised */
            continue;
        }
        if ((double)used > capacity[tank]) {
            st->level[tank] = -1;       /* counter and capacity disagree */
            continue;
        }
        pct = 100.0 * (capacity[tank] - (double)used) / capacity[tank];
        if (pct < 0.0)
            pct = 0.0;
        if (pct > 100.0)
            pct = 100.0;
        st->level[tank] = (int)(pct + 0.5);
    }

    st->state = st->raw[OFF_STATE];
    st->error = st->raw[OFF_ERROR];
    st->media_present = st->raw[OFF_PAPER] != 0;
    st->out_of_stock = (st->error & 0x01) != 0;
}

#include <libusb-1.0/libusb.h>

#define LX900_VID 0x0f25
#define LX900_PID 0x0022

static libusb_device_handle *open_printer(void)
{
    libusb_device **list;
    ssize_t n = libusb_get_device_list(NULL, &list);
    libusb_device_handle *h = NULL;
    ssize_t i;

    if (n < 0)
        return NULL;

    for (i = 0; i < n && !h; i++) {
        struct libusb_device_descriptor dd;
        struct libusb_config_descriptor *cfg = NULL;
        int is_printer = 0;
        int k, a;

        if (libusb_get_device_descriptor(list[i], &dd))
            continue;
        if (dd.idVendor != LX900_VID || dd.idProduct != LX900_PID)
            continue;
        if (libusb_open(list[i], &h)) {
            h = NULL;
            continue;
        }
        if (!libusb_get_active_config_descriptor(list[i], &cfg) && cfg) {
            for (k = 0; k < cfg->bNumInterfaces && !is_printer; k++)
                for (a = 0; a < cfg->interface[k].num_altsetting; a++)
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

int lx900_status_read(lx900_status_t *st)
{
    static const unsigned char req[5] = { 0x1b, 0x2a, 0x6d, 0x06, 0x82 };
    libusb_context *ctx = NULL;
    libusb_device_handle *h;
    unsigned char buf[512];
    int ep_out, ep_in, total = 0, r, tries;

    if (st == NULL)
        return -1;
    memset(st, 0, sizeof(*st));
    st->level[0] = st->level[1] = st->level[2] = st->level[3] = -1;

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
    for (tries = 0; tries < 12 && total < LX900_STATUS_LEN; tries++) {
        int n = 0;

        r = libusb_bulk_transfer(h, ep_in, buf, sizeof(buf), &n, 250);
        if (r == 0 && n > 0) {
            if (total + n > LX900_STATUS_LEN)
                n = LX900_STATUS_LEN - total;
            memcpy(st->raw + total, buf, (size_t)n);
            total += n;
        } else if (r != LIBUSB_ERROR_TIMEOUT) {
            break;
        }
    }

    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);

    if (total != LX900_STATUS_LEN)
        return -1;
    decode(st);
    return 0;
}

#else  /* !HAVE_LIBUSB */

int lx900_status_read(lx900_status_t *st)
{
    if (st != NULL) {
        memset(st, 0, sizeof(*st));
        st->level[0] = st->level[1] = st->level[2] = st->level[3] = -1;
    }
    return -1;
}

#endif /* HAVE_LIBUSB */
