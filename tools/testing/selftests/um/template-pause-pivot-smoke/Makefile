# SPDX-License-Identifier: GPL-2.0
#
# um/template-pause-pivot-smoke - template-pause stack-pivot smoke test.
#
# Verifies the os_template_pause_fork_clone_to() primitive crosses
# the fork child runs kernel C code on a private
# MAP_PRIVATE stack and writes "PIVOT_OK" to host fd 1 without the
# saved-RIP-corruption panic at um_template_pause_enter+0xf6.

TEST_PROGS := run-template-pause-pivot-smoke.sh

include ../../lib.mk
