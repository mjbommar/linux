# SPDX-License-Identifier: GPL-2.0
#
# um/umlbuild — end-to-end gate for `umlbuild instance --profile mvp`.
#
# Builds the mvp profile, then asserts the guest boots from its
# ubd-attached rootfs image and runs the cpython-tier0 hashlib check
# both under direct kernel invocation and under `umlctl up`.

TEST_PROGS := run-umlbuild-mvp.sh

include ../../lib.mk
