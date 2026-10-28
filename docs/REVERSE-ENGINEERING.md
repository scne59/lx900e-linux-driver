# LX900e on Linux — reverse-engineering notes

Vendor driver analysed: **ColorLabel900-3.0.dmg** (Primera "Color Label 900" driver 3.0,
2019-10-24), downloaded from
<https://www.primera.com/support/exe/ColorLabel900-3.0.dmg>
sha256 `930be7da2fc5abcfd81e176aa3f74e2a2104e5d9bd0ec2c4e732854cfe7b1836`.

Extraction on Linux, no macOS involved:

```
7z x ColorLabel900-3.0.dmg -odmg            # 7z understands UDZO+HFS
7z x dmg/Primera\ LX900/ColorLabel900Signed.pkg -opkg
# Payload is gzip+cpio
7z x pkg/ColorLabel900Intermediate.pkg/Payload -opayload   # -> Payload~
7z x payload/Payload~ -oroot
```

## What is inside

| Path (under `root/`) | Notes |
|---|---|
| `Library/Printers/PPDs/Contents/Resources/CL900.ppd` | 438 lines, full option set |
| `.../MacOS/rastertoCL900` | 76 KB x86_64 CUPS filter, stripped (only `__mh_execute_header` exported) |
| `.../MacOS/ColorLabel900` | Objective-C bundle: job accounting / status |
| `Lexmark/Frameworks/prdrhpec.framework/.../prdrhpec` | **857 KB, unstripped C++** — Lexmark "Host Print Engine" (HPE), (c) 2003 |
| `.../Resources/Printer.dylib` | 923 KB universal (ppc/i386/x86_64), not linked by the filter |
| `.../Resources/*.lut` | 6 colour tables (HighGloss, HighGloss2, Other, SemiGloss, WhiteMatteBOPP, WhitePolyester) |
| `.../Resources/900.icc` | ColorSync ICC profile |
| `.../Resources/PT_Al900.prn` | 144 B — head-alignment command stream: `ESC * 05 20` carrying sixteen values, all `3` |
| `.../Resources/PT_Cl900.prn` | 6.09 MB — cleaning/colour test page |
| `.../Resources/TestOffsets.prn` | 162 KB — 2400x1800 dots @ 1200 dpi offsets test page |

The engine is a **Lexmark** host-based print engine (`prdrhpec`, "HPE"); the printer has
no page description language. `ColorLabel900Config.plist` exposes tuning values
(`Saturation`, `ImageSharpeningLevel`, `FullSpitsCMY`, `JobMaintenance*`, ...).

## PPD / pipeline

```
*cupsFilter: "application/vnd.cups-raster 100 .../rastertoCL900"
*cupsManualCopies: True
*1284DeviceID: "MFG:Primera Technology Inc.;CMD:none;MDL:Color_Label_900;"
```

* Head resolution is **1200 dpi**. The vendor PPD also offers 600 dpi ("High")
  and 300 dpi ("Low"), but those are downsampled renderings: the band raster is
  always 1200 dpi.
* Page sizes: 2x1 ... 8x8 in; custom 0.75-8.5 in wide, 0.75-18 in long
* Options: `ColorModel`, `MediaType`, `PrintQuality` (Quality1-4), `ColorMatching`,
  `InkCartridge`, `Resolution`
* Colour tables are selected per media type from the `.lut` files.

So the Linux replacement needs exactly one binary: a CUPS raster filter plus the
PPD. The USB layer is standard printer class.

## The device stream

The full wire format — transport framing, prologue, page-info TLVs, band
records, the CRDRE row codec and the end-of-job sequence — is documented in
[`PROTOCOL.md`](PROTOCOL.md). It was recovered from the reference `.prn` files
and validated byte-for-byte against them.

The important correction to earlier notes in this file: `.prn` files are
**transport-framed**. What looked like a stray "20-byte prefix" on some band
records was a frame header:

```
a5 <len:BE16> 00 01 03 <job id:4> 02 <flags> ... 07 08 09 0a 07 08 09 0a
```

and the raster stream is the concatenation of the frame payloads. Once the
frames are stripped, every band record in `TestOffsets.prn` is contiguous and
its `total` field exactly matches the distance to the next record.

Likewise, the "table of u32 values" after the band header is a per-row
population count (set bits per raster line), and the row payloads are CRDRE
compressed, not raw. CRDRE round-trips exactly; the decoder in `tools/` was
checked against all 24 vendor records (240/240 popcounts match on the first
band, and every record walks to its exact declared length).

## Encoder internals (symbols from `prdrhpec`)

The library is **unstripped**, 1670 text symbols. Key pieces:

* C API: `CreateHostPrintEngine`, `HPEInitialize`, `HPEStartDoc`, `HPEStartPage`,
  `HPESendBand`, `HPEEndPage`, `HPEEndDoc`, plus ~50 `Set*`/`Get*` property calls
  (`SetPaperSize`, `SetInputResolution`, `SetMediaType`, `SetHalftoneAlgorithm`,
  `SetHPERasterFormat`, `SetPrintheadCoCoData`, ...).
* Pipeline classes: `RasterExpansion`, `ColorConvert<RGBInputRaster|GRAYInputRaster|RGBMInputRaster>`,
  `ColorTableLUT`/`LUT2`/`LUT3`, `HalftoneCMYK`, `HalftoneCMY_TrueBlue_{2,4,8,16}`,
  `ColorLayer`, `Shingler`/`Shingle33_1`/`Shingle50_2..5`/`Shingle12To48`/`Shingle100`,
  `Printhead`/`ColorPrinthead`/`GenesisColorPrinthead`, `Compression`,
  `DataStreamCompression`, `CRDRECompress`, `QBinaryRaster`, `QContoneRaster`,
  `Decompress`, `BuildSwath`, `BuildPipe`.
* So the format is: **halftone to 1-bit CMY(K), shingle into print-head passes,
  pack into head column order, CRDRE-compress each raster line, then emit
  `ESC #` band records**.

`HostPrintEngine::SendBand` (from the Ghidra decompilation of the vendor engine,
which is not redistributed) is the routine that assembles the 24-byte band header and appends
the popcount table and the row payloads; `LCOM::SWrite::Compress` is CRDRE.

## What is deliberately not implemented

* The vendor's halftone screens (`HalftoneCMY_TrueBlue_*`, `Shingler*`) and its
  colour pipeline. This driver halftones with an 8x8 ordered dither and does
  its own RGB -> CMY[K] conversion; the plane set follows the PPD's
  `InkCartridge` option (CMY only, black only, or CMYK with full under-colour
  removal).
* `.lut` / ICC based colour matching. The vendor applies `900.icc` plus a
  per-media `.lut` before halftoning.
* Multi-pass shingling. The vendor's `Shingle*` classes split a page into
  several interleaved print-head passes; this driver sends one pass per page,
  which is why only the native 1200 dpi resolution is offered.

## Open questions

1. Exact meaning of the page-info tags this driver copies verbatim from the
   vendor (`0x06`..`0x09`, `0x1a`, `0x1e`, `0x22`, `0x24`, `0x26`). `0x01` is
   understood - it is the media type code, and it has to match the ink
   configuration or the printer stalls - and the geometry tags are derived in
   `docs/PROTOCOL.md`. The rest are echoed unchanged, so only the mapped ones
   matter so far.
2. The precise config packet `ColorLabel900Util` sends to set sensor and present
   mode. The effect is known and confirmed on hardware (NVRAM offsets `0x58` and
   `0x5a`), and the values persist, but the byte sequence is not recorded here
   yet.
3. What the sixteen values in the alignment command (`PT_Al900.prn`) select.
   They are all `3` in the vendor sample, so sending the command faithfully is
   possible but the meaning is not known; the driver prints at whatever
   alignment the printer already holds.
4. Whether the printer ever uses `min`/`max` for anything other than internal
   bookkeeping. Empirically it prints correctly when they are truthful, and the
   vendor always sets them to the real ink extent.
