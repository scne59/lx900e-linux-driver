#!/usr/bin/env python3
"""Behavioural tests for driver/rastertoLX900.c.

Runs the filter on synthetic CUPS rasters and inspects what it emits, using the
independent decoder in tests/stream.py.  No printer, no root, no CUPS daemon.

    python3 tests/test_encoder.py [-v]

Everything here is deterministic: the registration offsets are pinned through
the environment so a /etc/lx900.conf on the build machine cannot change the
expected output.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import stream as S  # noqa: E402

# Overridable so CI can point these at a sanitizer build.
FILTER = os.environ.get("LX900_FILTER") or os.path.join(ROOT, "driver", "rastertoLX900")
MKRASTER = os.environ.get("LX900_MKRASTER") or os.path.join(ROOT, "build", "mkraster")

VERBOSE = "-v" in sys.argv[1:]

# The filter reads /etc/lx900.conf unless the environment overrides it.
BASE_ENV = dict(os.environ, LX900_LEFT_OFFSET="0", LX900_TOP_OFFSET="4")

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


# ---------------------------------------------------------------- helpers

def raster(w, h, dpi=1200, fill=None, gray=None, rect=None):
    args = [MKRASTER, str(w), str(h), str(dpi)]
    if fill is not None:
        args += ["fill"] + [str(v) for v in fill]
    elif gray is not None:
        args += ["gray", str(gray)]
    elif rect is not None:
        args += ["rect"] + [str(v) for v in rect]
    else:
        args += ["blank"]
    out = subprocess.run(args, check=True, capture_output=True).stdout
    if not out:
        raise AssertionError("mkraster produced nothing")
    return out


def encode(rast, mode="ColorBlack", copies=1, env=None):
    e = dict(BASE_ENV)
    if env:
        e.update(env)
    opts = "Resolution=1200dpi"
    if mode is not None:
        opts += " InkCartridge=" + mode
    return subprocess.run(
        [FILTER, "1", "user", "title", str(copies), opts, "-"],
        input=rast, capture_output=True, env=e)


def stream(rast, **kw):
    proc = encode(rast, **kw)
    if proc.returncode != 0:
        raise AssertionError("filter failed: %s" %
                             proc.stderr.decode("utf-8", "replace"))
    return S.Stream(proc.stdout)


def approx(value, want, tol, what):
    if abs(value - want) > tol:
        raise AssertionError("%s: got %.3f, want %.3f +/- %.3f" %
                             (what, value, want, tol))


def coverage(st, plane):
    return st.coverage(plane)


def planes_present(st):
    return sorted({r.plane for r in st.records})


# ---------------------------------------------------------------- framing

@test
def test_transport_framing():
    """Every frame is well formed and the payload reassembles cleanly."""
    st = stream(raster(2400, 1200, fill=(0, 0, 0)), mode="Color")
    if not st.data.startswith(b"\x1b\x2a\x05\x0c"):
        raise AssertionError("stream does not start with the prologue")
    if st.page_info_offset < 40:
        raise AssertionError("page info before the prologue finished")
    # strip_frames() raises on anything malformed, so reaching here is the test
    if len(st.data) >= len(st.raw):
        raise AssertionError("framing did not add any bytes")


@test
def test_prologue_fields():
    w, h, res = 2400, 1200, 1200
    st = stream(raster(w, h, fill=(0, 0, 0)), mode="Color")
    p = st.prologue
    if len(p) != 53:
        raise AssertionError("prologue is %d bytes, want 53" % len(p))
    if p[0:8] != bytes.fromhex("1b2a050c1b2a050d"):
        raise AssertionError("prologue prefix is %s" % p[0:8].hex())
    height = int.from_bytes(p[18:20], "little")
    width = int.from_bytes(p[20:22], "little")
    if height != h * 100 // res:
        raise AssertionError("prologue height %d, want %d" % (height, h * 100 // res))
    if width != w * 100 // res:
        raise AssertionError("prologue width %d, want %d" % (width, w * 100 // res))
    if p[39] != (sum(p[13:38]) & 0xFF):
        raise AssertionError("prologue checksum does not add up")
    if p[40:53] != bytes.fromhex("1b2a6d0512021b2a6d050003 03".replace(" ", "")):
        raise AssertionError("prologue tail is %s" % p[40:53].hex())


@test
def test_page_info():
    w, h = 2400, 1200
    st = stream(raster(w, h, fill=(0, 0, 0)), mode="Color")
    t = st.tags
    expect = {
        0x09: 2550,
        0x0B: h * 300 // 1200,
        0x12: 5100 * (1200 // 600) - w,
        0x14: h,
        0x15: w,
        0x16: h,
        0x17: w,
        0x24: 240,
    }
    for tag, want in expect.items():
        got = t.get(tag)
        if got != want:
            raise AssertionError("tag 0x%02x is %s, want %s" % (tag, got, want))
    if st.res_code != 2:
        raise AssertionError("resolution code %d, want 2 (1200 dpi)" % st.res_code)


@test
def test_record_structure():
    """Headers, length tables, popcounts and plane order all agree."""
    st = stream(raster(2400, 1200, gray=128))
    if not st.records:
        raise AssertionError("no records")
    for rec in st.records:
        if rec.b12 != 0 or rec.fmt != 2:
            raise AssertionError("%r: b12=%d fmt=%d" % (rec, rec.b12, rec.fmt))
        if rec.skipped:
            if rec.total != 24:
                raise AssertionError("%r: skip record is %d bytes" % (rec, rec.total))
            continue
        want = 24 + 4 * rec.rows + sum(2 + n for n in rec.lens)
        if rec.total != want:
            raise AssertionError("%r: total %d, fields say %d"
                                 % (rec, rec.total, want))
        if len(rec.counts) != rec.rows:
            raise AssertionError("%r: %d popcounts for %d rows"
                                 % (rec, len(rec.counts), rec.rows))
        if rec.minb > rec.maxb:
            raise AssertionError("%r: extent %d..%d" % (rec, rec.minb, rec.maxb))
    # every band carries a contiguous run of planes starting at 0
    for band in st.bands():
        got = [r.plane for r in band]
        if got != list(range(len(got))):
            raise AssertionError("band at y=%d has planes %s" % (band[0].y, got))


@test
def test_popcounts_match_decoded_rows():
    """The declared popcount of every row equals the decoded row's weight."""
    st = stream(raster(2400, 1200, gray=128))
    wbytes = (st.width + 7) // 8
    seed = {}
    for rec in st.records:
        if rec.skipped:
            seed[rec.plane] = bytes(wbytes)
            continue
        s = seed.get(rec.plane, bytes(wbytes))
        for i, payload in enumerate(rec.payload):
            row, used = S.decode_row(payload, wbytes, s)
            if used != len(payload):
                raise AssertionError("%r row %d: %d of %d bytes used"
                                     % (rec, i, used, len(payload)))
            weight = sum(bin(b).count("1") for b in row)
            if weight != rec.counts[i]:
                raise AssertionError("%r row %d: decoded %d dots, declared %d"
                                     % (rec, i, weight, rec.counts[i]))
            s = row
        seed[rec.plane] = s


@test
def test_end_of_page_and_job():
    st = stream(raster(2400, 1200, fill=(0, 0, 0)))
    if st.end_page_count != 1:
        raise AssertionError("%d end-of-page records, want 1" % st.end_page_count)
    if not st.has_eod_0b or not st.has_eod_0e:
        raise AssertionError("end-of-document packets missing")


# ---------------------------------------------------------------- content

@test
def test_white_page_has_no_ink():
    st = stream(raster(2400, 1200))
    if st.plane_count != 4:
        raise AssertionError("%d planes, want 4" % st.plane_count)
    for pl in range(4):
        if coverage(st, pl) != 0.0:
            raise AssertionError("plane %d has ink on a white page" % pl)
    # band 0 is written as real records even when empty (holds the plane set)
    first = st.bands()[0]
    if any(r.skipped for r in first):
        raise AssertionError("band 0 should not be a skip record")


@test
def test_black_page_uses_only_k():
    st = stream(raster(2400, 1200, fill=(0, 0, 0)))
    approx(coverage(st, 3), 1.0, 1e-9, "K coverage")
    for pl in (0, 1, 2):
        if coverage(st, pl) != 0.0:
            raise AssertionError("plane %d inked for pure black" % pl)


@test
def test_solid_colours():
    cases = {
        (0, 255, 255): (1.0, 0.0, 0.0),      # cyan
        (255, 0, 255): (0.0, 1.0, 0.0),      # magenta
        (255, 255, 0): (0.0, 0.0, 1.0),      # yellow
        (255, 0, 0): (0.0, 1.0, 1.0),        # red
        (0, 255, 0): (1.0, 0.0, 1.0),        # green: C and Y inked, not M
        (0, 0, 255): (1.0, 1.0, 0.0),        # blue
    }
    for rgb, want in cases.items():
        st = stream(raster(2400, 1200, fill=rgb))
        for pl, expect in zip(range(3), want):
            approx(coverage(st, pl), expect, 1e-9,
                   "plane %d of %s" % (pl, rgb))
        if coverage(st, 3) != 0.0:
            raise AssertionError("K inked for %s (min(c,m,y)=0)" % (rgb,))


@test
def test_grey_is_dithered_evenly():
    st = stream(raster(2400, 1200, gray=128))
    # 127/255 ink on K after under-colour removal: ~half the dots
    approx(coverage(st, 3), 0.5, 0.08, "K coverage of 50% grey")
    for pl in (0, 1, 2):
        if coverage(st, pl) != 0.0:
            raise AssertionError("plane %d inked on a neutral grey" % pl)
    # and in CMY mode all three planes carry it
    st = stream(raster(2400, 1200, gray=128), mode="Color")
    for pl in range(3):
        approx(coverage(st, pl), 0.5, 0.08, "plane %d of grey in Color mode" % pl)


@test
def test_mode_plane_sets():
    for mode, want_planes, want_tag in (
        ("Color", 3, 0x0B),
        ("Black", 4, 0x0C),
        ("ColorBlack", 4, 0x0C),
        (None, 4, 0x0C),          # unset = PPD default
    ):
        st = stream(raster(2400, 1200, fill=(0, 0, 0)), mode=mode)
        if st.plane_count != want_planes:
            raise AssertionError("%s: %d planes, want %d"
                                 % (mode, st.plane_count, want_planes))
        if st.tags[0x01] != want_tag:
            raise AssertionError("%s: tag 0x01 = 0x%02x, want 0x%02x"
                                 % (mode, st.tags[0x01], want_tag))
        got = planes_present(st)
        if got != list(range(want_planes)):
            raise AssertionError("%s: planes %s" % (mode, got))


@test
def test_copies_repeat_the_page():
    r = raster(2400, 1200, fill=(0, 0, 0))
    one = encode(r, copies=1).stdout
    three = encode(r, copies=3).stdout
    if len(three) != 3 * len(one):
        raise AssertionError("3 copies is %d bytes, 1 copy is %d"
                             % (len(three), len(one)))
    if three != one * 3:
        raise AssertionError("copies are not identical pages")
    if three.count(S.END_PAGE) != 3:
        raise AssertionError("%d end-of-page records for 3 copies"
                             % three.count(S.END_PAGE))


@test
def test_left_offset_moves_the_image():
    r = raster(2400, 1200, fill=(0, 0, 0))
    for off, want_min, want_max in ((0, 0, 2400), (24, 24, 2400), (-24, 0, 2376)):
        st = stream(r, env={"LX900_LEFT_OFFSET": str(off)})
        rec = next(r for r in st.records if not r.skipped)
        if (rec.minb, rec.maxb) != (want_min, want_max):
            raise AssertionError("left %d: extent %d..%d, want %d..%d"
                                 % (off, rec.minb, rec.maxb, want_min, want_max))


@test
def test_top_offset_delays_the_page():
    r = raster(2400, 1200, fill=(0, 0, 0))
    for off in (0, 4, 100):
        st = stream(r, env={"LX900_TOP_OFFSET": str(off)})
        if st.records[0].y != off:
            raise AssertionError("top %d: first record at y=%d"
                                 % (off, st.records[0].y))
        last = st.records[-1]
        if last.y + last.rows > 1200 + off:
            raise AssertionError("top %d: page overruns" % off)


# ---------------------------------------------------------------- codec

@test
def test_codec_long_skips_and_runs():
    """A wide page with a long identical prefix exercises the codec extensions."""
    w, h = 9900, 240
    st = stream(raster(w, h, rect=(6000, 50, 6100, 150)))
    rows = dict(st.bitmap(3))
    if not rows:
        raise AssertionError("K plane is empty")
    for y, bits in rows.items():
        for x, on in enumerate(bits):
            want = 1 if (6000 <= x < 6100 and 54 <= y < 154) else 0
            if on != want:
                raise AssertionError("pixel (%d,%d) is %d, want %d"
                                     % (x, y, on, want))


@test
def test_codec_solid_wide_page():
    """A wide solid row needs multi-pair runs; it must decode to all ones."""
    w, h = 9900, 240
    st = stream(raster(w, h, fill=(0, 0, 0)))
    rows = dict(st.bitmap(3))
    for y, bits in rows.items():
        if any(b == 0 for b in bits):
            raise AssertionError("row %d is not solid" % y)
    approx(st.coverage(3), 1.0, 1e-9, "solid K coverage")


# ---------------------------------------------------------------- guards

def expect_failure(rast, needle, mode="ColorBlack"):
    proc = encode(rast, mode=mode)
    if proc.returncode == 0:
        raise AssertionError("filter accepted input it should refuse")
    err = proc.stderr.decode("utf-8", "replace")
    if needle not in err:
        raise AssertionError("error message %r does not mention %r" % (err, needle))
    return err


@test
def test_refuses_non_1200dpi():
    expect_failure(raster(1200, 600, dpi=600), "1200")


@test
def test_refuses_pages_wider_than_the_head():
    expect_failure(raster(10000, 240), "cannot be printed")


@test
def test_refuses_an_overlong_page():
    """The copy buffer grows with the page, so cap the height like the PPD does."""
    expect_failure(raster(64, 21601), "tops out")


@test
def test_refuses_short_raster():
    r = raster(2400, 1200, fill=(0, 0, 0))
    expect_failure(r[:len(r) // 2], "short raster")


# ---------------------------------------------------------------- runner

def main():
    missing = [p for p in (FILTER, MKRASTER) if not os.path.exists(p)]
    if missing:
        print("missing build products: %s" % ", ".join(missing), file=sys.stderr)
        print("run tests/run.sh (it builds them)", file=sys.stderr)
        return 2

    failed = 0
    for fn in TESTS:
        name = fn.__name__
        try:
            fn()
        except AssertionError as e:
            failed += 1
            print("FAIL %s: %s" % (name, e))
        except S.StreamError as e:
            failed += 1
            print("FAIL %s: malformed stream: %s" % (name, e))
        else:
            if VERBOSE:
                print("ok   %s" % name)
    print("%d/%d encoder tests passed" % (len(TESTS) - failed, len(TESTS)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
