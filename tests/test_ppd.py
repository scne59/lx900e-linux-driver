#!/usr/bin/env python3
"""Tests for tools/gen-ppd.py."""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GEN = os.path.join(ROOT, "tools", "gen-ppd.py")

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


def generate(extra_text=None):
    """Run the generator, optionally with an extra-sizes file."""
    tmpdir = tempfile.mkdtemp(prefix="lx900-ppd-")
    out = os.path.join(tmpdir, "test.ppd")
    cmd = [sys.executable, GEN, "-o", out]
    if extra_text is not None:
        extra = os.path.join(tmpdir, "extra-sizes")
        with open(extra, "w", encoding="utf-8") as f:
            f.write(extra_text)
        cmd.append(extra)
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True)
    if proc.returncode != 0:
        raise AssertionError("generator failed: %s"
                             % proc.stderr.decode("utf-8", "replace"))
    with open(out, encoding="utf-8") as f:
        return f.read()


def page_choices(text):
    return re.findall(r"^\*PageSize ([^/]+)/", text, re.M)


@test
def test_required_keywords():
    text = generate()
    for key in ("*PPD-Adobe:", "*Manufacturer:", "*ModelName:", "*NickName:",
                "*Product:", "*cupsFilter:", "*cupsColorSpace:", "*DefaultPageSize:",
                "*DefaultResolution:", "*DefaultInkCartridge:", "*OpenUI *PageSize",
                "*CloseUI: *PageSize", "*ImageableArea", "*PaperDimension",
                "*cupsManualCopies:"):
        if key not in text:
            raise AssertionError("missing %s" % key)
    if "*cupsFilter:" not in text or "rastertoLX900" not in text:
        raise AssertionError("cupsFilter does not point at the filter")
    if "*DefaultResolution: 1200dpi" not in text:
        raise AssertionError("default resolution is not 1200dpi")
    if "*DefaultInkCartridge: ColorBlack" not in text:
        raise AssertionError("default ink cartridge is not ColorBlack")


@test
def test_page_geometry():
    text = generate()
    want = {
        "2x1": '144 72', "4x3": '288 216', "8x8": '576 576',
        "100x76mm": '283.46 215.43',
    }
    for name, dims in want.items():
        pat = r'^\*PaperDimension %s/[^:]*: "%s"$' % (re.escape(name), re.escape(dims))
        if not re.search(pat, text, re.M):
            raise AssertionError("no PaperDimension %s = %s" % (name, dims))
        pat = r'^\*ImageableArea %s/[^:]*: "0 0 %s"$' % (re.escape(name), re.escape(dims))
        if not re.search(pat, text, re.M):
            raise AssertionError("no ImageableArea %s" % name)


@test
def test_default_page_is_first_choice():
    """CUPS falls back to the first *PageSize when a requested one is unparsable."""
    text = generate()
    m = re.search(r"^\*DefaultPageSize: (\S+)$", text, re.M)
    if not m:
        raise AssertionError("no DefaultPageSize")
    first = page_choices(text)[0]
    if first != m.group(1):
        raise AssertionError("first page choice is %r, default is %r"
                             % (first, m.group(1)))


@test
def test_extra_sizes():
    text = generate("50x30mm\n# a comment\n4x6\nrubbish\n")
    choices = page_choices(text)
    if "50x30mm" not in choices:
        raise AssertionError("50x30mm was not added")
    if choices.count("4x6") != 1:
        raise AssertionError("4x6 is duplicated")
    if not re.search(r'^\*PaperDimension 50x30mm/[^:]*: "141\.73 85\.04"$',
                     text, re.M):
        raise AssertionError("50x30mm has the wrong dimensions")
    if "rubbish" in text:
        raise AssertionError("an unreadable size leaked into the PPD")


@test
def test_extra_size_out_of_range_is_skipped():
    text = generate("500x500mm\n")
    if "500x500mm" in text:
        raise AssertionError("an oversized size was accepted")


@test
def test_no_vendor_text():
    """The PPD must not carry text from the vendor's own PPD."""
    text = generate()
    for bad in ("Apple", "All Rights Reserved", "ColorSync", "2008-2009"):
        if bad in text:
            raise AssertionError("PPD contains vendor text %r" % bad)


@test
def test_app_media_table_matches_the_ppd():
    """The printer application carries its own copy of the size table."""
    app = os.path.join(ROOT, "pappl", "lx900-app.c")
    if not os.path.exists(app):
        print("skip (no printer application in this tree)")
        return

    # Import the generator so the comparison uses its table, not a copy.
    import importlib.util
    spec = importlib.util.spec_from_file_location("gen_ppd", GEN)
    gen = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gen)

    with open(app, encoding="utf-8") as f:
        text = f.read()
    block = text.split("static const media_size_t media_sizes[] = {", 1)[1]
    block = block.split("};", 1)[0]
    got = [(name, int(w), int(l)) for name, w, l in
           re.findall(r'\{\s*"([^"]+)",\s*(\d+),\s*(\d+)\s*\}', block)]

    want = [(name, round(w * 2540.0 / 72.0), round(h * 2540.0 / 72.0))
            for name, w, h, _label in gen.SIZES]
    if sorted(got) != sorted(want):
        raise AssertionError("application media table differs from gen-ppd.py:\n"
                             "  app: %s\n  ppd: %s" % (got, want))
    if got[0][0] != gen.DEFAULT_PAGE:
        raise AssertionError("application default size is %r, want %r"
                             % (got[0][0], gen.DEFAULT_PAGE))


@test
def test_ppd_compiles():
    cupstestppd = None
    for path in ("/usr/bin/cupstestppd", "/usr/sbin/cupstestppd"):
        if os.path.exists(path):
            cupstestppd = path
            break
    if not cupstestppd:
        print("skip cupstestppd (not installed)")
        return
    tmpdir = tempfile.mkdtemp(prefix="lx900-ppd-")
    out = os.path.join(tmpdir, "test.ppd")
    subprocess.run([sys.executable, GEN, "-o", out], cwd=ROOT, check=True,
                   capture_output=True)
    proc = subprocess.run([cupstestppd, "-q", out], capture_output=True)
    if proc.returncode != 0:
        raise AssertionError("cupstestppd rejected the PPD:\n%s"
                             % proc.stdout.decode("utf-8", "replace"))


def main():
    failed = 0
    for fn in TESTS:
        try:
            fn()
        except AssertionError as e:
            failed += 1
            print("FAIL %s: %s" % (fn.__name__, e))
        else:
            if "-v" in sys.argv[1:]:
                print("ok   %s" % fn.__name__)
    print("%d/%d PPD tests passed" % (len(TESTS) - failed, len(TESTS)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
