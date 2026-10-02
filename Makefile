CC      ?= gcc
CFLAGS  ?= -O2
WARN    := -Wall -Wextra
PREFIX  ?= /usr/local

DAEMON  := battery-monitord
PANEL   := battery-monitor-panel

# --- daemon: GLib + ALSA + mpg123 only (no GTK) ---------------------------
D_PKGS    := glib-2.0 gio-2.0 gio-unix-2.0 alsa libmpg123
D_CFLAGS  := $(shell pkg-config --cflags $(D_PKGS))
D_LIBS    := $(shell pkg-config --libs $(D_PKGS)) -pthread

# --- panel: GTK4 (+ layer-shell when available) ----------------------------
P_PKGS := gtk4 gio-2.0
LAYER_SHELL_PC := $(shell pkg-config --exists gtk4-layer-shell-0 && echo gtk4-layer-shell-0)
ifeq ($(LAYER_SHELL_PC),gtk4-layer-shell-0)
    P_PKGS += gtk4-layer-shell-0
    P_EXTRA := -DUSE_LAYER_SHELL
endif
P_CFLAGS := $(shell pkg-config --cflags $(P_PKGS)) $(P_EXTRA)
P_LIBS   := $(shell pkg-config --libs $(P_PKGS))

.PHONY: all clean install uninstall
all: $(DAEMON) $(PANEL)

$(DAEMON): battery-monitord.c bm_audio.c bm_audio.h bm_common.h
	$(CC) $(CFLAGS) $(WARN) $(D_CFLAGS) -o $@ battery-monitord.c bm_audio.c $(D_LIBS) -Wl,--as-needed

$(PANEL): battery-monitor-panel.c bm_common.h
	$(CC) $(CFLAGS) $(WARN) $(P_CFLAGS) -o $@ battery-monitor-panel.c $(P_LIBS) -Wl,--as-needed

install: all
	install -Dm755 $(DAEMON) $(DESTDIR)$(PREFIX)/bin/$(DAEMON)
	install -Dm755 $(PANEL)  $(DESTDIR)$(PREFIX)/bin/$(PANEL)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(DAEMON) $(DESTDIR)$(PREFIX)/bin/$(PANEL)

clean:
	rm -f $(DAEMON) $(PANEL)
