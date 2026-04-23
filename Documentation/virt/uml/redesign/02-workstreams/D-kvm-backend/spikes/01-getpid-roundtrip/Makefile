# SPDX-License-Identifier: GPL-2.0
CFLAGS  := -O2 -Wall -Wextra -std=gnu11
LDFLAGS :=

spike: spike.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

.PHONY: run
run: spike
	@./spike

.PHONY: clean
clean:
	rm -f spike
