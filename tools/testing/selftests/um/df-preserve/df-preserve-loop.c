/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Direction-flag preservation selftest.
 *
 * Validates that the user-RFLAGS round-trip path preserves DF across
 * the two paths it covers:
 *
 *   1. SYSCALL round-trip. Set DF=1, call a class-A passthrough
 *      syscall (__NR_getsid, deliberately NOT a gadget-handled
 *      NR; getsid VMEXITs to handle_syscall on every backend),
 *      read RFLAGS back via pushfq, check DF still set.
 *      Exercises the kvm_build_sysret_r11 helper that rebuilds
 *      R11 (= user RFLAGS) on re-entry through kvm_enter_guest's
 *      bootstrap SYSRETQ.
 *
 *   2. Recoverable #PF round-trip. Set DF=1, touch a fresh anon
 *      page (forcing a guest #PF that the host shadow-PT recovery
 *      path resolves), read RFLAGS back, check DF still set.
 *      Exercises the IST+24 RFLAGS extraction in the host-side
 *      #PF handler.
 *
 * Reports one line:
 *
 *   DF_PRESERVE: syscall=PASS|FAIL pf=PASS|FAIL pf_skip=<reason>
 *
 * pf_skip="" means the #PF path executed; non-empty indicates we
 * couldn't synthesize a fault path on this host (e.g. mmap failed
 * because the binary is freestanding with no mmap helper for the
 * runtime). In that case syscall=PASS alone is the meaningful
 * validation; the #PF path is exercised indirectly by every other
 * recoverable fault during normal boot.
 *
 * Freestanding 64-bit ELF, same shape as getpid-loop / clock-loop.
 */

#include <stdint.h>

#define __NR_write		1
#define __NR_exit_group		231
#define __NR_getsid		124
#define __NR_mmap		9
#define STDOUT_FD		1

#define PROT_READ		0x1
#define PROT_WRITE		0x2
#define MAP_PRIVATE		0x02
#define MAP_ANONYMOUS		0x20
#define MAP_FAILED		((void *)-1)

#define DF_BIT			(1UL << 10)

static inline long sys6(long nr, long a, long b, long c, long d, long e, long f)
{
	long r;
	register long r10 __asm__("r10") = d;
	register long r8  __asm__("r8")  = e;
	register long r9  __asm__("r9")  = f;
	__asm__ volatile (
		"syscall" : "=a"(r)
		: "0"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
		: "rcx", "r11", "memory"
	);
	return r;
}

/*
 * Use __NR_getsid (NR 124, class A passthrough) instead of a
 * gadget-handled NR. Gadget syscalls preserve user RFLAGS
 * "for free" because R11 is left untouched by the LSTAR
 * trampoline.  SYSRETQ at the gadget tail loads RFLAGS from R11
 * which still holds the original. The kvm_build_sysret_r11
 * helper is exercised on the fallback path: a non-gadget syscall
 * VMEXITs to handle_syscall, then the next kvm_enter_guest bootstrap
 * SYSRETQ rebuilds R11 via the helper.  Routing through getsid forces
 * that fallback path.
 */
static inline long sys_getsid(long pid)
{ return sys6(__NR_getsid, pid, 0, 0, 0, 0, 0); }

static inline long sys_mmap(void *addr, unsigned long len, int prot,
			    int flags, int fd, long offset)
{
	return sys6(__NR_mmap, (long)addr, len, prot, flags, fd, offset);
}

static inline long sys_write(int fd, const void *buf, unsigned long len)
{ return sys6(__NR_write, fd, (long)buf, (long)len, 0, 0, 0); }

static inline void sys_exit(int code)
{ sys6(__NR_exit_group, code, 0, 0, 0, 0, 0); __builtin_unreachable(); }

/*
 * Set DF=1 via std, run the supplied closure (a syscall or a
 * memory access), then read back RFLAGS via pushfq. Returns the
 * post-action RFLAGS so the caller can check DF.
 *
 * Implemented as inline asm because we need to bracket the action
 * with std/cld and capture RFLAGS via pushfq/popq atomically with
 * minimum compiler interference.
 */
static inline unsigned long with_df_then_pushf_syscall(void)
{
	unsigned long flags;

	__asm__ volatile (
		"std\n\t"			/* DF = 1 */
		"mov $124, %%eax\n\t"		/* __NR_getsid = 124 */
		"xor %%edi, %%edi\n\t"		/* getsid(0) */
		"syscall\n\t"			/* class-A fallback path */
		"pushfq\n\t"
		"popq %0\n\t"
		"cld\n\t"			/* clear DF before returning */
		: "=r"(flags)
		:
		: "rax", "rcx", "rdi", "r11", "memory"
	);
	return flags;
}

static inline unsigned long with_df_then_pushf_store(volatile char *p)
{
	unsigned long flags;

	__asm__ volatile (
		"std\n\t"			/* DF = 1 */
		"movb $1, (%1)\n\t"		/* may #PF if page is lazy */
		"pushfq\n\t"
		"popq %0\n\t"
		"cld\n\t"
		: "=r"(flags)
		: "r"(p)
		: "memory"
	);
	return flags;
}

static unsigned append_str(char *buf, unsigned off, const char *s)
{ while (*s) buf[off++] = *s++; return off; }

int main(void)
{
	unsigned long rflags;
	int syscall_ok, pf_ok = 0;
	const char *pf_skip = "";
	void *page;
	char out[256];
	unsigned n;

	/*
	 * Path 1 - SYSCALL via the class-A fallback.
	 *
	 * Set DF=1, call __NR_getsid(0) (class A - not gadget-
	 * handled), read RFLAGS. F2 says the user-visible RFLAGS
	 * bits (including DF) survive the kvm_build_sysret_r11
	 * round-trip when handle_syscall returns and the next
	 * kvm_enter_guest rebuilds R11. Using a class-A NR forces
	 * that path; a gadget NR would short-circuit in the LSTAR
	 * trampoline without touching the helper.
	 */
	rflags = with_df_then_pushf_syscall();
	syscall_ok = !!(rflags & DF_BIT);

	/*
	 * Path 2 - recoverable #PF.
	 *
	 * Allocate a fresh anonymous mapping and write a byte to its
	 * first page. The first write forces UML's mm to instantiate
	 * the page (zero-fill on first touch) and the shadow-PT #PF
	 * handler refreshes the guest CR3 mapping. F2's IST+24
	 * extraction says the user RFLAGS at fault time survives the
	 * recovery.
	 *
	 * If the host's mmap helper rejects this request (e.g.
	 * out-of-memory or the freestanding binary's syscall ABI
	 * mismatches), fall through to pf_skip with a tag so the
	 * runner knows the path didn't execute. The syscall result
	 * still validates F2 via the much-more-common SYSCALL path.
	 */
	page = (void *)sys_mmap(0, 4096, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if ((long)page < 0 || page == MAP_FAILED) {
		pf_skip = "(mmap failed)";
	} else {
		rflags = with_df_then_pushf_store((volatile char *)page);
		pf_ok = !!(rflags & DF_BIT);
	}

	n = 0;
	n = append_str(out, n, "DF_PRESERVE: syscall=");
	n = append_str(out, n, syscall_ok ? "PASS" : "FAIL");
	n = append_str(out, n, " pf=");
	if (*pf_skip) {
		n = append_str(out, n, "SKIP");
		n = append_str(out, n, " pf_skip=");
		n = append_str(out, n, pf_skip);
	} else {
		n = append_str(out, n, pf_ok ? "PASS" : "FAIL");
		n = append_str(out, n, " pf_skip=");
	}
	out[n++] = '\n';

	sys_write(STDOUT_FD, out, n);
	sys_exit((syscall_ok && (pf_ok || *pf_skip)) ? 0 : 1);
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
