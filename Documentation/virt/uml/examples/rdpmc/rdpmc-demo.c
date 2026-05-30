/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rdpmc demo — minimal freestanding init that reads hardware
 * performance counters from guest userspace via `rdpmc`.
 *
 * With CONFIG_UM_BACKEND_KVM_V2_RDPMC=y the kvm-v2 backend sets
 * CR4.PCE=1 in the guest's per-vCPU control registers; that flips
 * rdpmc from privileged to unprivileged so it runs at CPL=3 without
 * #GP.  Combined with KVM's vPMU (on by default), guest userspace
 * gets a one-instruction window onto real hardware counters.
 *
 * What we read:
 *
 *   ECX index 0 (PMC0)              — programmable counter, requires
 *                                     PERFEVTSEL programming.  Reads
 *                                     0 unless the host kernel armed
 *                                     it; we use this as the
 *                                     "feature presence" probe.
 *   ECX index (1<<30) + 0           — fixed counter 0: instructions
 *                                     retired.  Always counting if
 *                                     IA32_FIXED_CTR_CTRL has it
 *                                     enabled.
 *   ECX index (1<<30) + 1           — fixed counter 1: unhalted core
 *                                     cycles.
 *   ECX index (1<<30) + 2           — fixed counter 2: reference
 *                                     cycles (unaffected by frequency
 *                                     scaling).
 *
 * Expected output on a kvm-v2 build with CR4.PCE on and fixed
 * counters armed by the host:
 *
 *   RDPMC_DEMO: pmc0_pre=0 pmc0_post=0          (programmable; not armed)
 *   RDPMC_DEMO: fixed0_pre=<N> fixed0_post=<M>  (instructions retired)
 *   RDPMC_DEMO: fixed1_pre=<N> fixed1_post=<M>  (cycles)
 *   RDPMC_DEMO: fixed2_pre=<N> fixed2_post=<M>  (ref cycles)
 *   RDPMC_DEMO: delta_instr=<N> delta_cyc=<M> delta_ref=<L>
 *   RDPMC_DEMO: PASS rdpmc_works=1 counters_advanced=1
 *
 * With CONFIG_UM_BACKEND_KVM_V2_RDPMC=n or seccomp backend, the
 * rdpmc executes at CPL=3 with PCE=0 and #GPs -- the demo gets
 * killed by SIGSEGV.  That's the deliberate cap-off signal.
 *
 * Designed to run as `init=` under UML with hostfs root and no
 * libc; same pattern as the aperf-mperf-demo next door.
 */

#include <stdint.h>

#define __NR_read		0
#define __NR_write		1
#define __NR_exit_group		231

#define STDOUT_FD		1
#define BUSY_LOOP_ITERS		20000000UL

#define FIXED_CTR_INDEX(n)	(0x40000000U | (n))

static long sys3(long nr, long a, long b, long c)
{
	long r;
	__asm__ volatile (
		"syscall"
		: "=a"(r)
		: "0"(nr), "D"(a), "S"(b), "d"(c)
		: "rcx", "r11", "memory"
	);
	return r;
}

static unsigned long rdpmc(unsigned int idx)
{
	unsigned int lo, hi;
	__asm__ volatile (
		"rdpmc"
		: "=a"(lo), "=d"(hi)
		: "c"(idx)
	);
	return ((unsigned long)hi << 32) | lo;
}

static unsigned int append_str(char *buf, unsigned int off, const char *s)
{
	while (*s)
		buf[off++] = *s++;
	return off;
}

static unsigned int append_u64(char *buf, unsigned int off, unsigned long v)
{
	char tmp[24];
	unsigned int n = 0;

	if (v == 0) {
		buf[off++] = '0';
		return off;
	}
	while (v) {
		tmp[n++] = '0' + (v % 10);
		v /= 10;
	}
	while (n--)
		buf[off++] = tmp[n];
	return off;
}

static void busy_spin(void)
{
	volatile unsigned long i;
	for (i = 0; i < BUSY_LOOP_ITERS; i++)
		;
}

struct sample {
	unsigned long pmc0;
	unsigned long fixed0;
	unsigned long fixed1;
	unsigned long fixed2;
};

static void take_sample(struct sample *s)
{
	s->pmc0   = rdpmc(0);
	s->fixed0 = rdpmc(FIXED_CTR_INDEX(0));
	s->fixed1 = rdpmc(FIXED_CTR_INDEX(1));
	s->fixed2 = rdpmc(FIXED_CTR_INDEX(2));
}

int main(void)
{
	char out[1024];
	unsigned int n = 0;
	struct sample s1, s2;
	unsigned long delta_instr, delta_cyc, delta_ref;
	int rdpmc_works, counters_advanced, verdict_pass;
	const char *reason = (const char *)0;

	/*
	 * If CR4.PCE is unset, rdpmc at CPL=3 #GPs and SIGSEGV kills
	 * the process before we print anything.  The host-side
	 * launcher sees a missing RDPMC_DEMO line and reports the
	 * absence as FAIL.  That's the deliberate cap-off signal.
	 */
	take_sample(&s1);
	busy_spin();
	take_sample(&s2);

	n = append_str(out, n, "RDPMC_DEMO: pmc0_pre=");
	n = append_u64(out, n, s1.pmc0);
	n = append_str(out, n, " pmc0_post=");
	n = append_u64(out, n, s2.pmc0);
	n = append_str(out, n, "\n");

	n = append_str(out, n, "RDPMC_DEMO: fixed0_pre=");
	n = append_u64(out, n, s1.fixed0);
	n = append_str(out, n, " fixed0_post=");
	n = append_u64(out, n, s2.fixed0);
	n = append_str(out, n, "\n");

	n = append_str(out, n, "RDPMC_DEMO: fixed1_pre=");
	n = append_u64(out, n, s1.fixed1);
	n = append_str(out, n, " fixed1_post=");
	n = append_u64(out, n, s2.fixed1);
	n = append_str(out, n, "\n");

	n = append_str(out, n, "RDPMC_DEMO: fixed2_pre=");
	n = append_u64(out, n, s1.fixed2);
	n = append_str(out, n, " fixed2_post=");
	n = append_u64(out, n, s2.fixed2);
	n = append_str(out, n, "\n");

	delta_instr = (s2.fixed0 >= s1.fixed0) ? (s2.fixed0 - s1.fixed0) : 0;
	delta_cyc   = (s2.fixed1 >= s1.fixed1) ? (s2.fixed1 - s1.fixed1) : 0;
	delta_ref   = (s2.fixed2 >= s1.fixed2) ? (s2.fixed2 - s1.fixed2) : 0;

	n = append_str(out, n,
		       "RDPMC_DEMO: delta_instr=");
	n = append_u64(out, n, delta_instr);
	n = append_str(out, n, " delta_cyc=");
	n = append_u64(out, n, delta_cyc);
	n = append_str(out, n, " delta_ref=");
	n = append_u64(out, n, delta_ref);
	n = append_str(out, n, "\n");

	/*
	 * `rdpmc_works` is "we made it past the first rdpmc without
	 * #GP" — which means we reached this code AT ALL with the
	 * later writes printing values.  Counter advancement is the
	 * "host armed fixed counters" check (kernel-side
	 * IA32_FIXED_CTR_CTRL programming).
	 */
	rdpmc_works = 1;
	counters_advanced = (delta_instr > 0) || (delta_cyc > 0) ||
			    (delta_ref > 0);
	verdict_pass = rdpmc_works && counters_advanced;

	if (!verdict_pass) {
		if (!counters_advanced)
			reason = "fixed_counters_not_armed";
		else
			reason = "unknown";
	}

	n = append_str(out, n, "RDPMC_DEMO: ");
	n = append_str(out, n, verdict_pass ? "PASS" : "FAIL");
	n = append_str(out, n, " rdpmc_works=");
	n = append_str(out, n, rdpmc_works ? "1" : "0");
	n = append_str(out, n, " counters_advanced=");
	n = append_str(out, n, counters_advanced ? "1" : "0");
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
