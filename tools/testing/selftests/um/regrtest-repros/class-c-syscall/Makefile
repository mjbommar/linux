# SPDX-License-Identifier: GPL-2.0
CFLAGS ?= -Wall -O2 -static
BINS := \
	ioctl_fionread \
	ioctl_tiocgwinsz_socketpair \
	ioctl_blkgetsize_loop \
	socket_options_dgram \
	socket_unix_abstract \
	socket_udplite \
	os_waitid_edges \
	os_sched_getcpu \
	os_setblocking \
	sanity_struct_unicode

all: $(BINS)

$(BINS): %: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BINS)

.PHONY: all clean
