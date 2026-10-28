#!/bin/sh
#
# lx900-align - registration alignment for the LX900e.
#
#   tools/lx900-align.sh print                 print the alignment target
#   tools/lx900-align.sh show                  show the configured offsets
#   tools/lx900-align.sh set --left 0.4 --top -0.2
#   tools/lx900-align.sh reset                 back to the defaults
#
# "Alignment" here means where the image lands on the label: TOF (along the
# feed) and horizontal.  That is what the vendor utility's Align wizard does -
# it prints its own TestOffsets.prn target and then stores TOF/HORZ offsets,
# which the driver converts to 1200 dpi and applies as a shift.  This tool uses
# the same target file and writes the same two offsets.
#
# The offsets live in /etc/lx900.conf, in dots at 1200 dpi, and the filter
# applies them to every job.  Positive `left` moves the image right, positive
# `top` moves it down the label.
#
# What this does not do: align the colour head against the black head.  Those
# offsets are not exposed by the firmware in any stream we can produce, and the
# vendor wizard does not touch them either.
#
set -u

say() { printf '%s\n' "$*"; }
die() { printf '\nSTOP: %s\n' "$*" >&2; exit 1; }

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build"
SEND="$BUILD/lx900-send"
COLORTEST="$BUILD/colortest"
CONF=/etc/lx900.conf
QUEUE=${LX900_QUEUE:-LX900e}
SIZE=${LX900_PAGESIZE:-}
PPD=/usr/share/cups/model/Primera-LX900e.ppd
# Build the target with the filter from this tree: a pre-rendered file would
# not carry the offsets we are trying to tune.
FILTER="$ROOT/driver/rastertoLX900"
[ -x "$FILTER" ] || FILTER=/usr/lib/cups/filter/rastertoLX900

DPI=1200

dots_of_mm() {
    LC_ALL=C awk -v mm="$1" -v dpi="$DPI" 'BEGIN { printf "%d", mm * dpi / 25.4 + (mm < 0 ? -0.5 : 0.5) }'
}

mm_of_dots() {
    LC_ALL=C awk -v d="$1" -v dpi="$DPI" 'BEGIN { printf "%.2f", d * 25.4 / dpi }'
}

current() {
    key=$1 dflt=$2
    v=$dflt
    if [ -r "$CONF" ]; then
        got=$(sed -n "s/^[[:space:]]*$key[[:space:]]*=[[:space:]]*\(-\{0,1\}[0-9]\{1,\}\).*/\1/p" "$CONF" | tail -n 1)
        [ -n "$got" ] && v=$got
    fi
    printf '%s\n' "$v"
}

show() {
    left=$(current left 0)
    top=$(current top 4)
    say "config: $CONF"
    say "  left = $left dots  ($(mm_of_dots "$left") mm from the left edge)"
    say "  top  = $top dots  ($(mm_of_dots "$top") mm down the label)"
    say ""
    say "Positive left moves the image right, positive top moves it down."
}

write_conf() {
    left=$1
    top=$2
    tmp=$(mktemp) || die "cannot create a temporary file"
    {
        say "# LX900e registration offsets, in dots at 1200 dpi."
        say "# Written by tools/lx900-align.sh.  Negative values are allowed"
        say "# for left; top is clamped at 0 by the filter."
        say "left=$left"
        say "top=$top"
    } > "$tmp"
    if [ "$(id -u)" = 0 ]; then
        install -m 644 "$tmp" "$CONF" || { rm -f "$tmp"; die "cannot write $CONF"; }
    else
        sudo install -m 644 "$tmp" "$CONF" || { rm -f "$tmp"; die "cannot write $CONF"; }
    fi
    rm -f "$tmp"
    say "wrote $CONF"
}

resolve_size() {
    if [ -z "$SIZE" ]; then
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
}

print_target() {
    [ -x "$COLORTEST" ] || die "$COLORTEST is missing; run ./install.sh first"
    [ -x "$SEND" ] || die "$SEND is missing; run ./install.sh first"
    [ -x "$FILTER" ] || die "$FILTER is missing; run make -C driver"

    resolve_size

    # Send the page size as a custom size in points.  A named size only works
    # if the PPD has that choice; anything else silently falls back to the
    # queue default, which is how a "50x30mm" request used to print a 4x3 page.
    psopt=$(LC_ALL=C awk -v w="$W" -v h="$H" 'BEGIN {
        printf "Custom.%.2fx%.2f", w * 72 / 1200, h * 72 / 1200
    }')

    say "Page size: $SIZE (${W}x${H} dots, $psopt)"
    say "Current offsets:"
    show | sed 's/^/  /'
    say ""
    say "Target: a registration gauge in the page's top right corner, which is"
    say "the bottom left as you look at the label from the front of the printer:"
    say ""
    say "  a 20 mm line on the top edge, from the corner inwards"
    say "  a 10 mm line 10 mm below it, ending 10 mm in from the end"
    say "  a 20 mm line on the right edge, from the top corner down"
    say "  a 10 mm line 10 mm in from the right edge, starting 10 mm down"
    say ""
    say "The two short lines meet, so each pair forms an L: the long one on the"
    say "hard edge and page corner, the short one at the 10 mm corner."
    say ""
    say "The outer line of each pair marks 0 mm, the inner one 10 mm.  On the"
    say "label you will see them along the left edge and along the bottom edge."
    say "The spacing should measure 10 mm; the outer line should sit on the hard"
    say "edge.  Any gap there is the offset to set with --left / --top, in mm."
    say "The page repeats this in small type next to the gauge."
    say ""
    say "Everything is in the corner, so a small page is enough and less stock"
    say "is used:  print --size 50x30mm"
    say "(the mm suffix is required - without it the numbers are inches)"
    say ""

    "$SEND" -q 2>/dev/null | sed -n 's/^  \(tanks\|ink\):  */  /p' || true

    png=$BUILD/align.png
    rast=$BUILD/align.rast
    stream=$BUILD/align.lx9

    "$COLORTEST" align "$W" "$H" > "$png" \
        || die "could not build the target image"

    PPD=$PPD CONTENT_TYPE=image/png CHARSET=utf-8 \
        /usr/lib/cups/filter/imagetoraster 1 s t 1 \
        "PageSize=$psopt Resolution=1200dpi" "$png" > "$rast" 2>/dev/null \
        || die "imagetoraster failed"
    [ -s "$rast" ] || die "empty raster from imagetoraster"

    "$FILTER" 1 s t 1 "Resolution=1200dpi InkCartridge=ColorBlack" "$rast" \
        < "$rast" > "$stream" 2>/dev/null || die "the filter failed"
    [ -s "$stream" ] || die "empty stream"

    say ""
    "$SEND" -d "$stream" || die "sending failed"
    say ""
    say "Then set the difference, e.g.  tools/lx900-align.sh set --left -0.5"
}

if [ $# -eq 0 ]; then
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
fi

cmd=$1
shift

case $cmd in
    print)
        while [ $# -gt 0 ]; do
            case $1 in
                --size)
                    [ $# -ge 2 ] || die "--size needs a WxH value, e.g. 50x30"
                    SIZE=$2; shift 2 ;;
                *) die "unknown option $1 for print (use --size WxH)" ;;
            esac
        done
        if [ -r /usr/lib/cups/filter/rastertoLX900 ] &&
           ! cmp -s /usr/lib/cups/filter/rastertoLX900 "$ROOT/driver/rastertoLX900"; then
            say "NOTE: the installed filter differs from driver/rastertoLX900."
            say "      This target uses the tree build, but jobs through CUPS"
            say "      will not see the offsets until: sudo ./install.sh"
            say ""
        fi
        print_target
        ;;
    show)
        show
        ;;
    reset)
        write_conf 0 4
        show
        ;;
    set)
        left_dots=$(current left 0)
        top_dots=$(current top 4)
        while [ $# -gt 0 ]; do
            case $1 in
                --left)
                    [ $# -ge 2 ] || die "--left needs a value in mm"
                    left_dots=$(dots_of_mm "$2"); shift 2 ;;
                --top)
                    [ $# -ge 2 ] || die "--top needs a value in mm"
                    top_dots=$(dots_of_mm "$2"); shift 2 ;;
                *) die "unknown option $1 (use --left/--top, in mm)" ;;
            esac
        done
        [ "$top_dots" -ge 0 ] || die "top cannot be negative (the label starts at the top edge)"
        write_conf "$left_dots" "$top_dots"
        show
        say ""
        say "Print the target again to check:  tools/lx900-align.sh print"
        ;;
    -h|--help)
        sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
        ;;
    *)
        die "unknown command '$cmd' (print, show, set, reset)"
        ;;
esac
