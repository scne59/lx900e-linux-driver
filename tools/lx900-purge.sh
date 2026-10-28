#!/bin/sh
#
# lx900-purge - pull ink through the nozzles by printing a flood.
#
#   tools/lx900-purge.sh                 # one page, all four inks in bands
#   tools/lx900-purge.sh -i m            # one page of solid magenta
#   tools/lx900-purge.sh -i m -n 3 -w 5  # three, five seconds apart
#   tools/lx900-purge.sh -i c --check    # purge cyan, then print the nozzle page
#
# This is a purge by printing: 100 % coverage of the chosen channel, which is
# what pulls ink through a stubborn nozzle.  It is NOT the printer's own
# maintenance station - this machine has no maintenance-station command we can
# send, and the vendor's "Purge" is the same idea, a printed flood.
#
# Each page uses label stock, so it is not free.  The script reports how much
# each channel dropped so you can see whether it is doing anything.
#
# After a purge, print the nozzle check and look at the stripe band for the
# ink you worked on: /usr/share/doc ... or build/colortest-2x1.png through the
# queue, see docs/INSTALL.md.
#
set -u

say() { printf '%s\n' "$*"; }
die() { printf '\nSTOP: %s\n' "$*" >&2; exit 1; }

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build"
COLORTEST="$BUILD/colortest"
SEND="$BUILD/lx900-send"
QUEUE=${LX900_QUEUE:-LX900e}

INK=all
COUNT=1
DWELL=3
SIZE=
CHECK=0
DRY=0

need() {
    [ $# -ge 2 ] || die "$1 needs a value"
}

while [ $# -gt 0 ]; do
    case $1 in
        -i) need "$@"; INK=$2; shift 2 ;;
        -n) need "$@"; COUNT=$2; shift 2 ;;
        -s) need "$@"; SIZE=$2; shift 2 ;;
        -w) need "$@"; DWELL=$2; shift 2 ;;
        -q) need "$@"; QUEUE=$2; shift 2 ;;
        -c|--check) CHECK=1; shift ;;
        -D|--dry-run) DRY=1; shift ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) printf 'unknown option: %s\n' "$1" >&2; exit 2 ;;
    esac
done

case $INK in
    all|c|m|y|k) ;;
    *) die "-i takes one of: c m y k all" ;;
esac
case $COUNT in
    ''|*[!0-9]*) die "-n must be a number" ;;
esac
[ "$COUNT" -ge 1 ] || die "-n must be at least 1"

[ -x "$COLORTEST" ] || die "$COLORTEST is missing; run ./install.sh first"

# ---------------------------------------------------------------- page size

if [ -z "$SIZE" ]; then
    # The queue's PPD default is what a plain 'lp' would use.
    ppd=/etc/cups/ppd/$QUEUE.ppd
    if [ -r "$ppd" ]; then
        SIZE=$(sed -n 's/^\*DefaultPageSize:[[:space:]]*//p' "$ppd" | head -n 1)
    fi
    [ -n "$SIZE" ] || SIZE=4x3
fi

dots=$(LC_ALL=C printf '%s' "$SIZE" | LC_ALL=C awk '{
    s = $0; mm = 0
    if (s ~ /mm$/) { mm = 1; sub(/mm$/, "", s) }
    split(s, a, "x")
    w = a[1] + 0; h = a[2] + 0
    if (w <= 0 || h <= 0) exit 1
    if (mm) { w = w * 1200 / 25.4; h = h * 1200 / 25.4 }
    else    { w = w * 1200;         h = h * 1200 }
    printf "%d %d\n", w + 0.5, h + 0.5
}') || die "cannot understand page size '$SIZE'"

W=${dots% *}
H=${dots#* }
[ -n "$W" ] && [ -n "$H" ] || die "cannot understand page size '$SIZE'"

# ---------------------------------------------------------------- helpers

levels() {
    # Best effort: the printer is busy while a job runs.
    "$SEND" -q 2>/dev/null | sed -n 's/^  ink:  /  /p'
}

counters() {
    "$SEND" -q 2>/dev/null | sed -n 's/^  used:  //p'
}

getuse() {
    [ -n "$1" ] && [ -n "$2" ] || return 1
    printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"
}

wait_job() {
    n=0
    while lpstat -W not-completed -o 2>/dev/null | grep -q "^$1 "; do
        n=$((n + 1))
        [ "$n" -gt 300 ] && die "job $1 is still queued after 10 minutes"
        sleep 2
    done
}

# ---------------------------------------------------------------- run

say "Purge: ink=$INK, $COUNT page(s) of $SIZE (${W}x${H} dots), queue $QUEUE"
say "This prints on label stock - one label per page."
say ""

before=$(levels)
before_used=$(counters)
[ -n "$before" ] && say "before:$before"

png=$BUILD/purge-$INK.png
"$COLORTEST" purge "$W" "$H" "$INK" > "$png" || die "could not build the purge image"
say "image:  $png"

if [ "$DRY" = 1 ]; then
    say "(dry run, nothing sent)"
    exit 0
fi

i=1
while [ "$i" -le "$COUNT" ]; do
    say ""
    say "page $i of $COUNT..."
    job=$(lp -d "$QUEUE" -o "PageSize=$SIZE" -o InkCartridge=ColorBlack "$png" 2>&1)
    case $job in
        *"request id is"*) say "  $job" ;;
        *) die "lp failed: $job" ;;
    esac
    id=$(printf '%s' "$job" | sed -n 's/.*request id is \([^ ]*\).*/\1/p')
    wait_job "$id"
    say "  done"
    i=$((i + 1))
    [ "$i" -le "$COUNT" ] && sleep "$DWELL"
done

sleep 2
after=$(levels)
after_used=$(counters)
if [ -n "$after" ]; then
    say ""
    say "after: $after"
    [ -n "$before" ] && say "  (before:$before)"
fi

# The drop counters say what actually happened; the percentages are too coarse
# to show a single page (one 4x3 page is about 1% of a colour cartridge).
if [ -n "$before_used" ] && [ -n "$after_used" ]; then
    tmp=${INK#all}
    [ -n "$tmp" ] || tmp=cmyk
    say ""
    used_any=0
    for c in c m y k; do
        case $tmp in *$c*) ;; *) continue ;; esac
        case $c in
            c) name=cyan;    cap=1800000000 ;;
            m) name=magenta; cap=1800000000 ;;
            y) name=yellow;  cap=1800000000 ;;
            k) name=black;   cap=3186000000 ;;
        esac
        b=$(getuse "$before_used" "$name")
        a=$(getuse "$after_used" "$name")
        [ -n "$b" ] && [ -n "$a" ] || continue
        line=$(awk -v a="$a" -v b="$b" -v cap="$cap" 'BEGIN {
            d = a - b
            if (d <= 0) { printf "no change - nothing was fired"; exit }
            printf "%.0f drops, %.2f%% of the cartridge", d, 100 * d / cap
        }')
        say "  $name: $line"
        used_any=1
    done
    [ "$used_any" = 1 ] || say "  (no usable counter data)"
fi

if [ "$CHECK" = 1 ]; then
    say ""
    say "Printing the nozzle check page..."
    job=$(lp -d "$QUEUE" -o "PageSize=$SIZE" -o InkCartridge=Color "$BUILD/colortest-2x1.png" 2>&1)
    case $job in
        *"request id is"*)
            id=$(printf '%s' "$job" | sed -n 's/.*request id is \([^ ]*\).*/\1/p')
            wait_job "$id" ;;
        *) say "lp failed: $job" ;;
    esac
    say "Look at the stripe band for '$INK': solid lines mean the nozzle is"
    say "firing, gaps mean it is still blocked.  A channel that stays blank"
    say "after two or three purges is not going to be fixed by more ink."
fi
