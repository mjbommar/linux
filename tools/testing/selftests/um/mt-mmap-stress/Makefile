# SPDX-License-Identifier: GPL-2.0
all: mt-mmap-stress mt-mini

mt-mmap-stress: mt-mmap-stress.c
	gcc -static -O0 -pthread -o $@ $<

mt-mini: mt-mini.c
	gcc -static -O0 -pthread -o $@ $<

clean:
	rm -f mt-mmap-stress mt-mini
