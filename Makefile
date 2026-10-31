# lx900e-cups - Linux driver and CUPS 3 printer application for the
# Primera LX900e colour label printer.
#
# SPDX-License-Identifier: MIT

VERSION        := 1.0.0
PREFIX         ?= /usr/local
DESTDIR        ?=
CC             ?= cc
PKG_CONFIG     ?= pkg-config

CUPS_CFLAGS    := $(shell $(PKG_CONFIG) --cflags cups 2>/dev/null)
CUPS_LIBS      := $(shell $(PKG_CONFIG) --libs cups 2>/dev/null)
PAPPL_CFLAGS   := $(shell $(PKG_CONFIG) --cflags pappl 2>/dev/null)
PAPPL_LIBS     := $(shell $(PKG_CONFIG) --libs pappl 2>/dev/null)
# papplSystemCreatePrinters() arrived in PAPPL 1.4.  On older versions the
# application enumerates USB devices itself.
PAPPL_14       := $(shell $(PKG_CONFIG) --atleast-version=1.4 pappl 2>/dev/null && echo yes)
MANDIR         ?= $(PREFIX)/share/man
SYSTEMD_UNIT_DIR ?= $(PREFIX)/lib/systemd/system
APPDATA_DIR    ?= $(PREFIX)/share/lx900e-app

BUILD_DIR      := build
APP            := $(BUILD_DIR)/lx900e-app
SERVICE        := $(BUILD_DIR)/lx900e-app.service

# `make pappl` probes USB and bakes the URI (including the serial number) of a
# connected printer into the systemd unit, so the service starts bound to it.
# DEVICE_URI overrides the probe; a non-empty DESTDIR (packaging) never probes
# because the build host's printer must not leak into the package.
DEVICE_URI     ?=

CPPFLAGS       += -Iinclude -D_GNU_SOURCE -DLX900_APP_VERSION=\"$(VERSION)\"
ifeq ($(PAPPL_14),yes)
CPPFLAGS       += -DHAVE_PAPPL_CREATE_PRINTERS
endif
CFLAGS         ?= -O2 -g
CFLAGS         += -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
                  -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith

# The status query needs libusb.  Without it the application still builds and
# prints; it just cannot report ink levels.
LIBUSB_CFLAGS  := $(shell $(PKG_CONFIG) --cflags libusb-1.0 2>/dev/null)
LIBUSB_LIBS    := $(shell $(PKG_CONFIG) --libs libusb-1.0 2>/dev/null)
ifeq ($(strip $(LIBUSB_LIBS)),)
LIBUSB_CFLAGS  :=
LIBUSB_LIBS    :=
else
CPPFLAGS       += -DHAVE_LIBUSB $(LIBUSB_CFLAGS)
endif

.PHONY: all driver pappl detect test install install-pappl uninstall clean help FORCE

all: driver pappl

driver:
	$(MAKE) -C driver

pappl: $(APP) $(SERVICE)

$(APP): pappl/lx900-app.c src/lx900_encoder.c src/lx900_status.c \
        include/lx900.h include/lx900_status.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CUPS_CFLAGS) $(PAPPL_CFLAGS) \
		-o $@ pappl/lx900-app.c src/lx900_encoder.c src/lx900_status.c \
		$(PAPPL_LIBS) $(CUPS_LIBS) $(LIBUSB_LIBS) -lm

$(SERVICE): pappl/lx900e-app.service.in $(APP) tools/gen-service.awk FORCE
	@mkdir -p $(dir $@)
	@if [ -n "$(DEVICE_URI)" ]; then \
	    dev='$(DEVICE_URI)'; \
	elif [ -z "$(DESTDIR)" ]; then \
	    dev=$$($(APP) --detect 2>/dev/null | head -n 1); \
	else \
	    dev=; \
	fi; \
	if [ -n "$$dev" ]; then \
	    args=" --device '$$dev' --printer LX900e"; \
	    echo "  binding printer: $$dev"; \
	else \
	    args=; \
	fi; \
	awk -v bindir="$(PREFIX)/bin" -v args="$$args" \
	    -f tools/gen-service.awk $< > $@

detect: $(APP)
	@$(APP) --detect || { \
	    echo "no LX900e found on USB; connect it and try again" >&2; exit 1; }

test:
	tests/run.sh

install: all
	$(MAKE) -C driver PREFIX=$(PREFIX) DESTDIR=$(DESTDIR) install
	$(MAKE) install-pappl

install-pappl: pappl
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(APP) $(DESTDIR)$(PREFIX)/bin/lx900e-app
	install -d $(DESTDIR)$(MANDIR)/man1
	install -m 0644 man/lx900e-app.1 $(DESTDIR)$(MANDIR)/man1/
	install -D -m 0644 $(SERVICE) \
		$(DESTDIR)$(SYSTEMD_UNIT_DIR)/lx900e-app.service
	install -D -m 0644 pappl/lx900.css $(DESTDIR)$(APPDATA_DIR)/style.css
	@if [ -z "$(DESTDIR)" ]; then \
	    echo "installed; run 'systemctl daemon-reload && systemctl enable --now lx900e-app'"; \
	fi

uninstall:
	-$(MAKE) -C driver uninstall
	-rm -f $(DESTDIR)$(PREFIX)/bin/lx900e-app
	-rm -f $(DESTDIR)$(MANDIR)/man1/lx900e-app.1
	-rm -f $(DESTDIR)$(SYSTEMD_UNIT_DIR)/lx900e-app.service
	-rm -f $(DESTDIR)$(APPDATA_DIR)/style.css

clean:
	$(MAKE) -C driver clean
	rm -rf $(BUILD_DIR)

help:
	@echo "make            build the CUPS 2 filter, PPD and the printer application"
	@echo "make driver     build only the CUPS 2 filter and PPD"
	@echo "make pappl      build only the CUPS 3 printer application"
	@echo "make detect     print the USB URI and serial number of a connected printer"
	@echo "make test       run the printer-free test suites"
	@echo "make install    install everything (PREFIX=$(PREFIX))"
	@echo "make install-pappl  install only the printer application"
