CC = gcc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
PREFIX ?= $(HOME)
BINDIR ?= $(PREFIX)/bin
BACKEND ?= fast
CARGO ?= cargo
CARGO_FLAGS ?=

ifeq ($(BACKEND),fast)
READER_LIB = target/release/libtgzcat_reader.a
READER_CPPFLAGS = -DTGZCAT_FAST_READER
READER_LIBS = $(READER_LIB) -ldl -lpthread -lm -lz
READER_LDFLAGS = -Wl,--gc-sections -s
else ifeq ($(BACKEND),zlib)
READER_LIB =
READER_CPPFLAGS =
READER_LIBS = -lz
else
$(error BACKEND must be fast or zlib)
endif

.PHONY: all install clean check FORCE
all: tgzcat

# Relink when switching BACKEND; the C compilation is small.
tgzcat: tgzcat.c reader.h cache.h cli.h seek_index.h $(READER_LIB) FORCE
	$(CC) $(CPPFLAGS) $(READER_CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(READER_LDFLAGS) -o $@ $< $(READER_LIBS) $(LDLIBS)

target/release/libtgzcat_reader.a: Cargo.toml Cargo.lock reader.rs
	$(CARGO) build --release --locked --lib $(CARGO_FLAGS)

install: tgzcat
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 tgzcat "$(DESTDIR)$(BINDIR)/tgzcat"

check: tgzcat
	python3 test_tgzcat.py $(if $(ARCHIVE),"$(ARCHIVE)") $(if $(BASELINE),"$(BASELINE)")

clean:
	$(RM) tgzcat

FORCE:
