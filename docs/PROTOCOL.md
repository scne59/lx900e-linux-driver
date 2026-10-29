# LX900e wire protocol

Everything below was recovered from the output streams of the vendor's own
driver (its `TestOffsets.prn` and `PT_Cl900.prn` samples, which are not
redistributed here), from a Ghidra decompilation of the vendor's print engine,
and from confirming each step on a physical printer.

The LX900e has no page description language. It is a host-based device: the
driver does colour conversion, halftoning, packing and run-length compression,
then streams pre-rendered raster bands to the printer over USB.

## 1. USB transport

| item | value |
|---|---|
| VID:PID | `0f25:0022` |
| interface | printer class (7), 2 bulk endpoints |
| bulk OUT | `0x04` |
| bulk IN | `0x83` (status back-channel) |

The printer sends status on the bulk-IN endpoint while a job runs. Nobody
*has* to read it, but the CUPS `usb` backend does read it, and the vendor
filter does too. `tools/lx900-send` runs an equivalent reader thread.

Status replies are 144 bytes:

| offset | meaning |
|---|---|
| `0x02` | firmware ASCII (`S305_A041813_35 1.21 04/16/2013`) |
| `0x03` | job counter, increments per accepted job |
| `0x2e` | state: `0x49` = idle/ready, `0x31` = not ready |
| `0x2f` | error: `0x00` = none; bit 0 = loop sensor out of stock (end of roll) |
| `0x38`..`0x3b` | yellow drop counter, little-endian |
| `0x3c`..`0x3f` | magenta drop counter, little-endian |
| `0x40`..`0x43` | cyan drop counter, little-endian |
| `0x44`..`0x47` | black drop counter, little-endian |
| `0x58` | present mode: `1` = present, `0` = don't present |
| `0x6b` | paper status: `0` = no label stock detected, non-zero = stock present |
| `0x5a` | sensor mode: `2` = continuous, `1` = die-cut, `0` = reflective |
| `0x5c`..`0x5d` | last page width accepted, little-endian, 1/100 in |
| `0x5e`..`0x5f` | last page length accepted, little-endian, 1/100 in |
| `0x78` | cyan tank state |
| `0x79` | magenta tank state |
| `0x7a` | yellow tank state |
| `0x7b` | black tank state |

**The first request after a power cycle is not representative**: it returns
`type=0x31`, `error=0x36` and `INVALID` for all four tanks, then settles to
`0x49` / `ok` within a few seconds. A client must poll until `0x49` before
sending a job.

The four counters are drops fired. The vendor driver turns them into
percentages against fixed full scales, and this driver does the same:

```
remaining = 100 * (capacity - counter) / capacity
capacity  = 1,800,000,000 for cyan, magenta and yellow
            3,186,000,000 for black
```

Note the order: the counters sit at 0x38/0x3c/0x40 as **yellow, magenta,
cyan** - the vendor reads them that way round even though it labels them
C/M/Y/K. On a healthy printer the result tracks reality: after repeated
magenta purging that channel reads clearly lower than the others.

A counter larger than its capacity means the table does not apply to that
firmware, so report "unknown" rather than a negative level.

The four tank bytes are the ones the vendor filter reads as C/M/Y/K. `0x0b`
means "present, no fault"; `0x02` means the slot is empty; anything else is a
cartridge the printer does not recognise (wrong type, unreadable chip). Note
that these say nothing about whether ink actually *flows*: a completely dried
or air-locked head still reports `0x0b`.

Present mode and sensor mode live in NVRAM and survive a power cycle. A
brand-new printer must be switched to continuous + present-on once; after that
the driver does not have to re-send it per job. (`ColorLabel900Util` is the
vendor tool that flips these.)

## 2. Transport framing

The byte stream the printer receives is a sequence of frames:

```
a5  <len:BE16>  00 01 03  <job id:4>  02 <flags:1>  <data...>  07 08 09 0a 07 08 09 0a
```

* `len` counts the 9-byte inner header, the data, and the 8-byte trailer:
  `len = 9 + len(data) + 8`.
* `job id` is `07 08 09 0a` in every vendor file. It is not the CUPS job id.
* `flags` is `00` for raster data and `01` for the last raster frame.
* The 8-byte trailer (`07 08 09 0a 07 08 09 0a`) precedes every frame header, so
  a frame boundary looks like 16 bytes of `07 08 09 0a` repeated.

The **raster stream is the concatenation of the data sections**. Frames are
purely a transport detail: a band record may straddle a frame boundary. The
vendor uses 21900-byte data sections (`len = 0x559d`); the sender may use any
size, and this driver uses 21900 as well.

Command frames (end of document) substitute `01` for `02` and carry no
trailer:

```
a5 00 11 00 01 03 <job id:4> 01 00 <cmd:4> <arg:4>
```

### Why this matters

Getting this wrong is fatal, not cosmetic. An earlier version of this driver
copied the vendor's hard-coded `a5 55 9d ...` prologue but never delivered
21917 bytes behind it. The firmware waited for the promised frame, hit the
trailer bytes out of context, and reset — the device dropped off the bus and
re-enumerated with a new device number. If the printer "disconnects" during a
job, check the frame lengths first.

## 3. Raster stream layout

```
prologue                      53 bytes: setup commands + media block
page info                     a5 00 1c 00 01 02 00 + TLVs
band records                  24-byte header + row data, per plane per band
end marker                    1b 23 01 02 00
```

### 3.1 Prologue (53 bytes)

```
1b 2a 05 0c                              ESC * 05 0c
1b 2a 05 0d                              ESC * 05 0d
1b 2a 6d 06 81 1a ff ff ff ff            ESC * 6d 06 + 26-byte payload
   <height:LE16> <width:LE16>            media size in 1/100 inch
   00 80 00 80 00 80 00 80 ff            fixed
   00 00 00 00 00 00 00 00               fixed
   <sum:1>                               byte sum of payload bytes 5..30
1b 2a 6d 05 12 02
1b 2a 6d 05 00 03 03
```

The payload starts at file offset 24. Its byte 31 (file offset 51) is the
checksum: `sum(payload[5..30]) & 0xff`. Verified against the vendor: for
2400x1800 dots the only differences from `TestOffsets.prn` are the width field
and, consequently, the checksum.

### 3.2 Page info

```
1b 23 01 00 00 00 00          marker, total = 0
a5 00 1c 00 01 02 00          packet header
<tag:1> 02 <value:BE16> 00    repeated
```

Tags used by the 1200 dpi driver:

| tag | value |
|---|---|
| `0x01` | media type code, **selected by the cartridge configuration**: `0x0b` for the CMY cartridge alone, `0x0c` when the black cartridge takes part |
| `0x0b` | `height_dots * 300 / yres` |
| `0x12` | `5100 * (yres / 600) - width_dots` |
| `0x14`, `0x16` | `height_dots` |
| `0x15`, `0x17` | `width_dots` |
| `0x2c` | resolution code: 0=300, 1=600, 2=1200, 3=2400 |

At 1200 dpi with a 2400x1800 dot page this packet is **byte-identical** to the
one in `TestOffsets.prn` - apart from tag `0x01`, which is why that tag matters:
it is the only field here that does not fall out of the page geometry.

The vendor engine makes the same connection. `SetHPEProperties` chooses the
media type from the `InkCartridge` byte before calling `SetMediaType()`, and the
two bundled captures differ accordingly: three planes with `0x0b`, four planes
with `0x0c`.

**Getting this wrong hangs the printer.** A page that carries plane-3 data but
announces `0x0b` is accepted over USB, then the printer prints nothing, does not
feed, and stops answering status until the mains is cycled. Verified on
hardware: the same page with only that byte changed (`0x0b` -> `0x0c`) prints.

### 3.3 Band record

Big-endian throughout.

```
+0   1b 23 01 05              record tag
+4   total:BE32               whole record including this 24-byte header
+8   y:BE32                   first raster line of the band, in dots
+12  00 00
+14   rows:BE16               lines in this band (240, or fewer on the last)
+16   width:BE16              dots
+18   min:BE16                first inked dot column
+20   max:BE16                one past the last inked dot column
+22   02                      raster format
+23   plane                   0 = C, 1 = M, 2 = Y, 3 = K
+24   rows x popcount:BE32    set bits per raster line
      rows x (len:BE16 + CRDRE payload)
```

* **Every band of a page must start at plane 0 and run contiguously upwards.**
  A band whose first record is plane 3 is not something the vendor driver ever
  emits, so this driver always fans out from plane 0: a K-only page carries
  three empty `0,1,2` records as well.
* **A page must not start with an empty band.** Every stream that printed had
  a data-bearing record in band 0; the streams that made the printer stop
  answering had an all-skip band 0 with ink in later bands. The vendor stream
  contains no band with data on zero planes at all (41 bands checked), so an
  empty band is a path the firmware is probably never exercised on. This driver
  therefore writes band 0 as real records with zero-length rows when it carries
  no ink. Empty *planes* inside a band that does have ink stay skip records,
  which is what the vendor stream does (`D...`, `.D..`).
* `len == 0` means "this line is identical to the previous line" — the previous
  line is the predictor seed.
* The seed carries across bands *within a plane*; it resets to all zeroes only
  when the plane starts. So plane 0 bands at y=4, 244, 484... chain together.
* `popcount` is the number of set bits in the reconstructed line. The printer
  uses it for ink accounting, and it is a cheap self-check: decode the row,
  count the bits, compare.
* An **empty band** is a 24-byte record with `total = 24` and no body at all;
  the vendor writes `min = max = 0` in that case.

Planes are emitted per band, not per page: C/M/Y for y=4, then C/M/Y for
y=244, and so on.

### 3.4 CRDRE row codec

A row is coded against the previous row (the seed). The stream is a sequence of
operations; each starts with a control byte.

```
control & 0x80  ->  repeat mode
    skip  = (control >> 5) & 3
    count = (control & 0x1f) + 1
    copy `skip` bytes from the seed
    then read (run, value) pairs until `count` bytes have been produced
        run == 0  -> emit one literal `value`
        run != 0  -> emit `value` (run + 1) times
    `count` is a byte count, not a pair count: one pair can cover up to 256
    bytes, so a long run of one value costs a pair per 256 bytes.

control & 0x80 == 0  ->  literal mode
    skip  = (control >> 3) & 0xf
    count = (control & 7) + 1
    copy `skip` bytes from the seed
    copy `count` literal bytes from the stream
```

If `skip`/`count` sit at its maximum, the field continues in following bytes:
`0xff` adds 255 and the first non-`0xff` byte ends the run. This driver always
produced byte-exact payloads that the decoder above reproduces; the extension
encoding is implemented in `enc_row()` in `driver/rastertoLX900.c`.

A fully black 300-byte line (2400 dots) costs 5 bytes; every following
identical line costs 0. That is why an almost-empty 4x3 page is only 14 KB.

## 4. End of job

```
1b 23 01 02 00                                 end-of-raster marker (in the data)
a5 00 19 00 01 03 07 08 09 0a 02 00
   1b 2a 82 00 00 00 00 ac                     "flush page"
   07 08 09 0a 07 08 09 0a
a5 00 11 00 01 03 00 00 00 01 01 00
   1b 2a 05 0e 00 00 00 01                     end page
a5 00 11 00 01 03 00 00 00 01 01 00
   1b 2a 05 0b 00 00 00 01                     "done printing"
```

The vendor `.prn` files stop after `05 0e`. They are the output of the filter,
not of a whole job: `rastertoCL900` writes `05 0e` at end-of-document, and
`05 0b` comes from the Mac print system's job teardown. **The printer needs
`05 0b`**: without it the page is never flushed, the media does not advance, and
subsequent jobs are ignored. `tools/lx900-send -d` appends whichever of the two
packets the stream does not already contain.

## 5. 1200 dpi only

The LX900e print head is a 1200 dpi device. The vendor library exposes 300 and
600 dpi modes, but they are *resampled* renderings; the band raster is always
1200 dpi. This driver only offers 1200 dpi in the PPD and derives every page
parameter from the CUPS raster header, so the geometry in tags `0x0b`..`0x17`
always matches the actual raster.

## 6. Colour

Every raster plane is one ink channel, numbered by the firmware:

| plane | ink |
|---|---|
| 0 | cyan |
| 1 | magenta |
| 2 | yellow |
| 3 | black |

The LX900e takes a CMY cartridge, a black cartridge, or both, so a job carries
three, one or four planes. The page-info packet does not change with the plane
count - the vendor's four-plane `PT_Cl900.prn` utility page and the three-plane
`TestOffsets.prn` have byte-identical page-info bodies - so the firmware works
the plane set out from the band records it receives. `PT_Cl900.prn` lays its
planes out as four consecutive solid blocks in the order C, M, Y, K, which is
how plane 3 was identified as black.

`InkCartridge` in the PPD selects which ink is used. It does **not** change the
record fan-out for any mode that wants black: because a band has to start at
plane 0, `Black` emits empty `0,1,2` records and one K record, not a lone plane
3 record.

| InkCartridge | records | conversion |
|---|---|---|
| `Color` (default) | 0,1,2 | all ink to C/M/Y |
| `Black` | 0,1,2,3 | luma to K, C/M/Y empty |
| `ColorBlack` | 0,1,2,3 | under-colour removal |

The vendor PPD defaults to `ColorBlack`; this driver defaults to `Color`,
which is the configuration with the most hardware proof behind it. The record
layout for black is the vendor's (four planes, K last) and `PT_Cl900.prn`
confirms it; whether the ink itself lands is checked by step 3 of
`docs/TEST-PLAN.md`.

Under-colour removal is full (100% GCR): `k = min(c,m,y)`, then `c,m,y -= k`.
Neutrals therefore print with black ink instead of a C+M+Y overprint, which is
what keeps text crisp and stops greys drifting green when the cartridges are
not perfectly balanced. Saturated colours keep their C/M/Y mix because one of
their channels is already at zero. Black is only ever *split*, never generated
for dark colours, and there is no linearisation.

Two halftone details are not cosmetic.

All planes share the same 1200 dpi grid, so if they also share one threshold
matrix, equal ink demands land on *exactly the same* dots. A 50% grey then
comes out as half the page bare and half the page carrying 300% ink: visible
speckle, flooded stock and muddy midtones. The filter offsets each plane's
threshold by a different part of the 64-level ramp (`plane_phase[]` in
`driver/rastertoLX900.c`), so the patterns interleave. Fully saturated colours
are unaffected, because at 100% demand every plane is above every threshold.

The plate is still binary. There is no dot-size control and no multi-pass
shingling, so a 6% ink request is a sparse field of full-size 1200 dpi dots.

The vendor additionally applies a media-specific LUT (`ClrTbl*.lut`) and the
`900.icc` profile before halftoning. This driver uses neither, so solid colours
are correct but photographs are not matched to the vendor's output.
