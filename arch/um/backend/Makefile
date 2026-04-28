# SPDX-License-Identifier: GPL-2.0
#
# UML backend implementations.
#
# arch/um/include/asm/backend.h defines the typed ops table; subdirs
# implement it. Workstream A is migrating ops one at a time from
# arch/um/{kernel,os-Linux}/skas/. See
# Documentation/virt/uml/redesign/02-workstreams/A-backend-abstraction/.

obj-$(CONFIG_UM_BACKEND_SECCOMP) += seccomp/
# v1 KVM backend archived to arch/um/backend/kvm-v1-archive/ (memo 25
# Part 1). Not built — UM_BACKEND_KVM_V1_ARCHIVE depends on BROKEN.
obj-$(CONFIG_UM_BACKEND_KVM_V2)  += kvm-v2/
obj-y                            += common/
obj-$(CONFIG_UM_BACKEND_CONTRACT_TEST) += contract/
