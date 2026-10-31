#!/usr/bin/env python3
"""End-to-end test for the LX900e printer application.

Starts the real application against a ``file:`` device, submits jobs through
IPP with ipptool, and decodes the captured printer stream with the independent
decoder in tests/stream.py.  No printer and no root are needed.

    python3 tests/pappl-integration.py [-v]

The test is skipped when the application, ipptool, Ghostscript or ps2pdf are
not available.
"""

import os
import pathlib
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
APP = os.environ.get("LX900_APP", str(ROOT / "build" / "lx900e-app"))
MKRASTER = os.environ.get("LX900_MKRASTER", str(ROOT / "build" / "mkraster"))

sys.path.insert(0, str(HERE))
import stream as S  # noqa: E402

VERBOSE = "-v" in sys.argv[1:]


def skip(reason):
    print("pappl-integration: skipping (%s)" % reason)
    raise SystemExit(0)


if not pathlib.Path(APP).is_file():
    skip("lx900e-app is not built")
for tool in ("ipptool", "gs", "ps2pdf"):
    if shutil.which(tool) is None:
        skip("%s is not installed" % tool)

PORT = None
CAPTURE = None
SPOOL = None
LOG = None
OUT = None
PROCESS = None


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_for_port(port, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return
        except OSError:
            time.sleep(0.1)
    raise AssertionError("lx900e-app did not listen on port %d" % port)


def start_app(tmp):
    global PORT, CAPTURE, SPOOL, LOG, OUT, PROCESS

    PORT = free_port()
    CAPTURE = tmp / "capture.prn"
    SPOOL = tmp / "spool"
    LOG = tmp / "app.log"
    OUT = tmp / "app.out"
    SPOOL.mkdir()

    PROCESS = subprocess.Popen(
        [
            APP,
            "--device", "file:%s" % CAPTURE,
            "--printer", "LX900e",
            "--port", str(PORT),
            "--spool", str(SPOOL),
            "--log", str(LOG),
        ],
        stdout=OUT.open("wb"),
        stderr=subprocess.STDOUT,
    )
    wait_for_port(PORT)


def stop_app():
    if PROCESS is not None:
        PROCESS.terminate()
        try:
            PROCESS.wait(timeout=5)
        except subprocess.TimeoutExpired:
            PROCESS.kill()
            PROCESS.wait(timeout=5)


def ipptool_job(tmp, name, filename, document_format, extra="",
                media="custom_4x3in_4x3in"):
    test = tmp / "test.ipp"
    test.write_text(
        "{\n"
        '  NAME "%s"\n'
        "  OPERATION Print-Job\n"
        "  GROUP operation-attributes-tag\n"
        "  ATTR charset attributes-charset utf-8\n"
        "  ATTR language attributes-natural-language en\n"
        "  ATTR uri printer-uri $uri\n"
        "  ATTR name requesting-user-name test\n"
        "  ATTR mimeMediaType document-format %s\n"
        "  FILE %s\n"
        "  GROUP job-attributes-tag\n"
        "  ATTR keyword media %s\n"
        "  ATTR keyword print-scaling fit\n"
        "%s"
        "  STATUS successful-ok\n"
        "}\n" % (name, document_format, filename, media, extra),
        encoding="utf-8",
    )
    if CAPTURE.exists():
        CAPTURE.unlink()

    result = subprocess.run(
        ["ipptool", "-tv",
         "ipp://localhost:%d/ipp/print/LX900e" % PORT, str(test)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        check=False)
    if result.returncode != 0 or "[PASS]" not in result.stdout:
        raise AssertionError("ipptool failed:\n%s" % result.stdout[-2000:])

    # The job may still be rendering after Print-Job returns.
    deadline = time.time() + 60
    last_size = -1
    stable = 0
    while time.time() < deadline:
        if CAPTURE.exists():
            size = CAPTURE.stat().st_size
            if size > 0 and size == last_size:
                stable += 1
                if stable >= 5:
                    break
            else:
                stable = 0
                last_size = size
        time.sleep(0.1)
    if not CAPTURE.exists() or CAPTURE.stat().st_size == 0:
        raise AssertionError("no printer output; log:\n%s"
                             % LOG.read_text(errors="replace")[-3000:])


def make_pwg(tmp, w, h, colour):
    out = tmp / "input.pwg"
    args = [MKRASTER, "--pwg", str(w), str(h), "1200", "fill"] + \
           [str(c) for c in colour]
    with out.open("wb") as f:
        subprocess.run(args, check=True, stdout=f)
    return out


def make_pdf(tmp, pages=1):
    ps = tmp / ("input%d.ps" % pages)
    pdf = tmp / ("input%d.pdf" % pages)
    body = ["%!PS-Adobe-3.0\n", "%%%%Pages: %d\n" % pages]
    for page in range(1, pages + 1):
        body.append("%%%%Page: %d %d\n" % (page, page))
        body.append("0 setgray\n")
        body.append("%d %d 200 200 rectfill\n" % (36 + 20 * page, 36 + 20 * page))
        body.append("showpage\n")
    body.append("%%EOF\n")
    ps.write_text("".join(body), encoding="ascii")
    subprocess.run(["ps2pdf", str(ps), str(pdf)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return pdf


def captured():
    data = CAPTURE.read_bytes()
    return S.Stream(data), data


def check_detection():
    """The URI printer, with and without a printer attached."""
    result = subprocess.run([APP, "--list-devices"], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, check=False)
    assert result.returncode == 0, result.stderr
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    for line in lines:
        assert "\t" in line, line
        uri, device_id = line.split("\t", 1)
        assert uri.startswith("usb://"), line

    found = [line.split("\t", 1)[0] for line in lines
             if "Color_Label_900" in line]
    detected = subprocess.run([APP, "--detect"], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, check=False)
    if found:
        assert detected.returncode == 0, detected.stderr
        assert detected.stdout.strip() == found[0], (detected.stdout, found)
        assert "serial=" in detected.stdout, detected.stdout
    else:
        assert detected.returncode != 0, detected.stdout
        assert "no LX900e" in detected.stderr, detected.stderr


def check_startup_banner():
    text = OUT.read_text(errors="replace")
    assert "ipp://localhost:%d/ipp/print/LX900e" % PORT in text, text
    assert "/LX900e/ipp" not in text, text


def check_web():
    # PAPPL 1.3.1's /addprinter page asserts in Avahi when no DNS-SD daemon
    # is running, so it is not exercised here; the printer pages are ours.
    for path in ("/", "/LX900e/", "/LX900e/media", "/LX900e/printing",
                 "/LX900e/supplies", "/config"):
        with urllib.request.urlopen(
                "http://127.0.0.1:%d%s" % (PORT, path), timeout=5) as r:
            assert r.status == 200, (path, r.status)
            assert r.read(1), path

    with urllib.request.urlopen(
            "http://127.0.0.1:%d/style.css" % PORT, timeout=5) as r:
        css = r.read().decode("utf-8", errors="replace")
    assert "--accent" in css, "the LX900e stylesheet is not being served"
    assert "fonts.googleapis.com" not in css, "stylesheet uses external fonts"

    assert PROCESS.poll() is None, "the application exited during the web test"


def get_attributes(tmp):
    """Get every printer attribute, including our vendor ones.

    A request with a single "all" makes CUPS hand PAPPL a NULL requested-
    attributes array, so PAPPL copies the vendor attributes too.  Asking for
    "all,media-col-database" (what the stock ipptool test does) instead
    expands "all" to the IANA lists, which do not know our names.
    """
    request = tmp / "attrs.ipp"
    request.write_text(
        "{\n"
        '  NAME "attributes"\n'
        "  OPERATION Get-Printer-Attributes\n"
        "  GROUP operation-attributes-tag\n"
        "  ATTR charset attributes-charset utf-8\n"
        "  ATTR language attributes-natural-language en\n"
        "  ATTR uri printer-uri $uri\n"
        "  ATTR keyword requested-attributes all\n"
        "  STATUS successful-ok\n"
        "}\n",
        encoding="utf-8")
    result = subprocess.run(
        ["ipptool", "-tv",
         "ipp://localhost:%d/ipp/print/LX900e" % PORT, str(request)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        check=False)
    assert result.returncode == 0, result.stdout[-2000:]
    return result.stdout


def attr_line(text, name):
    for line in text.splitlines():
        line = line.strip()
        if line.startswith(name + " ("):
            return line.split("=", 1)[1].strip()
    return ""


def check_attributes(tmp):
    text = get_attributes(tmp)

    media = attr_line(text, "media-supported")
    assert media, text[-2000:]
    names = media.split(",")
    assert len(names) == len(set(names)), "duplicate media names: %s" % names
    for want in ("custom_4x3in_4x3in", "custom_2x1in_2x1in",
                 "custom_100x76mm_100x76mm", "custom_8x8in_8x8in"):
        assert want in names, (want, names)
    assert len(names) == 17, names

    ready = attr_line(text, "media-ready").split(",")
    assert len(ready) == 16, ready
    assert ready[0] == "custom_4x3in_4x3in", ready
    assert "custom_2x1in_2x1in" in ready, ready
    assert "custom_100x76mm_100x76mm" not in ready, ready

    sources = attr_line(text, "media-source-supported").split(",")
    assert "main-roll" in sources, sources
    assert "roll-4x6" in sources, sources
    assert len(sources) == len(set(sources)), sources

    col = attr_line(text, "media-col-ready")
    assert col.count("media-source=") == 16, col
    assert "media-source=main-roll" in col, col

    assert attr_line(text, "media-default") == "custom_4x3in_4x3in", text
    assert attr_line(text, "printer-resolution-default") == "1200dpi", text
    assert attr_line(text, "pwg-raster-document-type-supported") == \
        "black_1,sgray_8,srgb_8", text
    assert "color" in attr_line(text, "print-color-mode-supported"), text
    assert attr_line(text, "lx900-ink-cartridge-default") == "ColorBlack", text
    assert "Color,Black,ColorBlack" == \
        attr_line(text, "lx900-ink-cartridge-supported"), text
    assert "label" in attr_line(text, "printer-kind"), text


def check_pwg_black(tmp):
    pwg = make_pwg(tmp, 4800, 3600, (0, 0, 0))
    ipptool_job(tmp, "PWG raster black", pwg, "image/pwg-raster")
    st, data = captured()

    assert st.width == 4800, st.width
    assert st.plane_count == 4, st.plane_count
    assert st.res_code == 2, st.res_code
    assert st.tags.get(0x01) == 0x0C, st.tags.get(0x01)
    assert st.end_page_count == 1, st.end_page_count
    assert st.has_eod_0b and st.has_eod_0e
    assert st.coverage(3) > 0.9, st.coverage(3)
    for plane in (0, 1, 2):
        assert st.coverage(plane) == 0.0, (plane, st.coverage(plane))
    return len(data)


def check_pwg_colour(tmp):
    pwg = make_pwg(tmp, 4800, 3600, (0, 255, 255))     # cyan
    ipptool_job(tmp, "PWG raster cyan", pwg, "image/pwg-raster",
                "  ATTR keyword lx900-ink-cartridge Color\n")
    st, _ = captured()

    assert st.plane_count == 3, st.plane_count
    assert st.tags.get(0x01) == 0x0B, st.tags.get(0x01)
    assert st.coverage(0) > 0.9, st.coverage(0)
    assert st.coverage(1) == 0.0 and st.coverage(2) == 0.0


def check_pdf(tmp):
    pdf = make_pdf(tmp)
    ipptool_job(tmp, "PDF", pdf, "application/pdf")
    st, _ = captured()

    assert st.width == 4800, st.width
    assert st.plane_count == 4, st.plane_count
    assert st.coverage(3) > 0.02, st.coverage(3)
    assert st.end_page_count == 1


def check_multipage(tmp):
    pdf = make_pdf(tmp, pages=2)
    ipptool_job(tmp, "PDF two pages", pdf, "application/pdf")
    st, _ = captured()

    assert st.end_page_count == 2, st.end_page_count
    assert st.has_eod_0b and st.has_eod_0e


def check_copies(tmp):
    pdf = make_pdf(tmp)
    ipptool_job(tmp, "PDF two copies", pdf, "application/pdf",
                "  ATTR integer copies 2\n")
    st, data = captured()

    assert st.end_page_count == 2, st.end_page_count
    # Two identical pages: the stream is the single page twice.
    assert len(data) % 2 == 0
    half = len(data) // 2
    assert data[:half] == data[half:], "copies are not identical"


def main():
    with tempfile.TemporaryDirectory(prefix="lx900e-pappl-") as tmpname:
        tmp = pathlib.Path(tmpname)
        try:
            check_detection()
            if VERBOSE:
                print("ok   device detection")
            start_app(tmp)
            check_startup_banner()
            if VERBOSE:
                print("ok   banner")
            check_web()
            if VERBOSE:
                print("ok   web interface and stylesheet")
            check_attributes(tmp)
            if VERBOSE:
                print("ok   printer attributes")
            check_pwg_black(tmp)
            if VERBOSE:
                print("ok   PWG raster, black")
            check_pwg_colour(tmp)
            if VERBOSE:
                print("ok   PWG raster, colour cartridge")
            check_pdf(tmp)
            if VERBOSE:
                print("ok   PDF through Ghostscript")
            check_multipage(tmp)
            if VERBOSE:
                print("ok   multi-page PDF")
            check_copies(tmp)
            if VERBOSE:
                print("ok   copies")
        finally:
            stop_app()
    print("pappl-integration: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
