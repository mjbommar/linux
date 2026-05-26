# SPDX-License-Identifier: GPL-2.0
# These reproducers are intentionally CPU-bound busy loops with
# sigaction-driven counters. -O2 is fine because the spin counter and
# elapsed-time check are volatile, but the inner loop must not be
# elided. -static so it runs inside a minimal UML guest rootfs.
CFLAGS ?= -Wall -O2 -static
LDFLAGS ?=
BINS := itimer_virtual itimer_prof itimer_real getrusage_split

all: $(BINS)

$(BINS): %: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BINS)

.PHONY: all clean
