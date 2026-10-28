#!/bin/sh
#
# install.sh - build and install the Linux driver for the Primera LX900e.
#
#   ./install.sh              build, install files, create/refresh the queue
#   ./install.sh --no-queue   build and install files only
#   ./install.sh --uninstall  remove the queue, filter and PPD
#   ./install.sh --dry-run    show what would be done
#
# Everything is installed with "sudo" unless already running as root.

set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DRIVER="$HERE/driver"
TOOLS="$HERE/tools"
BUILD="${BUILD:-$HERE/build}"

QUEUE="${LX900_QUEUE:-LX900e}"
PPD_NAME="Primera-LX900e.ppd"
FILTERDIR="${FILTERDIR:-/usr/lib/cups/filter}"
BACKENDDIR="${BACKENDDIR:-/usr/lib/cups/backend}"
PPDDIR="${PPDDIR:-/usr/share/cups/model}"
FILTER="$FILTERDIR/rastertoLX900"
PPD="$PPDDIR/$PPD_NAME"

DO_QUEUE=1
DO_UNINSTALL=0
DRY=0

for arg in "$@"; do
    case "$arg" in
        --no-queue)  DO_QUEUE=0 ;;
        --uninstall) DO_UNINSTALL=1 ;;
        --dry-run)   DRY=1 ;;
        -h|--help)   sed -n '3,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

say()  { printf '%s\n' "$*"; }
step() { printf '\n== %s ==\n' "$*"; }
run()  {
    if [ "$DRY" = 1 ]; then
        printf '   [dry-run] %s\n' "$*"
    else
        "$@"
    fi
}
sudo_run() {
    if [ "$(id -u)" = 0 ]; then run "$@"
    elif [ "$DRY" = 1 ]; then printf '   [dry-run] sudo %s\n' "$*"
    else sudo "$@"; fi
}
# Build steps must not run as root when the script is called via sudo: the
# tree is owned by the user and root-owned build output is a nuisance.
build() {
    if [ "$DRY" = 1 ]; then
        printf '   [dry-run] %s\n' "$*"
        return 0
    fi
    if [ "$(id -u)" = 0 ] && [ -n "${SUDO_USER:-}" ]; then
        sudo -u "$SUDO_USER" -H "$@"
    else
        "$@"
    fi
}

# ---------------------------------------------------------------- uninstall

if [ "$DO_UNINSTALL" = 1 ]; then
    step "Removing printer queue $QUEUE"
    if lpstat -p "$QUEUE" >/dev/null 2>&1; then
        sudo_run lpadmin -x "$QUEUE"
    else
        say "queue $QUEUE does not exist, skipping"
    fi
    step "Removing installed files"
    sudo_run rm -f "$FILTER" "$PPD"
    say "done. (build/ and the source tree were left alone)"
    exit 0
fi

# ---------------------------------------------------------------- preflight

step "Checking build prerequisites"
missing=""
command -v cc  >/dev/null 2>&1 || missing="$missing cc"
command -v make >/dev/null 2>&1 || missing="$missing make"

if [ ! -f /usr/include/cups/raster.h ] && [ ! -f /usr/local/include/cups/raster.h ] \
   && ! pkg-config --exists cups 2>/dev/null; then
    say "CUPS development headers were not found."
    if command -v apt-get >/dev/null 2>&1; then
        say "  install with:  sudo apt-get install build-essential libcups2-dev"
    elif command -v dnf >/dev/null 2>&1; then
        say "  install with:  sudo dnf install gcc make cups-devel"
    elif command -v pacman >/dev/null 2>&1; then
        say "  install with:  sudo pacman -S base-devel cups"
    else
        say "  install your distribution's CUPS development package."
    fi
    missing="$missing libcups2-dev"
fi

if [ -n "$missing" ]; then
    say ""
    say "Missing:$missing"
    exit 1
fi
say "ok"

# ---------------------------------------------------------------- build

step "Building rastertoLX900"
build make -C "$DRIVER" clean >/dev/null 2>&1 || true
build make -C "$DRIVER"

step "Building helper tools (optional)"
build mkdir -p "$BUILD" 2>/dev/null || true
if [ -f /usr/include/libusb-1.0/libusb.h ]; then
    if build cc -O2 -o "$BUILD/lx900-send" "$TOOLS/lx900-send.c" -lusb-1.0 2>/dev/null; then
        [ "$DRY" = 1 ] || say "built $BUILD/lx900-send"
    else
        [ "$DRY" = 1 ] || say "could not build lx900-send (USB test tool), continuing"
    fi
else
    [ "$DRY" = 1 ] || say "libusb-1.0 headers not found, skipping lx900-send"
fi
if build cc -O2 -o "$BUILD/mkraster" "$TOOLS/mkraster.c" -lcups 2>/dev/null; then
    [ "$DRY" = 1 ] || say "built $BUILD/mkraster"
fi
if build cc -O2 -o "$BUILD/colortest" "$TOOLS/colortest.c" 2>/dev/null; then
    [ "$DRY" = 1 ] || say "built $BUILD/colortest"
fi
if [ -f /usr/include/libusb-1.0/libusb.h ]; then
    if build cc -O2 -o "$BUILD/lx900" "$DRIVER/lx900-backend.c" -lusb-1.0 2>/dev/null; then
        [ "$DRY" = 1 ] || say "built $BUILD/lx900 (ink level backend)"
    else
        [ "$DRY" = 1 ] || say "could not build the ink level backend, continuing"
    fi
fi

# ---------------------------------------------------------------- install

step "Installing filter and PPD"
sudo_run install -d "$FILTERDIR" "$PPDDIR"
sudo_run install -m 755 "$DRIVER/rastertoLX900" "$FILTER"
sudo_run install -m 644 "$DRIVER/CL900-linux.ppd" "$PPD"
say "filter: $FILTER"
say "ppd:    $PPD"

step "Installing ink level backend"
# Mode 744 so CUPS runs it as root, like the stock usb backend: it opens the
# USB device to read the status block, then execs the stock backend.
if [ -x "$BUILD/lx900" ]; then
    sudo_run install -d "$BACKENDDIR"
    sudo_run install -m 744 "$BUILD/lx900" "$BACKENDDIR/lx900"
    say "backend: $BACKENDDIR/lx900"
else
    say "no ink level backend built (libusb headers missing); skipping"
fi

# ---------------------------------------------------------------- queue

if [ "$DO_QUEUE" = 0 ]; then
    step "Skipping queue setup (--no-queue)"
    say "create one later with:"
    say "  sudo lpadmin -p $QUEUE -E -v usb://Primera/Color_Label_900 -m $PPD_NAME"
    exit 0
fi

step "Looking for the printer on USB"
# Some distributions install the CUPS USB backend root-only, so device
# discovery has to run with the privileges the backend expects.  The probe is
# only advisory: a queue that already has a device URI can still print, and the
# backend resolves that URI at job time.
if [ "$DRY" = 1 ]; then
    URIS=""
else
    URIS=$(sudo_run lpinfo -v 2>/dev/null \
           | awk '/^direct usb:\/\// {print $2}' || true)
fi

if [ -z "$URIS" ]; then
    URIS=$(lpstat -v "$QUEUE" 2>/dev/null \
           | sed -n 's/.*device for [^:]*: //p' || true)
    if [ -n "$URIS" ] && [ "$DRY" != 1 ]; then
        say "device probe found nothing; keeping the queue's existing URI"
        say "  $URIS"
    fi
fi

if [ -z "$URIS" ]; then
    say "No USB printer is currently visible to CUPS."
    if command -v lsusb >/dev/null 2>&1 && lsusb | grep -qi '0f25:0022'; then
        say ""
        say "The LX900e is on the USB bus (lsusb shows 0f25:0022) but CUPS has"
        say "no device URI for it.  Usually the USB backend could not be run."
        say "Restart CUPS and retry:"
        say "  sudo systemctl restart cups"
        say "  sudo $0"
    else
        say "Plug the LX900e in, power it on, then run:"
        say "  sudo $0"
    fi
    exit 1
fi

# Pick a Primera device if there is more than one, else the first.
URI=$(printf '%s\n' "$URIS" | grep -i 'primera\|lx900\|color_label' | head -n 1)
if [ -z "$URI" ]; then
    URI=$(printf '%s\n' "$URIS" | head -n 1)
    say "note: no Primera-named USB device, using $URI"
else
    say "found $URI"
fi

# Route the queue through our wrapper so ink levels get published.  The
# wrapper execs the stock usb backend, so a job path that worked before keeps
# working; strip any prefix the queue already has so reinstalls are idempotent.
if [ -x "$BUILD/lx900" ]; then
    URI="lx900:/$(printf '%s\n' "$URI" | sed 's|^lx900:/||')"
    say "queue will use the ink level backend: $URI"
fi

step "Creating printer queue $QUEUE"
if lpstat -p "$QUEUE" >/dev/null 2>&1; then
    say "queue exists, updating device URI and PPD"
    sudo_run lpadmin -p "$QUEUE" -E -v "$URI" -m "$PPD_NAME"
else
    sudo_run lpadmin -p "$QUEUE" -E -v "$URI" -m "$PPD_NAME"
fi

step "Applying defaults"
sudo_run lpadmin -p "$QUEUE" \
    -o printer-is-shared=false \
    -o Resolution=1200dpi \
    -o PageSize=4x3 \
    -o MediaType=HighGloss \
    -o PrintQuality=Quality4
sudo_run cupsenable "$QUEUE"
sudo_run cupsaccept "$QUEUE"

step "Done"
say "Queue:  $QUEUE"
say "The LX900e is a 1200 dpi device; the queue only offers 1200dpi."
say "Set the media size to match the labels you have loaded, e.g.:"
say "  lpadmin -p $QUEUE -o PageSize=4x6"
say "Test:   lp -d $QUEUE /path/to/label.pdf"
say "Status: lpstat -p $QUEUE -l"
say "Bring-up: tools/lx900-doctor.sh -n 120   (see docs/TEST-PLAN.md)"
