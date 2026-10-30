/*
 * SPDX-License-Identifier: MIT
 *
 * Status queries for the LX900e, used by the printer application.
 *
 * The CUPS 2 filter path gets the same information from driver/lx900-backend.c,
 * which wraps the stock usb backend.  A printer application has no backend to
 * wrap, so it asks the printer directly over libusb.  Both paths use the same
 * request and the same status layout (docs/PROTOCOL.md section 5).
 *
 * The device is opened for the duration of one query and closed again, so a
 * caller must only use this while the printer is idle.
 */
#ifndef LX900_STATUS_H
#define LX900_STATUS_H

#define LX900_STATUS_LEN 144

typedef struct {
    unsigned char raw[LX900_STATUS_LEN];

    /* One entry per tank: 0 = cyan, 1 = magenta, 2 = yellow, 3 = black. */
    int level[4];               /* 0..100, -1 unknown, -2 not fitted */
    int media_present;          /* label stock at the sensor */
    int out_of_stock;           /* loop sensor says the roll is finished */
    int state;                  /* raw state byte */
    int error;                  /* raw error byte */
} lx900_status_t;

/*
 * Ask the printer for a status block.  Returns 0 on success, -1 if the device
 * is not present, busy, or does not answer.  On failure `st` is zeroed and
 * every level is set to -1.
 */
int lx900_status_read(lx900_status_t *st);

#endif /* LX900_STATUS_H */
