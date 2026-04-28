# SPDX-License-Identifier: GPL-2.0
CFLAGS ?= -Wall -O2 -static
BINS := terminal_size openpty tty_isatty controlling_tty termios_get termios_mode_probe

all: $(BINS)

$(BINS): %: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BINS)

.PHONY: all clean
