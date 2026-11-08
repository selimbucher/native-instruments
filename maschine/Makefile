# ni-maschine, native: needs alsa-lib. font.h comes from fonts.py (Spleen).

CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter
PKG_CONFIG ?= pkg-config
ALSA_CFLAGS := $(shell $(PKG_CONFIG) --cflags alsa)
ALSA_LIBS := $(shell $(PKG_CONFIG) --libs alsa)
DATADIR = ../src/ni_wine/data

COMMON = engine.c template.c xml.c layout.c
SRCS = main.c screen.c mk3.c seq.c $(COMMON)

all: ni-maschine

ni-maschine: $(SRCS) $(wildcard *.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ALSA_CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(ALSA_LIBS) -lm

test: test.c screen.c mk3.c $(COMMON) $(wildcard *.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ test.c screen.c mk3.c $(COMMON) -lm

check: test
	./test

install: ni-maschine
	install -m755 ni-maschine $(DATADIR)

clean:
	rm -f ni-maschine test

.PHONY: all check install clean
