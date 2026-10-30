#!/bin/sh
#
# Build everything that can be built without a printer, then run every test
# suite.  This is what `make test` and CI run.
#
#   tests/run.sh            # all suites
#   tests/run.sh -v         # same, with per-test names
#
# Nothing here needs root, a CUPS daemon or a printer. The suites that would
# need hardware are in docs/TEST-PLAN.md.
#
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

CC=${CC:-cc}
VERBOSE=""
[ "${1:-}" = "-v" ] && VERBOSE=-v

fail=0

step() { printf '\n== %s\n' "$*"; }

run() {
    name=$1
    shift
    if "$@"; then
        : 
    else
        printf '%s failed\n' "$name"
        fail=1
    fi
}

step "build"
if ! make -C driver >/dev/null; then
    echo "cannot build the driver (libcups2-dev missing?)"
    exit 1
fi
mkdir -p build
$CC -O2 -o build/mkraster tools/mkraster.c -lcups || exit 1
$CC -O2 -o build/colortest tools/colortest.c || exit 1
if [ -f /usr/include/libusb-1.0/libusb.h ]; then
    if $CC -O2 -o build/lx900-send tools/lx900-send.c -lusb-1.0 -lpthread; then
        :
    else
        echo "(could not build lx900-send, continuing)"
    fi
else
    echo "(libusb-1.0 headers missing: skipping the tools that need them)"
fi

step "encoder"
run encoder python3 tests/test_encoder.py $VERBOSE

step "PPD generator"
run ppd python3 tests/test_ppd.py $VERBOSE

step "backend helpers"
if [ -f /usr/include/libusb-1.0/libusb.h ]; then
    conf=$(mktemp "${TMPDIR:-/tmp}/lx900-unit-XXXXXX") || exit 1
    if $CC -O2 -Wall -Wextra -DLX900_CONF="\"$conf\"" \
           -o build/unit_status tests/unit_status.c -lusb-1.0; then
        run backend build/unit_status
    else
        echo "could not build the backend unit tests"
        fail=1
    fi
    rm -f "$conf"
else
    echo "skipped (needs libusb-1.0 headers)"
fi

step "shell tools"
run tools sh tests/test_tools.sh

step "printer application"
if pkg-config --exists pappl 2>/dev/null; then
    if make pappl >/dev/null 2>&1; then
        run pappl python3 tests/pappl-integration.py $VERBOSE
    else
        echo "could not build the printer application"
        fail=1
    fi
else
    echo "skipped (PAPPL is not installed)"
fi

printf '\n'
if [ "$fail" = 0 ]; then
    echo "all test suites passed"
else
    echo "SOME TESTS FAILED"
fi
exit "$fail"
