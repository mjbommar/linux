/* SPDX-License-Identifier: GPL-2.0 */
/*
 * APERF/MPERF MSR passthrough demo — minimal freestanding init.
 *
 * Two complementary checks emitted in one run:
 *
 *   1. PLUMBING — read /tmp/debug/um/kvm_v2/aperf_mperf and confirm
 *      vm_create issued KVM_ENABLE_CAP and KVM accepted the cap
 *      (toggle=on, ioctl_attempted=1, ioctl_rc=0, status=enabled).
 *
 *   2. VALUES   — call the UML-private LSTAR gadget at NR=0xc0de
 *      (KVM_V2_NR_UML_APERFMPERF) to read IA32_APERF (0xE7) and
 *      IA32_MPERF (0xE8).  The gadget body executes the rdmsrs at
 *      guest CPL=0 inside the KVM guest; with the cap enabled the
 *      reads pass through to hardware, with the cap disabled KVM
 *      emulates them as zero.  The demo distinguishes the two by
 *      the non-zero / zero split.
 *
 * Designed to run as `init=` under UML with hostfs root, no rootfs
 * image, no libc — same minimal-ELF pattern as the existing
 * mm-smoke-loop selftest at tools/testing/selftests/um/kvm-mm-smoke/.
 *
 * Expected output on a kvm-v2 build with
 * CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH=y +
 * CONFIG_UM_BACKEND_KVM_V2_GADGET=y and kvm_v2_aperfmperf=on:
 *
 *   APERF_MPERF_DEMO: toggle=on
 *   APERF_MPERF_DEMO: ioctl_attempted=1
 *   APERF_MPERF_DEMO: ioctl_rc=0
 *   APERF_MPERF_DEMO: host_feature_aperfmperf=1
 *   APERF_MPERF_DEMO: status=enabled
 *   APERF_MPERF_DEMO: sample1 aperf=<N1> mperf=<M1>
 *   APERF_MPERF_DEMO: sample2 aperf=<N2> mperf=<M2>
 *   APERF_MPERF_DEMO: delta   aperf=<dN> mperf=<dM>
 *   APERF_MPERF_DEMO: ratio_pct=<R1> (sample1) <R2> (sample2) <Rd> (delta)
 *   APERF_MPERF_DEMO: PASS plumbing_ok=1 counters_nonzero=1 counters_advanced=1
 *
 * Counter-side checks fall back to a sensible FAIL line if the
 * Kconfig is set but the gadget is off, or if the cap-enable was
 * dropped, etc.
 */

#include <stdint.h>

#define __NR_read		0
#define __NR_write		1
#define __NR_close		3
#define __NR_mkdir		83
#define __NR_mount		165
#define __NR_exit_group		231
#define __NR_openat		257

/*
 * UML-private NR routed through the kvm-v2 LSTAR gadget.  Must
 * stay in sync with KVM_V2_NR_UML_APERFMPERF in
 * arch/um/backend/kvm-v2/syscall_trap.h.  Picked at 0xc0de to be
 * far above any present or reasonably-future Linux x86_64 NR.
 */
#define NR_UML_APERFMPERF	0xc0de

#define AT_FDCWD		(-100)
#define O_RDONLY		0x0
#define STDOUT_FD		1

#define PROBE_MOUNT		"/tmp/debug"
#define PROBE_PATH		"/tmp/debug/um/kvm_v2/aperf_mperf"

#define BUSY_LOOP_ITERS		2000000UL

struct um_aperfmperf {
	unsigned long aperf;
	unsigned long mperf;
};

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

/*
 * Call the UML-private gadget NR with RDI=out pointer.  Gadget
 * returns 0 in RAX on success (out filled), or -ENOSYS if the
 * gadget didn't intercept and the host returned the error.
 */
static long uml_aperfmperf(struct um_aperfmperf *out)
{
	return sys1(NR_UML_APERFMPERF, out);
}

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
	while (p < end && i < dst_sz - 1)
		dst[i++] = *p++;
	dst[i] = '\0';
}

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
	(void)sys3(__NR_mkdir, PROBE_MOUNT, 0755, 0);
	(void)sys5(__NR_mount, "debugfs", PROBE_MOUNT, "debugfs", 0UL, 0);
}

static void busy_spin(void)
{
	volatile unsigned long i;
	for (i = 0; i < BUSY_LOOP_ITERS; i++)
		;
}

int main(void)
{
	char out[2048];
	unsigned int n = 0;
	struct probe p;
	struct um_aperfmperf s1 = {0}, s2 = {0};
	long rc_p, rc_s1, rc_s2;
	unsigned long da, dm;
	int counters_nonzero, counters_advanced;
	int plumbing_ok, verdict_pass;
	const char *reason = (const char *)0;

	mount_debugfs();

	rc_p = read_probe(&p);
	if (rc_p < 0) {
		n = append_str(out, n,
			"APERF_MPERF_DEMO: FAIL probe_open_rc=");
		n = append_int(out, n, rc_p);
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

	plumbing_ok = (p.toggle_on && p.ioctl_attempted &&
		       p.ioctl_rc == 0);

	/*
	 * Call the gadget twice with a busy spin in between.  rc=0
	 * means the gadget intercepted (writes are valid).  rc<0
	 * means the gadget fell through (no consumer; CONFIG=n).
	 */
	rc_s1 = uml_aperfmperf(&s1);
	busy_spin();
	rc_s2 = uml_aperfmperf(&s2);

	if (rc_s1 < 0 || rc_s2 < 0) {
		n = append_str(out, n,
			"APERF_MPERF_DEMO: gadget_rc1=");
		n = append_int(out, n, rc_s1);
		n = append_str(out, n, " gadget_rc2=");
		n = append_int(out, n, rc_s2);
		n = append_str(out, n,
			" reason=gadget_not_present\n");
		n = append_str(out, n,
			"APERF_MPERF_DEMO: FAIL plumbing_ok=");
		n = append_int(out, n, plumbing_ok);
		n = append_str(out, n,
			" reason=kconfig_kvm_v2_gadget_off\n");
		sys3(__NR_write, STDOUT_FD, (long)out, n);
		sys3(__NR_exit_group, 1, 0, 0);
		return 1;
	}

	n = append_str(out, n, "APERF_MPERF_DEMO: sample1 aperf=");
	n = append_int(out, n, (long)s1.aperf);
	n = append_str(out, n, " mperf=");
	n = append_int(out, n, (long)s1.mperf);
	n = append_str(out, n, "\n");

	n = append_str(out, n, "APERF_MPERF_DEMO: sample2 aperf=");
	n = append_int(out, n, (long)s2.aperf);
	n = append_str(out, n, " mperf=");
	n = append_int(out, n, (long)s2.mperf);
	n = append_str(out, n, "\n");

	da = (s2.aperf >= s1.aperf) ? (s2.aperf - s1.aperf) : 0;
	dm = (s2.mperf >= s1.mperf) ? (s2.mperf - s1.mperf) : 0;

	n = append_str(out, n, "APERF_MPERF_DEMO: delta   aperf=");
	n = append_int(out, n, (long)da);
	n = append_str(out, n, " mperf=");
	n = append_int(out, n, (long)dm);
	n = append_str(out, n, "\n");

	n = append_str(out, n, "APERF_MPERF_DEMO: ratio_pct=");
	if (s1.mperf > 0)
		n = append_int(out, n,
			       (long)((s1.aperf * 100UL) / s1.mperf));
	else
		n = append_str(out, n, "n/a");
	n = append_str(out, n, " (sample1) ");
	if (s2.mperf > 0)
		n = append_int(out, n,
			       (long)((s2.aperf * 100UL) / s2.mperf));
	else
		n = append_str(out, n, "n/a");
	n = append_str(out, n, " (sample2) ");
	if (dm > 0)
		n = append_int(out, n,
			       (long)((da * 100UL) / dm));
	else
		n = append_str(out, n, "n/a");
	n = append_str(out, n, " (delta)\n");

	counters_nonzero = (s1.aperf > 0 && s1.mperf > 0 &&
			    s2.aperf > 0 && s2.mperf > 0);
	counters_advanced = (s2.aperf > s1.aperf && s2.mperf > s1.mperf);

	verdict_pass = plumbing_ok && counters_nonzero && counters_advanced;

	if (!verdict_pass) {
		if (!plumbing_ok) {
			if (!p.toggle_on)
				reason = "passthrough_off_by_cmdline";
			else if (!p.ioctl_attempted)
				reason = "cap_not_requested";
			else if (!p.host_feature)
				reason = "host_no_feature";
			else
				reason = "kvm_rejected_cap";
		} else if (!counters_nonzero) {
			reason = "kvm_emulated_zero";
		} else if (!counters_advanced) {
			reason = "counters_stuck";
		}
	}

	n = append_str(out, n, "APERF_MPERF_DEMO: ");
	n = append_str(out, n, verdict_pass ? "PASS" : "FAIL");
	n = append_str(out, n, " plumbing_ok=");
	n = append_int(out, n, plumbing_ok);
	n = append_str(out, n, " counters_nonzero=");
	n = append_int(out, n, counters_nonzero);
	n = append_str(out, n, " counters_advanced=");
	n = append_int(out, n, counters_advanced);
	if (reason) {
		n = append_str(out, n, " reason=");
		n = append_str(out, n, reason);
	}
	n = append_str(out, n, "\n");

	sys3(__NR_write, STDOUT_FD, (long)out, n);
	sys3(__NR_exit_group, verdict_pass ? 0 : 1, 0, 0);
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
