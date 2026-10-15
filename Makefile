# Makefile for viewdat - Ngspice .dat waveform viewer
#
#   make            build both tools (viewdat + viewdat-gui)
#   make viewdat    terminal viewer only (needs just gcc + libc + libm)
#   make viewdat-gui  GUI viewer (needs GTK+ 3 and cairo, via pkg-config)
#   make clean

CC      ?= gcc
BASEFLAGS = -Wall -Wextra -O2 -std=c99 -D_POSIX_C_SOURCE=200809L

GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0)

.PHONY: all clean install

all: viewdat viewdat-gui

# Terminal viewer
viewdat: src/viewdat.c src/datreader.c src/datreader.h
	$(CC) $(BASEFLAGS) -o $@ src/viewdat.c src/datreader.c -lm

# GUI viewer (GTK3 + Cairo)
viewdat-gui: src/viewdat-gui.c src/datreader.c src/datreader.h
	$(CC) $(BASEFLAGS) $(GTK_CFLAGS) -o $@ src/viewdat-gui.c src/datreader.c $(GTK_LIBS) -lm

clean:
	rm -f viewdat viewdat-gui

PREFIX ?= /usr/local
install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 viewdat viewdat-gui $(DESTDIR)$(PREFIX)/bin/
