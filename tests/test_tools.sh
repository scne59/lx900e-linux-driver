#!/bin/sh
#
# Checks on the shell tools that do not need a printer: syntax, usage text and
# argument handling.  The behaviour that needs hardware is in
# docs/TEST-PLAN.md, not here.
#
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

fail=0

pass() { :; }
bad() { printf 'FAIL %s\n' "$1"; fail=1; }

check() {
    name=$1
    shift
    "$@" >/dev/null 2>&1 || bad "$name"
}

expect_fail() {
    name=$1
    shift
    if "$@" >/dev/null 2>&1; then bad "$name (should have failed)"; fi
}

expect_text() {
    name=$1
    want=$2
    shift 2
    out=$("$@" 2>&1) || { bad "$name (exit status)"; return; }
    case $out in
        *"$want"*) pass ;;
        *) bad "$name (no \"$want\" in output)";;
    esac
}

# ---------------------------------------------------------------- syntax

for f in tools/*.sh install.sh tests/*.sh; do
    check "sh -n $f" sh -n "$f"
done

for f in tools/*.py tests/*.py; do
    check "python -m py_compile $f" python3 -m py_compile "$f"
done

if [ -f build/lx900-send ]; then
    check "lx900-send rejects a bad option" sh -c '! build/lx900-send -z'
fi

# ---------------------------------------------------------------- usage

expect_text "doctor help" "bring-up ladder" tools/lx900-doctor.sh -h
expect_text "align usage" "registration" tools/lx900-align.sh
expect_text "align show" "config:" tools/lx900-align.sh show
expect_text "purge help" "pull ink through" tools/lx900-purge.sh -h
expect_text "installer help" "--dry-run" ./install.sh --help

expect_fail "align rejects an unknown command" tools/lx900-align.sh frobnicate
expect_fail "purge rejects an unknown ink" tools/lx900-purge.sh -i purple --dry-run
expect_fail "purge rejects a non-numeric count" tools/lx900-purge.sh -n many --dry-run
expect_fail "installer rejects an unknown option" ./install.sh --frobnicate

# ---------------------------------------------------------------- unit generation

# The device URI can contain & or |, which sed would treat specially.  The
# generator uses index()/substr() so the URI survives unchanged.
line=$(printf '@BINDIR@/lx900e-app@DEVICE_ARGS@ --port 8000\n' |
    awk -v bindir=/usr/bin \
        -v args=" --device 'usb://Primera/Color_Label_900?serial=a&b'" \
        -f tools/gen-service.awk)
if [ "$line" != "/usr/bin/lx900e-app --device 'usb://Primera/Color_Label_900?serial=a&b' --port 8000" ]; then
    bad "service generator (got \"$line\")"
fi

line=$(printf '@BINDIR@/lx900e-app@DEVICE_ARGS@ --port 8000\n' |
    awk -v bindir=/usr/bin -v args="" -f tools/gen-service.awk)
if [ "$line" != "/usr/bin/lx900e-app --port 8000" ]; then
    bad "service generator without a device (got \"$line\")"
fi

# ---------------------------------------------------------------- offline run

if [ -x build/colortest ]; then
    expect_text "purge dry run" "(dry run" tools/lx900-purge.sh -i k --dry-run
fi

printf '%s\n' "$([ "$fail" = 0 ] && echo "shell tool tests passed" || echo "shell tool tests failed")"
exit "$fail"
