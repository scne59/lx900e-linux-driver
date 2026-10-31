# Installing the LX900e driver on Linux

Target: any Linux with CUPS 2.4. Developed and tested on Ubuntu 26.04 with
CUPS 2.4.16. The printer is reached through the standard CUPS `usb` backend, so
no kernel driver is involved.

## Prerequisites

```
sudo apt install build-essential libcups2-dev libusb-1.0-0-dev
```

* `libcups2-dev` — to build the filter.
* `libusb-1.0-0-dev` — only for `tools/lx900-send`, the raw USB test tool.
* CUPS must be running (`systemctl status cups`).

## Install

```
cd lx900
sudo ./install.sh
```

`install.sh` does four things:

1. builds `driver/rastertoLX900`,
2. builds the helper tools into `build/`,
3. installs the filter to `/usr/lib/cups/filter/rastertoLX900` and the PPD to
   `/usr/share/cups/model/Primera-LX900e.ppd`,
4. creates or refreshes the queue `LX900e` against the USB device.

Options:

```
./install.sh --no-queue          # build and install files only
./install.sh --uninstall         # remove queue, filter and PPD
./install.sh --dry-run           # show the commands without running them
LX900_QUEUE=MyPrinter ./install.sh
```

### Why root is required

CUPS refuses to execute a filter it does not consider trustworthy:

```
File "/home/you/lx900/driver/rastertoLX900" has insecure permissions (0100755/uid=1000/gid=1000)
```

It wants the file owned by root and not group-writable. A PPD can point at an
absolute path outside `/usr/lib/cups/filter`, but the ownership check still
applies, so there is no way around the install step.

## CUPS 3 and the printer application

CUPS 3 does not use PPDs, so the classic queue above is a CUPS 2 path.  For
CUPS 3, AirPrint, Mopria or any other IPP Everywhere client, build and run the
PAPPL application instead:

```
sudo apt install libpappl-dev ghostscript
make pappl
make detect          # prints the URI, including the serial number
sudo make install-pappl
lx900e-app --device 'usb://Primera/Color_Label_900?serial=00000001' \
    --printer LX900e --port 8000 --spool "$HOME/.local/state/lx900e-app"
```

`make detect` asks PAPPL for the connected printers and prints the URI of the
first LX900e, including its serial number.  `lx900e-app --list-devices` lists
all USB printers.  `make install-pappl` runs the probe itself: if the printer
is connected, the installed systemd unit is bound to that URI; if it is not,
the unit is installed without `--device` and the service discovers the printer
when it starts.  Package builds (`DESTDIR` set) never probe.

Add `ipp://localhost:8000/ipp/print/LX900e` as a network printer in CUPS 3, or
open `http://localhost:8000/` to use the web interface.  Without `--device` the
application discovers USB printers and lets you add one through the web
interface.

Both front ends use the same encoder and the same `/etc/lx900.conf`
registration offsets.  The `lx900-ink-cartridge` print option corresponds to
the PPD's `InkCartridge`.

To run it as a service:

```
sudo systemctl daemon-reload
sudo systemctl enable --now lx900e-app
```

and edit the unit if you want to bind one specific printer (the file has an
example).

## Pick the media size

The queue defaults to `4x3`. Set what you actually have loaded:

```
lpadmin -p LX900e -o PageSize=4x6
```

Available sizes: `2x1 2x2 3x2 3x2.5 3x4 4x2 4x3 4x4 4x6 5x4 5x5 6x2 6x4 8x4 8x6 8x8
100x76mm`, plus `Custom`.

Resolution is fixed at `1200dpi`; that is the native head resolution and the
only value the PPD offers.

### The page size is the label size, not the sheet

This is a label printer: it feeds the length you ask for and prints the width
the head covers. So the page size in the driver must be the **document or label
size**, not the size of the sheet you fed through it. A 100 x 76 mm graphic on
an A4 sheet is a `100x76mm` job - the printer will feed 76 mm of that sheet and
print it 1:1.

There is deliberately no A4 or Letter entry. A4 is 210 mm wide and the head
covers 8.25 in (209.55 mm), so a full A4 page is 0.45 mm too wide; the filter
refuses it rather than sending something the head cannot print. Letter is
wider still. Pick the label size and let the rest of the sheet go unused.

If a print dialog manages to send a page size CUPS cannot parse - some send
`Custom.210,00x297,00mm`, with the decimal comma of their locale - CUPS falls
back to the PPD's default page size, which is `4x3`. The document is then placed
on a 4 x 3 inch page rather than being scaled to nothing. If a job comes out far
too small and rotated, that is what happened: choose the size from the list
instead of typing it into the custom fields.

### Adding your own label size

The cleanest way to print a size that is not in the list is to name it once, so
it appears in the dialog like any other size. Edit `driver/extra-sizes` and add
one size per line, in millimetres or inches:

```
100x76mm
50x80mm
62x100mm
4x6
```

then reinstall:

```
sudo ./install.sh
```

Each line becomes a `PageSize` choice. `100x76mm` is built in; anything else you
add ends up in the same list.

To make one of them the queue default (so plain `lpr` uses it):

```
sudo lpadmin -p LX900e -o PageSize=100x76mm
```

or, for your own jobs only:

```
lpoptions -p LX900e -o PageSize=100x76mm
```

### Why the dialog's custom fields are unreliable

GTK-based dialogs format a custom page size with the locale's decimal
separator. On a German system that produces

```
PageSize=Custom.210,00x297,00mm
```

and CUPS cannot parse the comma, so it silently falls back to the PPD's default
page size. If a document comes out far too small, or rotated and cropped, that
is what happened - check `argv[5]` in `/var/log/cups/error_log`.

The same size written by hand works fine, as long as the decimal point is a
period:

```
lp -d LX900e -o media=Custom.100x76mm label.pdf          # millimetres
lp -d LX900e -o PageSize=Custom.283.46x215.43 label.pdf  # points
```

Both forms are accepted with or without a `pt`/`mm` suffix; only the comma
breaks them.

## Test

```
lpstat -p LX900e -l
lp -d LX900e /path/to/label.pdf
```

Watch a job as it goes:

```
lpstat -W all -o
tail -f /var/log/cups/error_log
```

Print the built-in raster test page without involving CUPS at all:

```
driver/rastertoLX900 < /tmp/page.rast > /tmp/page.lx9
tools/lx900-send -v -d -w 12 /tmp/page.lx9
```

## Ink levels

`install.sh` installs a small backend wrapper, `/usr/lib/cups/backend/lx900`, and
points the queue at it (`lx900:/usb://...`). Before each job it asks the printer
for its status block and publishes the result to CUPS as the standard IPP
marker attributes, then hands the job to the stock `usb` backend. Printing is
unchanged; the wrapper only adds attributes.

The numbers are real measurements, not estimates: the printer keeps a drop
counter per colour, and the driver divides it by the same full-scale values the
vendor software uses.

Where you can see them:

```
build/lx900-send -q                       # cyan=39% magenta=21% yellow=33% black=46%
ipptool -tv ipp://localhost/printers/LX900e \
    /usr/share/cups/ipptool/get-printer-attributes.test | grep marker
```

The CUPS web interface at <http://localhost:631/printers/LX900e> shows the
printer's state, and tools that read `marker-levels` (system-config-printer's
printer properties, most desktop "supplies" applets) show the percentages.
The GTK print dialog itself does not display supplies - that is a limitation of
that dialog, not of the data.

cupsd only accepts these attributes while a job is running, so the levels appear
after the first print and refresh with each one.

To bypass the wrapper (for example while debugging), point the queue back at
the plain USB URI:

```
sudo lpadmin -p LX900e -v "usb://Primera/Color_Label_900?serial=00000001"
```

## Cleaning and purging

```
tools/lx900-purge.sh                  # one page, all four inks in bands
tools/lx900-purge.sh -i m             # one page of solid magenta
tools/lx900-purge.sh -i m -n 3 -w 10  # three pages, ten seconds apart
tools/lx900-purge.sh -i c --check     # purge cyan, then print the nozzle page
```

This is a purge by printing: 100 % coverage of the chosen channel, which is what
pulls ink through a reluctant nozzle. `-i` takes `c`, `m`, `y`, `k` or `all`
(four horizontal bands), `-n` repeats, `-w` sets the dwell between repeats, `-s`
overrides the page size, and `--dry-run` builds the image without printing.
`--check` prints the nozzle-check page afterwards so you can look at the stripe
band for that ink.

It reports what the purge actually cost, using the printer's drop counters:

```
  black: 3929701 drops, 0.12% of the cartridge
```

Percentages alone are too coarse for this - a single page is around 1% of a
colour cartridge, and much less for a small page.

**This is not the printer's maintenance station.** A real head clean wipes and
caps the nozzles and pulls a vacuum; this printer exposes no command for that,
and the vendor's own "Purge" is the same idea as this, a printed flood. So a
purge costs a label and some ink, and works the way printing works.

A sane sequence for a channel that is dropping nozzles:

1. `tools/lx900-purge.sh -i <ink> --check`
2. Look at the stripe band for that ink on the check page. Solid lines mean the
   nozzle fires; gaps mean it is still blocked.
3. If it is still bad, purge again - but stop after two or three. A channel that
   stays blank through repeated floods is not going to be fixed by more ink. On
   this printer magenta behaves exactly like that, and needs a physical
   (ultrasonic) clean of the head, not more printing.

## Aligning the print (registration)

`tools/lx900-align.sh` adjusts where the image lands on the label:

```
tools/lx900-align.sh print
tools/lx900-align.sh set --left -0.5 --top 0.2
tools/lx900-align.sh show
tools/lx900-align.sh reset
```

This is the mechanism the vendor utility uses. Its Align wizard prints a target
and then stores two offsets, `TOFOffset` and `HorzOffset`, which the driver
converts to 1200 dpi and applies as a shift. We apply the same two offsets.

### The target

A registration gauge in the page's **top right corner**, which is the **bottom
left** as you look at the label from the front of the printer:

| line | where | length |
|---|---|---|
| outer horizontal | on the top edge, from the top right corner inwards | 20 mm |
| inner horizontal | 10 mm below the top edge, ending 10 mm in from the right end | 10 mm |
| outer vertical | on the right edge, from the top corner down | 20 mm |
| inner vertical | 10 mm in from the right edge, starting 10 mm down | 10 mm |

The two short lines meet, so each pair forms an **L**: the long one sits on the
hard edge and the page corner, the short one at the 10 mm corner.

Because the label is read rotated 180 degrees, the right edge is the one on your
left and the top edge is the one nearest you. So the gauge appears along the
left edge and along the bottom edge of the label.

The outer line of each pair marks **0 mm**, the inner one **10 mm**. Two checks:

* the outer line should sit on the hard edge - any gap between the edge and the
  line is the offset, in the direction of `--left` or `--top`;
* the spacing between the pair should measure **10 mm**; if it does not, the
  image is scaled, not just shifted, and neither offset will fix it.

A note is printed next to the gauge, in the same corner, rotated so it reads
from the front and in small type (1.5 x 2 mm characters):

```
OUTER LINE = 0MM
GAP TO EDGE = OFFSET
SET --LEFT / --TOP
BY THAT GAP IN MM
LX900-ALIGN.SH
```

It is left out on stock smaller than about 48 x 27 mm, where it would collide
with the gauge.

Because the whole target sits in one corner, it fits in a small page - a check
does not need a full label's worth of A4 or roll stock:

```
tools/lx900-align.sh print --size 50x30
```

The gauge still measures the same thing: the page origin is the corner of the
stock, so 0 mm is the hard edge whatever the page size is.

`set` takes millimetres and writes dots at 1200 dpi to `/etc/lx900.conf`:

```
# /etc/lx900.conf
left=19
top=4
```

The filter reads that file for every job. `LX900_LEFT_OFFSET` and
`LX900_TOP_OFFSET` override it, which is how the test pages are built.

Three limitations worth knowing:

* **`top` can only push the image down.** The offset works by delaying the start
  of the page and dropping the leading rows, so 0 is the minimum; the printer's
  own top margin is the floor. `left` is signed.
* **`PresentOffset`** - the third value the vendor wizard stores - is the
  printer's feed-to-present distance, not something the host applies.
* **Head-to-head alignment** between the colour cartridge and the black one is
  not exposed anywhere we can reach. If black and colour are offset *from each
  other* on the same label, this will move both together and not fix it.

If a change seems to have no effect, check that `print` is building the target
with the filter rather than sending the vendor's pre-rendered
the vendor's `TestOffsets.prn` sample - a pre-built file never passes through the driver,
so no offset can apply to it.

## Printing with no label stock loaded

The printer will happily fire the heads with nothing loaded - ink ends up on the
platen and the label is not printed. It reports the condition in its status
block (`paper` in `lx900-send -q`), so the backend uses it: while the printer
says there is no stock, the job is held instead of printed, and `media-empty` is
raised so the queue shows why. If stock appears within `paper_wait` seconds the
job carries on by itself; otherwise it fails with a clear message and you can
re-submit.

```
# /etc/lx900.conf
paper_check=1      # 0 disables the check
paper_wait=120     # seconds to wait for stock
```

Verify it on your printer: take the stock out and run

```
build/lx900-send -q
```

It should say `paper=NONE`. If it still says `paper=loaded` with the path empty,
this printer's sensor does not track stock in the current mode - set
`paper_check=0` and the check is skipped entirely. The check only applies when
the queue runs through the `lx900:` backend wrapper.

The end-of-roll bit is reported too (`loop sensor: out of stock`) and is treated
the same way, because printing past the end of a roll is the same problem.

## Changing a cartridge

The mechanical part belongs to the printer: the two buttons on top move the
carriage out to the change position, and a button press afterwards completes the
install. The driver's job is the part around it - saying *which* cartridge, and
confirming the printer has accepted the new one:

```
build/lx900-send -c
```

It prints the levels, names the cartridge that reports empty/missing/invalid and
which slot it lives in, walks you through the front-panel steps, then polls the
status block (every two seconds, up to five minutes unless you pass `-T sec`)
and tells you when the printer accepts the cartridge - including whether the
level jumped or the drop counter reset, which is how you know it read the new
one rather than the old chip.

The colour cartridge (slot 1) holds cyan, magenta and yellow together; black is
its own cartridge in slot 2. So a cyan fault and a magenta fault mean the same
physical swap.

Cartridge faults also reach CUPS: the backend reports them as the standard
`marker-supply-empty` / `marker-supply-low` state reasons and puts the slot in
`marker-message`, so the printer's properties dialog can flag it.

**What this does not do** is move the carriage. The vendor utility drives an
undocumented state machine over the print stream to do that (`Change2` to
`Change4`, then wait for idle). Those commands are not in any capture and this
firmware has already wedged more than once on streams we did not understand, so
guessing them is not worth the risk. The buttons already do it.

After a swap, print a diagnostic page to check the new nozzles:

```
lp -d LX900e -o InkCartridge=Color build/colortest-2x1.png
```

## Printer settings that live in NVRAM

Two settings must be right or nothing prints, and both persist across a power
cycle, so usually you only do this once per printer:

* **sensor mode = continuous** (`status[0x5a] = 2`) — otherwise the printer
  waits for die-cut gaps or reflective marks that continuous stock does not
  have,
* **present mode = on** (`status[0x58] = 1`) — otherwise it does not advance to
  the tear-off position.

Check them:

```
tools/lx900-send -q
```

Look at offsets `0x58` and `0x5a` in the 144-byte dump. On a printer that has
never been configured, use the vendor's `ColorLabel900Util` once (a macOS
binary) or send the equivalent config packet before the first job.

## Troubleshooting

**The printer disappears from the bus mid-job.** That is the firmware rejecting
a malformed stream, not a USB problem. It re-enumerates with a new device
number and needs a mains power cycle. Check the transport framing
(`docs/PROTOCOL.md` §2): every frame's length must match what is actually
delivered.

Do not reach for `lx900-send -R` when this happens. A USB bus reset makes this
printer drop off the bus completely and it does **not** come back until the
mains is cycled, which loses you the one diagnostic that still works (the
status reply). Use `lx900-send -q` first; only a mains cycle fixes a wedged
firmware.

**Which ink does the printer think it has?** `lx900-send -q` decodes it:

```
  tanks: cyan=ok magenta=ok yellow=ok black=ok
  present=1 sensor=2  last page 2.00 x 1.00 in  job=17
```

`ok` means the slot is fitted and reports no fault, `MISSING` means the slot is
empty, `INVALID` means the printer does not recognise the cartridge. These are
the printer's own bytes, so they settle the question of whether a black
cartridge is really installed. They do not tell you whether ink flows: a dried
head still reports `ok`.

If a bring-up fails and you want to walk it in a fixed order, run
`tools/lx900-doctor.sh` (see [TEST-PLAN.md](TEST-PLAN.md)).

**Job completes but nothing is printed.** The page was never flushed. The stream
must end with the `05 0e` *and* `05 0b` packets. `lx900-send -d` adds whichever
is missing.

**`lp -n 3` prints only one sheet.** The installed filter predates the copy
handling; reinstall. The driver emits the page `argv[4]` times because CUPS does
not duplicate raster pages itself.

**Black pages do nothing, then the printer stops answering.** Fixed. Page-info
tag `0x01` is the media type code and it has to match the ink in play: `0x0b`
for a CMY-only page, `0x0c` when black takes part. Sending `0x0b` on a page that
carries plane-3 data makes the printer accept the job, print nothing, and stop
answering until the mains is cycled. If you see this, the installed filter is
old:

```
sudo ./install.sh
```

The CMY output is byte-identical either way, so an old filter still prints
colour pages and only misbehaves on black.

**`unsupported raster width` / a page that refuses to print.** The page is
wider than the print head. The head covers 8.25 in, and the filter refuses
anything wider rather than sending it and wedging the printer. A4 and US Letter
are both wider than that, so a document formatted for paper has to be scaled
onto the label:

```
lp -d LX900e -o fit-to-page -o PageSize=4x6 document.pdf
```

or pick a PageSize that matches the stock you have loaded.

**`claim failed` from `lx900-send`.** Another process owns the USB interface,
almost always the CUPS usb backend still finishing - or stuck on - a job.
`lpstat -o` may already look empty while the backend is still alive:

```
ps aux | grep backend/usb
sudo systemctl restart cups
```

This is not a printer fault: the status request never reached the printer.

**The first status request after power-on looks broken.** It returns
`type=0x31 error=0x36` and `INVALID` for all four tanks. That is the firmware
still starting up; the next request returns `type=0x49` and `ok`. Wait for
`0x49` before sending a job.

**The first status request after power-on looks broken.** It returns
`type=0x31 error=0x36` and `INVALID` for all four tanks. That is the firmware
still starting up; the next request returns `type=0x49` and `ok`. Wait for
`0x49` before sending a job.

**Black text prints muddy, or greys come out green.** On the three-plane
setting every grey is a C+M+Y overprint, which turns green as soon as one
colour channel is weak. That is what a dead magenta channel looks like. Print a
diagnostic page (`README.md`, "Judging colour") before blaming the driver.

**"No USB printer is currently visible to CUPS".** Some distributions ship
`/usr/lib/cups/backend/usb` as root-only, so a plain `lpinfo -v` as your user
lists no USB devices at all. Run the installer with `sudo`; it probes with the
privileges the backend needs and, if the probe still comes up empty, keeps the
device URI the queue already has. To check by hand:

```
lsusb | grep 0f25:0022        # is the printer on the bus?
sudo lpinfo -v | grep usb     # does CUPS see a device URI for it?
```

**`lp` says "filter failed".** The filter exited non-zero or was killed. Run it
by hand on a raster file to see the message.

**The wrong device is picked.** `install.sh` prefers a USB device whose URI
mentions Primera/LX900/Color_Label. Override it:

```
lpadmin -p LX900e -v "usb://Primera/Color_Label_900?serial=00000001"
```

**`/dev/usb/lp0` does not exist.** Not needed. `lx900-send -q` detaches the
`usblp` kernel driver and it does not always come back; the CUPS `usb` backend
uses libusb directly and does not care.

**The log says `Unable to encrypt connection: Server credentials not set`.**
A client tried IPPS/HTTPS, but CUPS could not create its self-signed
certificate. CUPS writes it under `$HOME/.cups/ssl`, and the service user must
be able to create that directory. The shipped unit sets
`HOME=/var/lib/lx900e-app` (the systemd state directory) for exactly this
reason; if you run the application by hand as a user without a writable home,
either set `HOME` to a writable directory or pass `--no-tls` and use plain
IPP.
