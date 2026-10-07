# OpenRS -- build with GNU make (Linux, macOS, MinGW on Windows)
#
#   make                       build openrs (openrs.exe on Windows)
#   make test                  build and run the protocol tests (needs python3)
#   make install               install to $(PREFIX)/bin, default /usr/local
#   make clean
#
# Variables:
#   CC, CFLAGS, LDFLAGS        compiler and flags
#   VERSION                    version shown in the usage text
#                              (default: git describe, or "dev")
#   EXE                        executable suffix, .exe on Windows
#   RUNNER                     runs the binary in "make test", e.g. wine or
#                              qemu-aarch64-static for a cross-built binary
#
# Cross-compiling, e.g. with zig:
#   make CC="zig cc -target aarch64-linux-musl" LDFLAGS=-static
#   make CC="zig cc -target x86_64-windows-gnu" EXE=.exe
#   make test EXE=.exe RUNNER=wine

CFLAGS ?= -O2 -Wall
PREFIX ?= /usr/local

ifeq ($(OS),Windows_NT)
  EXE ?= .exe
  # MinGW has gcc but usually no cc
  ifeq ($(origin CC),default)
    CC = gcc
  endif
endif

VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null)
ifneq ($(VERSION),)
  DEFS = -DOPENRS_VERSION=\"$(VERSION)\"
endif

TARGET  = openrs$(EXE)
SOURCES = $(wildcard src/*.c)
HEADERS = $(wildcard src/*.h)

.PHONY: all test install clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(DEFS) $(CPPFLAGS) -o $@ $(SOURCES) $(LDFLAGS)

test: $(TARGET)
	python3 tests/protocol_test.py ./$(TARGET) $(RUNNER)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -f openrs openrs.exe openrs.pdb
