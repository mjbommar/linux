# SPDX-License-Identifier: GPL-2.0
CFLAGS ?= -Wall -O2 -static
BINS := fork_exec_wait fork_pipe_ipc pool_workers waitpid_wnohang sigchld_select asyncio_subprocess_min

all: $(BINS)

$(BINS): %: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BINS)

.PHONY: all clean
