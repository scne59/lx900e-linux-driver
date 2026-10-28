CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra
PREFIX   ?= /usr
FILTERDIR ?= $(PREFIX)/lib/cups/filter
PPDDIR    ?= $(PREFIX)/share/cups/model

all: rastertoLX900 lx900 CL900-linux.ppd

rastertoLX900: rastertoLX900.c
	$(CC) $(CFLAGS) -o $@ $< -lcups

# CUPS backend wrapper that publishes ink levels, then execs the usb backend.
# Optional: without libusb headers the queue simply keeps using usb directly.
lx900: lx900-backend.c
	@if [ -f /usr/include/libusb-1.0/libusb.h ]; then \
	    $(CC) $(CFLAGS) -o $@ $< -lusb-1.0; \
	else \
	    echo "libusb-1.0 headers not found, skipping the lx900 backend"; \
	fi

# The PPD is generated from this project's own table, not patched from the
# vendor's macOS PPD.  driver/extra-sizes, if present, adds more page sizes.
EXTRA = $(if $(wildcard extra-sizes),extra-sizes,)

CL900-linux.ppd: ../tools/gen-ppd.py $(wildcard extra-sizes)
	python3 ../tools/gen-ppd.py -o $@ $(EXTRA)

install: all
	install -d $(DESTDIR)$(FILTERDIR) $(DESTDIR)$(PPDDIR)
	install -m 755 rastertoLX900 $(DESTDIR)$(FILTERDIR)/rastertoLX900
	install -m 644 CL900-linux.ppd $(DESTDIR)$(PPDDIR)/Primera-LX900e.ppd

uninstall:
	rm -f $(DESTDIR)$(FILTERDIR)/rastertoLX900
	rm -f $(DESTDIR)$(PPDDIR)/Primera-LX900e.ppd

clean:
	rm -f rastertoLX900 lx900 CL900-linux.ppd

.PHONY: all install uninstall clean
