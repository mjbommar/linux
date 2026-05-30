/* SPDX-License-Identifier: GPL-2.0 */
/*
 * APERF/MPERF MSR passthrough demo — minimal freestanding init.
 *
 * What this demo proves:
 *
 *   1. UML's kvm-v2 backend issued KVM_ENABLE_CAP for
 *      KVM_CAP_X86_DISABLE_EXITS with the
 *      KVM_X86_DISABLE_EXITS_APERFMPERF bit during VM creation.
 *   2. The KVM kernel accepted the cap (ioctl_rc == 0).
 *   3. The verdict matches the operator's intent (toggle=on with
 *      the boot param, or toggle=off with kvm_v2_aperfmperf=off).
 *
 * What this demo does NOT prove:
 *
 *   That a guest-side rdmsr on 0xE7/0xE8 returns non-zero counters.
 *   UML's "kernel" code runs at host CPL=3 (architecturally — UML
 *   is a userspace VMM), and rdmsr always #GPs there.  The only
 *   code that runs at guest CPL=0 inside the KVM guest is the LSTAR
 *   gadget and the IDT/exception stubs; exposing rdmsr through the
 *   gadget would require an additional custom NR (out of scope
 *   here).  For a regular Linux guest running under QEMU (which is
 *   Anderson's case), the guest kernel runs at guest CPL=0 and
 *   benefits directly from the cap.  See ../../aperf-mperf.rst for
 *   the bridging notes.
 *
 *   So this demo establishes that the architectural plumbing is
 *   correct — the bit the QEMU patch is missing.
 *
 * Designed to run as `init=` under UML with no rootfs image and no
 * libc — same minimal-ELF pattern as the existing mm-smoke-loop
 * selftest at tools/testing/selftests/um/kvm-mm-smoke/.
 *
 * Expected output on a kvm-v2 build with
 * CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH=y and
 * kvm_v2_aperfmperf=on on the cmdline:
 *
 *   APERF_MPERF_DEMO: toggle=on
 *   APERF_MPERF_DEMO: ioctl_attempted=1
 *   APERF_MPERF_DEMO: ioctl_rc=0
 *   APERF_MPERF_DEMO: host_feature_aperfmperf=1
 *   APERF_MPERF_DEMO: status=enabled
 *   APERF_MPERF_DEMO: PASS plumbing_ok=1
 *
 * With kvm_v2_aperfmperf=off:
 *
 *   APERF_MPERF_DEMO: toggle=off
 *   APERF_MPERF_DEMO: ioctl_attempted=0
 *   APERF_MPERF_DEMO: ioctl_rc=0
 *   APERF_MPERF_DEMO: host_feature_aperfmperf=1
 *   APERF_MPERF_DEMO: status=disabled
 *   APERF_MPERF_DEMO: FAIL plumbing_ok=0 reason=cap_not_requested
 *
 * See README.md alongside this file for the run instructions and a
 * captured run-output.log.
 */

#include <stdint.h>

#define __NR_read		0
#define __NR_write		1
#define __NR_close		3
#define __NR_mkdir		83
#define __NR_mount		165
#define __NR_exit_group		231
#define __NR_openat		257

#define AT_FDCWD		(-100)
#define O_RDONLY		0x0
#define STDOUT_FD		1

#define PROBE_MOUNT		"/tmp/debug"
#define PROBE_PATH		"/tmp/debug/um/kvm_v2/aperf_mperf"

static long sys6(long nr, long a, long b, long c, long d, long e, long f)
{
	long r;
	register long r10 __asm__("r10") = d;
	register long r8  __asm__("r8")  = e;
	register long r9  __asm__("r9")  = f;
	__asm__ volatile (
		"syscall"
		: "=a"(r)
		: "0"(nr), "D"(a), "S"(b), "d"(c),
		  "r"(r10), "r"(r8), "r"(r9)
		: "rcx", "r11", "memory"
	);
	return r;
}

#define sys1(nr,a)		sys6((nr),(long)(a),0,0,0,0,0)
#define sys3(nr,a,b,c)		sys6((nr),(long)(a),(long)(b),(long)(c),0,0,0)
#define sys4(nr,a,b,c,d)	sys6((nr),(long)(a),(long)(b),(long)(c),(long)(d),0,0)
#define sys5(nr,a,b,c,d,e)	sys6((nr),(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),0)

static unsigned int append_str(char *buf, unsigned int off, const char *s)
{
	while (*s)
		buf[off++] = *s++;
	return off;
}

static unsigned int append_int(char *buf, unsigned int off, long v)
{
	char tmp[24];
	unsigned int n = 0;
	unsigned long u;

	if (v < 0) {
		buf[off++] = '-';
		u = (unsigned long)(-v);
	} else {
		u = (unsigned long)v;
	}
	if (u == 0) {
		buf[off++] = '0';
		return off;
	}
	while (u) {
		tmp[n++] = '0' + (u % 10);
		u /= 10;
	}
	while (n--)
		buf[off++] = tmp[n];
	return off;
}

static int eq_str(const char *p, const char *end, const char *want)
{
	while (p < end && *want) {
		if (*p != *want)
			return 0;
		p++;
		want++;
	}
	return *want == '\0' && p == end;
}

struct probe {
	int		toggle_on;
	int		ioctl_attempted;
	long		ioctl_rc;
	int		host_feature;
	char		status[24];
};

static long parse_long(const char *p, const char *end)
{
	long v = 0;
	int neg = 0;

	if (p < end && *p == '-') {
		neg = 1;
		p++;
	}
	while (p < end && *p >= '0' && *p <= '9') {
		v = v * 10 + (long)(*p - '0');
		p++;
	}
	return neg ? -v : v;
}

static void copy_status(const char *p, const char *end, char *dst,
			unsigned int dst_sz)
{
	unsigned int i = 0;
	while (p < end && i < dst_sz - 1) {
		dst[i++] = *p++;
	}
	dst[i] = '\0';
}

/*
 * Parse the probe.  Each line: key=value\n.  We care about toggle,
 * ioctl_attempted, ioctl_rc, host_feature_aperfmperf, status.
 */
static void parse_probe(const char *buf, unsigned int len, struct probe *out)
{
	const char *p = buf;
	const char *end = buf + len;

	out->toggle_on = 0;
	out->ioctl_attempted = 0;
	out->ioctl_rc = 0;
	out->host_feature = 0;
	out->status[0] = '\0';

	while (p < end) {
		const char *line_end = p;
		const char *eq;

		while (line_end < end && *line_end != '\n')
			line_end++;
		eq = p;
		while (eq < line_end && *eq != '=')
			eq++;
		if (eq == line_end || eq + 1 == line_end)
			goto next_line;

		if (eq_str(p, eq, "toggle"))
			out->toggle_on = eq_str(eq + 1, line_end, "on");
		else if (eq_str(p, eq, "ioctl_attempted"))
			out->ioctl_attempted =
				(parse_long(eq + 1, line_end) != 0);
		else if (eq_str(p, eq, "ioctl_rc"))
			out->ioctl_rc = parse_long(eq + 1, line_end);
		else if (eq_str(p, eq, "host_feature_aperfmperf"))
			out->host_feature =
				(parse_long(eq + 1, line_end) != 0);
		else if (eq_str(p, eq, "status"))
			copy_status(eq + 1, line_end, out->status,
				    sizeof(out->status));

next_line:
		p = line_end;
		if (p < end)
			p++;
	}
}

static int read_probe(struct probe *out)
{
	char buf[512];
	long fd, rc;
	unsigned int total = 0;

	fd = sys4(__NR_openat, AT_FDCWD, PROBE_PATH, O_RDONLY, 0);
	if (fd < 0)
		return (int)fd;
	for (;;) {
		rc = sys3(__NR_read, fd, buf + total,
			  sizeof(buf) - 1 - total);
		if (rc <= 0)
			break;
		total += (unsigned int)rc;
		if (total >= sizeof(buf) - 1)
			break;
	}
	sys1(__NR_close, fd);
	parse_probe(buf, total, out);
	return 0;
}

static void mount_debugfs(void)
{
	/*
	 * Under hostfs root the host's /sys is not exported.  Mount
	 * debugfs at /tmp/debug instead — /tmp exists on every host
	 * filesystem.  Both syscalls are best-effort: mkdir returns
	 * -EEXIST if the dir already exists, mount returns -EBUSY if
	 * something is already mounted.  Errors surface as a failed
	 * openat below.
	 */
	(void)sys3(__NR_mkdir, PROBE_MOUNT, 0755, 0);
	(void)sys5(__NR_mount, "debugfs", PROBE_MOUNT, "debugfs", 0UL, 0);
}

int main(void)
{
	char out[1024];
	unsigned int n = 0;
	struct probe p;
	int rc;
	int plumbing_ok;
	const char *reason = (const char *)0;

	mount_debugfs();

	rc = read_probe(&p);
	if (rc < 0) {
		n = append_str(out, n,
			"APERF_MPERF_DEMO: FAIL probe_open_rc=");
		n = append_int(out, n, rc);
		n = append_str(out, n,
			" reason=debugfs_or_kconfig_missing\n");
		sys3(__NR_write, STDOUT_FD, (long)out, n);
		sys3(__NR_exit_group, 1, 0, 0);
		return 1;
	}

	n = append_str(out, n, "APERF_MPERF_DEMO: toggle=");
	n = append_str(out, n, p.toggle_on ? "on" : "off");
	n = append_str(out, n, "\n");

	n = append_str(out, n, "APERF_MPERF_DEMO: ioctl_attempted=");
	n = append_int(out, n, p.ioctl_attempted);
	n = append_str(out, n, "\n");

	n = append_str(out, n, "APERF_MPERF_DEMO: ioctl_rc=");
	n = append_int(out, n, p.ioctl_rc);
	n = append_str(out, n, "\n");

	n = append_str(out, n, "APERF_MPERF_DEMO: host_feature_aperfmperf=");
	n = append_int(out, n, p.host_feature);
	n = append_str(out, n, "\n");

	n = append_str(out, n, "APERF_MPERF_DEMO: status=");
	n = append_str(out, n, p.status);
	n = append_str(out, n, "\n");

	/*
	 * Verdict: PASS iff the operator requested passthrough AND KVM
	 * accepted it.
	 */
	plumbing_ok = (p.toggle_on && p.ioctl_attempted &&
		       p.ioctl_rc == 0);

	if (!plumbing_ok) {
		if (!p.toggle_on)
			reason = "passthrough_off_by_cmdline";
		else if (!p.ioctl_attempted)
			reason = "cap_not_requested";
		else if (!p.host_feature)
			reason = "host_no_feature";
		else
			reason = "kvm_rejected_cap";
	}

	n = append_str(out, n, "APERF_MPERF_DEMO: ");
	n = append_str(out, n, plumbing_ok ? "PASS" : "FAIL");
	n = append_str(out, n, " plumbing_ok=");
	n = append_int(out, n, plumbing_ok);
	if (reason) {
		n = append_str(out, n, " reason=");
		n = append_str(out, n, reason);
	}
	n = append_str(out, n, "\n");

	sys3(__NR_write, STDOUT_FD, (long)out, n);
	sys3(__NR_exit_group, plumbing_ok ? 0 : 1, 0, 0);
	return 0;
}

__asm__ (
	".text\n.globl _start\n_start:\n"
	"\txor %rbp, %rbp\n"
	"\tcall main\n"
	"\tmov %rax, %rdi\n"
	"\tmov $231, %rax\n"
	"\tsyscall\n\thlt\n"
);
