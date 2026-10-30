# Tests

```
make test          # or tests/run.sh
```

Builds the driver and tools, then runs every suite that can run without a
printer. Nothing here needs root, a CUPS daemon or a printer.

| suite | covers |
|---|---|
| `test_encoder.py` | The filter, end to end: transport framing, prologue, page info, band records, the CRDRE codec, plane selection per `InkCartridge`, the dither, copies, registration offsets, and the guards (dpi, page width, page height, truncated rasters). |
| `test_ppd.py` | `tools/gen-ppd.py`: required keywords, page geometry, the default-page-first ordering, extra sizes, and `cupstestppd` if it is installed. |
| `unit_status.c` | White-box tests for the status helpers in `driver/lx900-backend.c`: media detection, the config reader, ink percentages and the marker attributes. |
| `test_tools.sh` | The shell tools: syntax, usage text, and that bad arguments are rejected. |
| `pappl-integration.py` | The CUPS 3 printer application, end to end: starts it on a `file:` device, submits PWG raster and PDF jobs through IPP with `ipptool`, and decodes the captured printer stream. Also checks the web pages, the stylesheet, media attributes, the vendor ink option and copies. Skipped without PAPPL, Ghostscript or `ipptool`. |

`stream.py` is an implementation of the stream format written from
`docs/PROTOCOL.md`, not from the C code. The encoder tests decode the filter's
output with it, so a format change has to be made in both places - which is the
point: it catches a change that makes the stream unreadable by anything but the
encoder that wrote it.

Useful environment variables:

* `LX900_FILTER` / `LX900_MKRASTER` - point the encoder suite at different
  binaries, e.g. a sanitizer build (see `.github/workflows/ci.yml`).
* `CC` - compiler to use.

## Not covered here

Anything that needs the printer: whether a page actually comes out, ink flow,
the media sensor, cartridge handling, ink levels as the printer reports them.
Those are in [docs/TEST-PLAN.md](../docs/TEST-PLAN.md), which is a scripted
ladder against real hardware.

## Adding a test

Append a function decorated with `@test` in the relevant `test_*.py`; the
runner discovers them in file order. For the filter, prefer asserting on
decoded output (coverage per plane, extents, framing) rather than on exact
bytes - the tests should survive a change of dither matrix or encoder
heuristic without being rewritten.
