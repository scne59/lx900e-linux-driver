#!/bin/sh
#
# lx900-doctor - walk the bring-up ladder in docs/TEST-PLAN.md.
#
#   tools/lx900-doctor            # interactive: fail fast, ask after each page
#   tools/lx900-doctor -n 120     # wait up to 120 s for the printer to answer
#   tools/lx900-doctor -s 3       # start at step 3 (skip the earlier ones)
#
# The script never installs anything and never needs root.  It stops at the
# first step that fails, because a later step on a wedged printer tells you
# nothing and costs you another mains power cycle.
#
# Each printed page needs a human to look at it, so the script asks one
# question per page: did it print, did it feed, does the printer still answer.
#
# Every raw send is also recorded: the printer talks back on its bulk-IN
# endpoint, and the bytes it sends immediately before it hangs are the only
# direct evidence of why it refused the page.  They land in build/in-stepN.log.
#
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build"
SEND="$BUILD/lx900-send"
COLORTEST="$BUILD/colortest"
QUEUE=${LX900_QUEUE:-LX900e}
PPD=/usr/share/cups/model/Primera-LX900e.ppd
# Test pages must be encoded by the filter in this tree, not by whatever is
# installed: otherwise the ladder silently tests an older build.
FILTER=$ROOT/driver/rastertoLX900
[ -x "$FILTER" ] || FILTER=/usr/lib/cups/filter/rastertoLX900

WAIT=0
START=1

while getopts "n:s:h" opt; do
    case $opt in
        n) WAIT=$OPTARG ;;
        s) START=$OPTARG ;;
        h) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) exit 2 ;;
    esac
done

say()   { printf '%s\n' "$*"; }
head_() { printf '\n=== %s\n' "$*"; }
note()  { printf '    %s\n' "$*"; }
die()   { printf '\nSTOP: %s\n' "$*" >&2; exit 1; }

# Ask a yes/no question.  Every question here is about something a human has to
# look at (a label coming out), so refuse to run without a terminal rather than
# guessing an answer.
ask() {
    if [ ! -t 0 ]; then
        die "this needs a terminal: every step asks you to look at the printed
label.  Run it directly, not through a pipe or cron."
    fi
    printf '%s [Y/n] ' "$1"
    read -r a
    case ${a:-y} in
        [Nn]*) return 1 ;;
        *) return 0 ;;
    esac
}

# Wait until the printer answers a status request and reports ready.
#
# The first request after a power cycle always looks broken: type=0x31,
# error=0x36, all four tanks INVALID.  It settles to type=0x49 and all tanks ok
# within a few seconds, so keep polling and never send a job during that window.
wait_ready() {
    waited=0
    while :; do
        out=$("$SEND" -q 2>&1)
        if printf '%s' "$out" | grep -q 'status reply: 144 bytes'; then
            printf '%s\n' "$out" | grep -E '^  (type=|tanks:|present=)' || true
            if printf '%s' "$out" | grep -q 'type=0x49'; then
                return 0
            fi
            say "still starting up (type != 0x49), waiting"
        elif printf '%s' "$out" | grep -q 'claim failed'; then
            die "the USB interface is owned by another process - almost always
the CUPS usb backend finishing or stuck on a job.  Check 'lpstat -o' and
'ps aux | grep backend/usb'.  Let the job finish, or release it with:

    sudo systemctl restart cups

This is not a printer fault; the status request never reached it."
        elif [ "$waited" -ge "$WAIT" ]; then
            printf '%s\n' "$out" >&2
            die "the printer is not answering.  Power it off at the mains, wait
ten seconds, switch it back on, then run this with -n 120 (or -n 0 if it is
already up)."
        fi
        sleep 4
        waited=$((waited + 4))
    done
}

# Send one raw stream and record everything the printer says while it does.
raw_send() {
    log=$BUILD/in-step$1.log
    shift
    rm -f "$log"
    "$SEND" -d -w 8 -o "$log" "$@" || die "sending failed"
    say "printer's own words: $(wc -c < "$log") bytes in $(basename "$log")"
    sleep 2
}

still_alive() {
    out=$("$SEND" -q 2>&1)
    case $out in
        *"claim failed"*)
            die "the USB interface is owned by another process, so the result of
that step is unknown.  Free it with: sudo systemctl restart cups" ;;
    esac
    printf '%s' "$out" | grep -q 'status reply: 144 bytes'
}

wait_job() {
    n=0
    while lpstat -W not-completed -o 2>/dev/null | grep -q "^$1 "; do
        n=$((n + 1))
        [ "$n" -gt 150 ] && die "job $1 is still queued after 5 minutes"
        sleep 2
    done
}

[ -x "$SEND" ] || die "$SEND is missing; run ./install.sh first"
[ -x "$COLORTEST" ] || die "$COLORTEST is missing; run ./install.sh first"

# ---------------------------------------------------------------- assets

assets() {
    [ -f "$BUILD/bartop.png" ] || "$COLORTEST" bar 2400 1200 top > "$BUILD/bartop.png"
    [ -f "$BUILD/kbar.png" ]   || "$COLORTEST" bar 2400 1200 > "$BUILD/kbar.png"
    [ -f "$BUILD/colortest-2x1.png" ] || "$COLORTEST" 2400 1200 > "$BUILD/colortest-2x1.png"

    [ -r "$PPD" ] || die "$PPD is missing; run: sudo ./install.sh"
    [ -x "$FILTER" ] || die "$FILTER is missing; run: make -C driver"

    # want             image         InkCartridge
    for spec in \
        "bartop-Black     bartop.png Black" \
        "kbar-Color       kbar.png   Color" \
        "kbar-ColorBlack  kbar.png   ColorBlack"
    do
        set -- $spec
        want=$1 png=$2 mode=$3
        [ -s "$BUILD/$want.lx9" ] && continue

        rast=$BUILD/tmp.rast
        PPD=$PPD CONTENT_TYPE=image/png CHARSET=utf-8 \
            /usr/lib/cups/filter/imagetoraster 1 s t 1 \
            "PageSize=2x1 Resolution=1200dpi" "$BUILD/$png" > "$rast" 2>/dev/null \
            || die "could not rasterise $png"
        [ -s "$rast" ] || die "empty raster from imagetoraster"
        "$FILTER" 1 s t 1 "Resolution=1200dpi InkCartridge=$mode" "$rast" \
            < "$rast" > "$BUILD/$want.lx9" 2>/dev/null \
            || die "could not encode $want"
        [ -s "$BUILD/$want.lx9" ] || die "empty $want.lx9"
        rm -f "$rast"
    done
    say "test pages in $BUILD:"
    for f in bartop-Black kbar-Color kbar-ColorBlack; do
        note "$f.lx9  $(wc -c < "$BUILD/$f.lx9") bytes"
    done
}

# ---------------------------------------------------------------- steps

step0() {
    head_ "step 0: is the firmware awake?"
    wait_ready
    say "printer is ready"
    say ""
    say "This printer has status LEDs only, no display, so the block above is the"
    say "machine-readable state.  type=0x49 with all tanks ok is as ready as it"
    say "gets.  The 0x31/0x36 + INVALID reading right after power-on is normal."
}

step1() {
    head_ "step 1: does the CUPS queue reach the printer?  (CMY, 3 planes)"
    say "printing the diagnostic page through CUPS with InkCartridge=Color"
    job=$(lp -d "$QUEUE" -o InkCartridge=Color "$BUILD/colortest-2x1.png" 2>&1)
    case $job in
        *"request id is"*) say "$job" ;;
        *) die "lp failed: $job" ;;
    esac
    id=$(printf '%s' "$job" | sed -n 's/.*request id is \([^ ]*\).*/\1/p')
    wait_job "$id"
    say "job finished; check /var/log/cups/error_log if nothing came out"
    if ! ask "Did the diagnostic page print (some colour on the label)?"; then
        die "the queue or the printer is broken, so the encoder is not the
problem.  Check /var/log/cups/error_log for 'rastertoLX900' and for the usb
backend, and see docs/INSTALL.md#troubleshooting."
    fi
}

step2() {
    head_ "step 2: black ink  (4 planes, page-info media code 0x0c)"
    say "a 1.5 x 0.125 inch black bar near the top of the page, four plane"
    say "records, ink only on K, tag 0x01 = 0x0c.  This is the combination"
    say "that was verified on hardware after the media-code fix."
    raw_send 2 "$BUILD/bartop-Black.lx9"
    if ! still_alive; then
        die "black ink hung the printer again.  Check that the installed filter
is current (sudo ./install.sh): an older build sends tag 0x01 = 0x0b and the
printer refuses it.  Send build/in-step2.log."
    fi
    if ! ask "Did the black bar print near the top of the label?"; then
        say ""
        say "It fed but stayed blank: ink path, not stream.  Do NOT resend."
        die "black head did not fire"
    fi
    say "black ink works"
}

step3() {
    head_ "step 3: black ink with an empty first band"
    say "the same bar moved to the middle, so the first band carries no ink."
    say "The filter writes that band as real records with zero-length rows"
    say "instead of skip records - a shape the vendor stream always has."
    say ""
    say "This is the ordinary case: a label with a blank strip at the top."
    raw_send 3 "$BUILD/kbar-ColorBlack.lx9"
    if ! still_alive; then
        die "the printer hung on a page whose first band is empty.  The band is
written as real records, so either the installed filter is old
(sudo ./install.sh) or this page shape needs more work.  Send
build/in-step3.log."
    fi
    if ! ask "Did the black bar print in the middle of the label?"; then
        die "the page was accepted but the bar did not print; check the label."
    fi
    say "black works with a blank strip at the top of the page"
}

step4() {
    head_ "step 4: the queue, with black  (ColorBlack through CUPS)"
    job=$(lp -d "$QUEUE" -o InkCartridge=ColorBlack "$BUILD/kbar.png" 2>&1)
    case $job in
        *"request id is"*) say "$job" ;;
        *) die "lp failed: $job" ;;
    esac
    id=$(printf '%s' "$job" | sed -n 's/.*request id is \([^ ]*\).*/\1/p')
    wait_job "$id"
    if ! ask "Did the bar print through CUPS?"; then
        die "CUPS + ColorBlack failed although the raw send worked: the fault is
in the CUPS path, not the encoder.  Compare the job's options with the direct
send (docs/TEST-PLAN.md step 4)."
    fi
    say "the queue, the encoder and black ink all agree"
}

step5() {
    head_ "step 5: multiple copies"
    job=$(lp -d "$QUEUE" -n 3 -o InkCartridge=Color "$BUILD/colortest-2x1.png" 2>&1)
    case $job in
        *"request id is"*) say "$job" ;;
        *) die "lp failed: $job" ;;
    esac
    id=$(printf '%s' "$job" | sed -n 's/.*request id is \([^ ]*\).*/\1/p')
    wait_job "$id"
    if ! ask "Did three separate labels come out?"; then
        die "the copy count is not being repeated.  The filter emits the page
argv[4] times; if only one label came out, the installed filter is old
(sudo ./install.sh) or the PPD lost *cupsManualCopies."
    fi
    say "copies work"
}

assets

# CUPS steps run whatever filter is installed, which may be older than this
# tree.  Say so rather than letting the ladder test a stale build.
INSTALLED=/usr/lib/cups/filter/rastertoLX900
if [ -r "$INSTALLED" ] && [ -x "$ROOT/driver/rastertoLX900" ] &&
   ! cmp -s "$INSTALLED" "$ROOT/driver/rastertoLX900"; then
    say ""
    say "NOTE: the installed filter differs from driver/rastertoLX900."
    say "      Steps 1, 4 and 5 go through CUPS and would test the old build."
    say "      Run: sudo ./install.sh"
fi

step0
[ "$START" -le 1 ] && step1
[ "$START" -le 2 ] && step2
[ "$START" -le 3 ] && step3
[ "$START" -le 4 ] && step4
[ "$START" -le 5 ] && step5

head_ "done"
say "every step printed.  The printer, the queue and the encoder are all good."
say "Any remaining fault is ink or head related, not software."
exit 0
