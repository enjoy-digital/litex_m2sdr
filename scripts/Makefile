# Build the scripts/ hardware test utilities against the IN-REPO libm2sdr
# (never the installed copy). These are standalone loopback/latency harnesses,
# kept out of the main software/user build because they are developer tools.
#
# Build libm2sdr.a first, then the tools:
#   make -C litex_m2sdr/software/user            # produces libm2sdr.a
#   make -C scripts                              # builds all tools here
#   make -C scripts timed_tx_selftest            # or just one
#   sudo ./scripts/timed_tx_selftest --chan 2 --rate 30.72e6 --lead-us 8000
#
# loopback_selftest.sh builds loopback_selftest.c on its own too (it is the
# config-sweep runner); this Makefile just gives every scripts/ tool a plain
# `make` target.

REPO    := $(CURDIR)/..
LIBDIR  := $(REPO)/litex_m2sdr/software/user/libm2sdr
KERNDIR := $(REPO)/litex_m2sdr/software/kernel
LIB     := $(LIBDIR)/libm2sdr.a

CFLAGS  ?= -O2 -Wall -Wextra
CPPFLAGS += -I$(LIBDIR) -I$(KERNDIR)
LDLIBS  := $(LIB) -lm -lpthread

TOOLS := timed_tx_selftest latency_probe loopback_selftest

all: $(TOOLS)

$(TOOLS): %: %.c $(LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDLIBS)

$(LIB):
	@echo "error: $(LIB) not built -- run 'make -C litex_m2sdr/software/user' first" >&2; \
	 exit 1

clean:
	rm -f $(TOOLS)

.PHONY: all clean
