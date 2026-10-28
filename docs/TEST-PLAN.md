# Verification and bring-up

`tools/lx900-doctor.sh` walks this list in order and stops at the first
failure, so a fault is reported at the step that caused it instead of as "it did
not print".

```
tools/lx900-doctor.sh            # interactive: ask after each page
tools/lx900-doctor.sh -n 120     # wait up to two minutes for the printer
tools/lx900-doctor.sh -s 3       # pick up at step 3
```

It builds the test pages from the filter in this tree, records everything the
printer sends back on its bulk-IN endpoint (`build/in-stepN.log`), and asks one
question per page. It never installs anything and never needs root.

## What is verified

| page | tag 0x01 | planes | ink | result |
|---|---|---|---|---|
| `colortest-2x1` (CUPS) | `0x0b` | 3 | C/M/Y | prints |
| magenta purge | `0x0b` | 3 | M only | prints |
| `bartop-Black` | `0x0c` | 4 | K | **prints** |
| `kbar-ColorBlack` | `0x0c` | 4 | K | **prints** |
| `kbar-ColorBlack` with tag `0x0b` | `0x0b` | 4 | K | hangs the printer |

## The one field that is not geometry

Page-info tag `0x01` is the media type code, and the vendor engine chooses it
from the **cartridge configuration**, not just the stock: `SetHPEProperties`
picks a media type from the `InkCartridge` byte before calling `SetMediaType()`.
The two bundled captures agree - `TestOffsets.prn` (three planes) uses `0x0b`,
`PT_Cl900.prn` (four planes) uses `0x0c`.

It is also the only field in the page-info that does not fall out of the page
geometry. Earlier versions of this driver sent `0x0b` for every job, so a page
carrying plane-3 data announced itself as a three-plane page: the printer
accepted the stream, printed nothing, did not feed, and stopped answering status
until the mains was cycled. One byte, `0x0b` -> `0x0c`, fixed it.

Two things are easy to get wrong when touching the encoder:

* the media code has to match the ink that is in play, and
* a band must start at plane 0 - a K-only page still carries empty C/M/Y
  records, because a band whose first record is plane 3 hangs the firmware.

## 0. Is the firmware awake?

```
build/lx900-send -q
```

You want a 144-byte reply, `type=0x49 error=0x00`, and:

```
  tanks: cyan=ok magenta=ok yellow=ok black=ok
  present=1 sensor=2  last page 2.00 x 1.00 in  job=17
```

**The first status request after a power cycle is not representative.** It
returns `type=0x31 error=0x36` with all four tanks `INVALID`, then settles to
`0x49` / `ok` within a few seconds. Wait for `0x49` before sending a job.

This printer has status LEDs only - there is no display to read, so this block
is the machine-readable state.

`claim failed` is a different thing entirely: another process, almost always the
CUPS usb backend finishing or stuck on a job, owns the interface. `lpstat -o`
and `ps aux | grep backend/usb` will show it; `sudo systemctl restart cups`
frees it. It is not a printer fault.

Do **not** run `lx900-send -R`: a USB bus reset drops this printer off the bus
until the mains is cycled, and loses the status reply, which is the only
diagnostic that still works when the firmware is wedged.

The `tanks` line is the printer's own opinion. `ok` means "fitted and no fault
reported"; it does **not** mean ink flows - the magenta channel on this printer
reports `ok` and prints nothing.

The `ink` line is computed from the printer's drop counters (see
[PROTOCOL.md](PROTOCOL.md) §1): percentage of each colour's full scale left.
The same numbers are what the backend publishes to CUPS as `marker-levels`, so
if the two disagree the backend wrapper is not installed.

## 1. Does the CUPS queue reach the printer?

```
lp -d LX900e -o InkCartridge=Color build/colortest-2x1.png
```

The three-plane CMY page. If this does not print, stop: the problem is the
queue, the backend or the printer, not the encoder. Check
`/var/log/cups/error_log` for `PID ... (rastertoLX900) exited` and for the `usb`
backend's own messages.

## 2. Does black ink print?

```
build/lx900-send -d build/bartop-Black.lx9
```

A 1.5 x 0.125 inch black bar near the top of the page: four plane records, ink
only on K, tag `0x01` = `0x0c`. 2.2 kB. This is the combination verified on
hardware.

If it hangs, check that the installed filter is current (`sudo ./install.sh`):
an older build sends `0x0b` here and the printer refuses it.

If the label feeds but stays blank, the stream is fine and the ink path is not.

## 3. Black ink with a blank strip at the top

```
build/lx900-send -d build/kbar-ColorBlack.lx9
```

The same bar moved to the middle, so the first band carries no ink. The filter
writes that band as real records with zero-length rows rather than skip records,
which is the shape the vendor stream always has: `PT_Cl900.prn` has 41 bands and
not one of them is empty on all four planes. 10.9 kB.

This is the ordinary case - a label with a blank top margin.

## 4. Black ink through CUPS

```
lp -d LX900e -o InkCartridge=ColorBlack build/kbar.png
```

Same page through the queue, including the empty first band. If this fails while
step 3 succeeded, the difference is in the CUPS path (raster attributes, job
options, the `usb` backend's timeouts), not the encoder.

## 5. Multiple copies

```
lp -d LX900e -n 3 -o InkCartridge=Color build/colortest-2x1.png
```

The filter repeats the page `argv[4]` times because CUPS does not duplicate
raster pages for drivers that declare `*cupsManualCopies: True`. Three labels
should come out, each with one image.

## Rebuilding a test page by hand

```
build/colortest bar 2400 1200 top > build/bartop.png  # bar in the first band
build/colortest bar 2400 1200     > build/kbar.png    # bar in the middle
PPD=/usr/share/cups/model/Primera-LX900e.ppd \
CONTENT_TYPE=image/png CHARSET=utf-8 \
  /usr/lib/cups/filter/imagetoraster 1 s t 1 \
  "PageSize=2x1 Resolution=1200dpi" build/bartop.png > /tmp/bartop.rast
driver/rastertoLX900 1 s t 1 \
  "Resolution=1200dpi InkCartridge=Black" /tmp/bartop.rast \
  < /tmp/bartop.rast > build/bartop-Black.lx9
```

Use `driver/rastertoLX900`, not the installed copy, or you will test an older
build. The filter reads the raster on **stdin**; the filename argument is
ignored, so feeding it with `<` is required, not a style choice.

`LX900_TAG01=<n>` overrides the media code and `LX900_TOP_OFFSET=<n>` the
raster origin, which is how the `0x0b`/`0x0c` difference was pinned down.

## Reading a failure

| symptom | likely cause |
|---|---|
| `claim failed` | another process owns the USB interface; restart cups |
| `type=0x31 error=0x36`, tanks `INVALID` | normal for the first seconds after power-on; wait for `0x49` |
| `status reply: 0 bytes` | firmware wedged; mains power cycle |
| accepted, no feed, no head movement, then status dies | page-info media code does not match the planes in the page, or a band that does not start at plane 0 |
| page feeds but prints blank | an ink channel, not the encoder |
| `unsupported raster width` | the page is wider than the 8.25 in head; scale it with `-o fit-to-page` |
| `cannot open CUPS raster stream` | the filter reads stdin; feed the `.rast` with `<` |
