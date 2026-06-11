# SPDX-License-Identifier: GPL-2.0
TARGETS = aperf-mperf-smoke bpf-jit-smoke rdpmc-smoke cpython-parity cpython-full cpython-tier0 cve-repro df-preserve dyn-loader ftrace-smoke hooks-flip kcov-smoke kmsan-smoke kprobes-stress kvm-bounds kvm-mm-smoke kvm-record-clock-bench kvm-record-smoke kvm-smoke kvm-snapshot-bench kvm-snapshot-restore-smoke launcher-smoke perf-fallback perf-getpid perf-pidfam pool-bench pool-exec-smoke pool-mconsole-path-probe pool-port-forward-smoke pool-serve-smoke pool-spawn-smoke profiles regrtest-repros rt-sigreturn-isolation snapshot-elf-roundtrip snapshot-kvm-smoke snapshot-smoke syzkaller-shim-smoke template-pause-fork-smoke template-pause-fork-stress template-pause-pivot-smoke template-pause-pool-member-smoke template-pause-pool-sustained-smoke template-pause-smoke umlbuild umlctl-smoke userspace-smoke vector2-fd-handoff-smoke vector2-fd-multiqueue-smoke vector2-inproc-tap-smoke vector2-pool-tap-smoke vector2-sandbox-audit
include ../lib.mk

UM_SELFTEST_OUTPUT := $(if $(OUTPUT),$(abspath $(OUTPUT)),$(CURDIR))

all:
	@set -e; for TARGET in $(TARGETS); do				\
		$(MAKE) -C $$TARGET OUTPUT=$(UM_SELFTEST_OUTPUT)/$$TARGET;	\
	done;

clean:
	@set -e; for TARGET in $(TARGETS); do				\
		$(MAKE) -C $$TARGET OUTPUT=$(UM_SELFTEST_OUTPUT)/$$TARGET clean;	\
	done;
