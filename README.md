# Primera LX900e driver for Linux

An original, from-scratch CUPS driver for the Primera LX900e colour label
printer, built by reverse-engineering the vendor's macOS driver. No vendor code
or binaries are redistributed here; everything in `driver/` and `tools/` was
written for this project. The vendor's macOS driver was used only as reference
material for the analysis and is not redistributed - see `NOTICE`.

Status: **it prints.** A CUPS raster page goes through
`rastertoLX900` and comes out of the printer, which feeds to the present
position and stays ready for the next job.

## What it does

* Classic CUPS 2.4 driver: one PPD plus one raster filter.
* 1200 dpi, four ink channels (C, M, Y, K), no overcoat. The PPD's
  `InkCartridge` option picks which ink is used: `Color` (CMY), `Black`, or
  `Color + Black` (the default, both).
* Greys and black text print with the black cartridge: full under-colour
  removal turns a neutral into K ink instead of a C+M+Y overprint, which keeps
  text crisp and stops greys drifting green when the colour channels are
  unbalanced.
* Bands always fan out from plane 0, so the black modes still carry three empty
  C/M/Y records per band: the firmware hangs on a band that starts at plane 3.
* Banded raster, CRDRE-compressed, in the printer's native transport framing.
* Multi-copy jobs work: CUPS passes the copy count in argv[4], so the filter
  repeats the finished page itself (`*cupsManualCopies: True`).
* Ink levels. A small backend wrapper (`lx900-backend.c`) reads the printer's
  status block before each job and publishes `marker-levels` and friends to
  CUPS, so IPP clients that show supplies have real numbers.
* Cartridge changes. `lx900-send -c` names the cartridge to replace and watches
  the printer until it accepts the new one; the backend reports cartridge
  faults as `marker-supply-empty` / `marker-supply-low`.
* Purging. `lx900-purge.sh` floods a channel to clear nozzles and reports the
  ink it used, from the printer's drop counters.
* Registration alignment. `lx900-align.sh` prints an alignment target (with the
  measurements printed on it) and stores the TOF/horizontal offsets the filter
  applies.
* Refuses to print with no label stock loaded: the backend holds the job while
  the printer reports no media, instead of firing ink onto the platen.
* All page geometry derived from the CUPS raster header, so any PPD page size
  works up to the print head's 8.25 in width. Wider pages (A4, US Letter) are
  refused with an error rather than sent; use `-o fit-to-page`.

## What it does not do (yet)

* Colour accuracy. RGB -> CMY[K] plus an 8x8 ordered dither, not the vendor's
  ICC profile and per-media `.lut` tables. Solid colours are fine; photographs
  are not. There is no linearisation, so midtones are somewhat dark.
* Job accounting.
* Head-to-head alignment between the colour cartridge and the black head. The
  vendor ships a 144-byte command stream for it (`PT_Al900.prn`): an
  `ESC * 05 20` command carrying sixteen values, all `3` in the sample. What
  those values mean is not known, so the driver does not send it and prints at
  the printer's stored alignment. Registration - where the image lands on the
  label - is handled by `tools/lx900-align.sh`.

## Quick start

```
sudo apt install build-essential libcups2-dev
sudo ./install.sh
lpadmin -p LX900e -o PageSize=4x6      # match your loaded stock
lp -d LX900e label.pdf
```

Details, including why `sudo` is unavoidable, are in [docs/INSTALL.md](docs/INSTALL.md).

## Documentation

| file | contents |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | prerequisites, install, media sizes, troubleshooting |
| [docs/TEST-PLAN.md](docs/TEST-PLAN.md) | the order in which to prove the printer, the black head and four-plane pages |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | the printer's wire format: USB framing, band records, CRDRE, end-of-job |
| [docs/REVERSE-ENGINEERING.md](docs/REVERSE-ENGINEERING.md) | how the vendor driver was analysed, what is still unknown |

## Layout

```
driver/rastertoLX900.c    the filter (CUPS raster in, printer stream out)
driver/CL900-linux.ppd    the PPD
driver/Makefile
driver/lx900-backend.c    CUPS backend wrapper: ink levels, then the usb backend
tools/lx900-send.c        raw USB sender / status probe, for bring-up
tools/lx900-doctor.sh     walk the bring-up ladder in docs/TEST-PLAN.md
tools/lx900-purge.sh      purge / prime a channel by printing a flood
tools/lx900-align.sh      registration: print the target, set TOF/horizontal
tools/mkraster.c          emit a test CUPS raster page
tools/colortest.c         emit the colour / nozzle diagnostic page as a PNG
tools/gen-ppd.py          generate the PPD
install.sh                build, install, create the queue
```

## Testing without a printer in the loop

```
tools/lx900-send -q                 # status block, tank states and ink levels
tools/lx900-send -c                 # guided cartridge change
tools/lx900-send -v -d file.prn     # send a raw stream, add missing end-of-job packets
tools/lx900-doctor.sh -n 120           # walk the bring-up ladder from docs/TEST-PLAN.md
```

`-q` also decodes the four tank states, so it answers "is a black cartridge
actually fitted?" without guessing.

`-d` appends the `05 0e` and `05 0b` packets only if the stream does not already
end with them; sending `05 0e` twice makes the printer advance the media twice.

## Judging colour

`tools/colortest.c` writes a 2 x 1 inch diagnostic page as a PNG, with no
dependency on ImageMagick or any other imaging library:

```
build/colortest 2400 1200 > colortest.png
lp -d LX900e -o PageSize=2x1 colortest.png
```

It contains six full-strength patches (C, M, Y, R, G, B), an 11-step grey
wedge, one single-dot stripe band per ink (C, M, Y, K) and a flat 50% field. How
to read it:

| symptom | cause |
|---|---|
| pale or missing lines inside a stripe band | blocked nozzle in that ink |
| a stripe band completely blank | that channel is not firing at all |
| streaks or mottle in the flat 50% field | feed or head problem |
| patches print but hues are off | driver colour transform |

The stripe bands are the quickest way to tell a dirty head from a bad driver:
they are single-nozzle-wide, so one blocked nozzle shows up as an obvious pale
line, while a colour-transform error leaves every stripe intact and only shifts
the hue.

If you still have a Windows or macOS machine with the vendor driver, printing
the same PNG there and comparing is the definitive test.

### A dead channel looks like a colour bug

The failure modes are easy to confuse. If magenta stops firing, the magenta and
red patches fade, blue loses its red component and starts to look like cyan,
and the magenta stripe band goes blank. With the default `InkCartridge
ColorBlack` the grey wedge stays neutral, because neutrals are printed with
black ink; with `InkCartridge Color` the same fault turns the whole wedge
**green** (grey is C+M+Y, and without magenta only C+Y is left). Two symptoms,
one dead ink.

Give the tool an RGB triple and it fills the page with that colour instead of
drawing the test layout, which makes a purge sheet:

```
build/colortest 4800 3600 255 0 255 > magenta-purge-4x3.png   # solid magenta
lp -d LX900e -o PageSize=4x3 magenta-purge-4x3.png
```

Print it two or three times, then reprint the diagnostic page and look at the
magenta stripe band. Lines that fill in mean the head is recovering; a band that
stays blank means the channel is not being driven at all and no amount of driver
work will help.

### Choosing the ink configuration

The default is `InkCartridge ColorBlack`: both cartridges, greys and text in K.

```
lpadmin -p LX900e -o InkCartridge=ColorBlack   # both cartridges (default)
lpadmin -p LX900e -o InkCartridge=Color        # CMY cartridge only
lpadmin -p LX900e -o InkCartridge=Black        # black cartridge only
```

`Black` and `ColorBlack` both emit four plane records and put ink only where
the mode says; `Color` emits three. All three are verified on hardware.

`tools/lx900-send -q` prints the printer's own view of the four slots:

```
  tanks: cyan=ok magenta=ok yellow=ok black=ok
```

`ok` means the slot is fitted and reports no fault. It does not mean ink flows -
a dried head still reports `ok`, which is what a dead magenta channel looks
like. If a whole channel is missing from a print, see "A dead channel looks like
a colour bug" above.

## Requirements

`build-essential`, `libcups2-dev`, optionally `libusb-1.0-0-dev` (needed by
`lx900-send` and by the ink-level backend; without it the queue prints but
reports no levels). The printer is a 1200 dpi USB device; nothing else is
required.

## Licence

MIT - see `LICENSE`. 

`NOTICE` records where the protocol knowledge came from and lists the vendor
material that is deliberately **not** part of this tree.
