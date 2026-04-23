# SPDX-License-Identifier: GPL-2.0
CFLAGS  := -O2 -Wall -Wextra -std=gnu11

spike: spike.c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: run clean
run: spike
	@./spike
clean:
	rm -f spike
