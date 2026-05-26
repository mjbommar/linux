# SPDX-License-Identifier: GPL-2.0
#
# um/snapshot-elf-roundtrip kselftest (#181).
#
# Runner is a shell script that boots UML, triggers an ELF64 snapshot
# export via debugfs, and validates the dump file via readelf / gdb
# (the latter optional, SKIP-clean if gdb is missing). Nothing to
# compile here.

TEST_PROGS := run-snapshot-elf-roundtrip.sh

include ../../lib.mk
