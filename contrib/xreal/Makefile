# Build the standalone xreal-ctl HID helper (WP-XR1). Needs hidapi (hidapi-hidraw pkg-config module).
#   make            build ./xreal-ctl
#   make install    install to $(PREFIX)/bin (default /usr/local; needs write access)
#   make clean

PREFIX ?= /usr/local
CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra

HIDAPI_CFLAGS := $(shell pkg-config --cflags hidapi-hidraw 2>/dev/null)
HIDAPI_LIBS   := $(shell pkg-config --libs hidapi-hidraw 2>/dev/null)

ifeq ($(strip $(HIDAPI_LIBS)),)
$(error hidapi-hidraw not found via pkg-config. Install hidapi (Arch: pacman -S hidapi).)
endif

xreal-ctl: xreal-ctl.c
	$(CC) $(CFLAGS) $(HIDAPI_CFLAGS) -o $@ $< $(HIDAPI_LIBS)

install: xreal-ctl
	install -Dm755 xreal-ctl $(DESTDIR)$(PREFIX)/bin/xreal-ctl

clean:
	rm -f xreal-ctl

.PHONY: install clean
