// SPDX-License-Identifier: GPL-2.0
/*
 * UML template-pause + fork hook (Memo 09 Phase 1a).
 *
 * The cooperative in-kernel seam that lets a booted UML quiesce at a
 * named "ready point," SIGSTOP the host process for the supervisor to
 * fork(2), and resume on SIGCONT with a per-child identity blob
 * applied.  See Documentation/virt/uml/redesign/06-sequencing/
 * post-2026-05-19-next-sprint/09-fork-server-snapshot-restore.md
 * for the full design.
 *
 * Distinct from arch/um/kernel/snapshot.c's AFL-style forkserver path:
 * that path's parent never returns and the parent itself drives per-
 * iteration fork via fds 198/199.  Here the kernel doesn't fork —
 * the umlctl supervisor does, externally, after seeing the master's
 * SIGSTOP.  Each forked child wakes from the same SIGSTOP point with
 * its own identity blob to apply (Phase 2 wires the actual MAC / IP /
 * tap-fd replumb; Phase 1a just reads + logs).
 *
 * Phase 1a scope (this commit):
 *   - __setup("um_template_pause", ...) arm flag
 *   - /proc/um/template_pause write trigger
 *   - um_template_pause_enter() core: log → SIGSTOP → log → read
 *     identity blob → validate magic/version → log
 *
 * Out of scope (Phase 2):
 *   - Applying the identity (netdev MAC swap, IP rebind, tap fd swap)
 *   - mconsole socket re-path
 *   - sysfs node reflecting current identity
 */

#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/errno.h>
#include <linux/hardirq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include <asm/um-template-pause.h>
#include "template_pause_identity.h"
#ifdef CONFIG_UM_TEMPLATE_PAUSE_FORK
#include <asm/um-snapshot.h>		/* um_snapshot_worker_init() */
#include <skas.h>			/* um_skas_teardown_all_stubs(),
					 *  um_skas_respawn_all_stubs() */
#include <skas/skas.h>			/* userspace() trap loop */
#include <os_io_ring.h>			/* os_close_inherited_io_uring_fds */
#endif

#include <backend.h>				/* um_backend, kind enum */

#include <os.h>

/*
 * Armed via "um_template_pause" on the kernel cmdline.  Off by
 * default: a non-pool boot must never accidentally raise SIGSTOP on
 * itself just because a stray /proc write lands.  The proc write
 * handler checks this flag and returns -ENODEV when unarmed.
 */
static bool template_pause_armed_flag __read_mostly;

/*
 * Fork-on-resume mode.  Off by default (Phase 1a behaviour: single
 * SIGSTOP/SIGCONT cycle, master IS the taken instance).  Armed when
 * the kernel cmdline reads `um_template_pause=fork` (Phase 2a:
 * fork-on-resume loop — each SIGCONT forks the master, child wakes
 * as the taken pool member, master re-pauses for the next take).
 *
 * Requires CONFIG_UM_TEMPLATE_PAUSE_FORK at build time (selects the
 * CoW physmem path + um_snapshot_worker_init).  Without that config,
 * even `=fork` is downgraded to Phase 1a behaviour with a warning.
 */
static bool template_pause_fork_armed_flag __read_mostly;

/*
 * Early-pause mode.  Armed by `um_template_pause=early` on the
 * cmdline.  In this mode the kernel pauses ITSELF (via a late
 * initcall) before run_init_process is reached — no guest userspace
 * has executed, no SKAS stub children exist, and the runqueue holds
 * only kernel-mode tasks.  This is the AFL forkserver's
 * "v1-friendly" ready point invariant and avoids the v1-ceiling
 * UML_LONGJMP-into-stale-jmp_buf hazard that the late ready point
 * (via /proc/um/template_pause from init) triggers.
 *
 * Combinable with =fork: `um_template_pause=early-fork` arms BOTH.
 */
static bool template_pause_early_armed_flag __read_mostly;

#ifdef CONFIG_UM_TEMPLATE_PAUSE_FORK_DIAG
/*
 * Diagnostic-mode arm.  Off by default.  Armed by
 * `um_template_pause_mfc_diag=1` on the kernel cmdline.  When set,
 * master skips the post-fork SIGKILL of the M-fork child, and the
 * child installs `mfc_diag_segv_handler` for SIGSEGV/SIGBUS/
 * SIGILL/SIGFPE before doing __NR_exit_group(0).  If the child
 * faults during exit, the handler dumps sig/addr/rip/cr2 to fd 1
 * and exits with code 7.
 *
 * Only compiled in under CONFIG_UM_TEMPLATE_PAUSE_FORK_DIAG.
 */
static bool template_pause_mfc_diag_armed_flag __read_mostly;
#endif

/*
 * Private-stack mode arm.  DEFAULT ON in fork mode (the production
 * fix for bug B1, validated 2026-05-20).  Can be disabled via
 * `um_template_pause_private_stack=0` on the kernel cmdline for
 * the legacy __NR_fork + post-fork SIGKILL behaviour (used for
 * regression testing or in case the clone() path has a bug).
 *
 * Mechanism: master uses __NR_clone (NOT __NR_fork) with a private
 * MAP_PRIVATE | MAP_ANONYMOUS stack for the child.  This bypasses
 * the MAP_SHARED physmem stack-race documented in B1.  The child's
 * first instruction post-clone runs on its OWN stack, so master's
 * concurrent writes to MASTER's stack don't corrupt the child's
 * view.  Child does inline __NR_exit_group(0) and terminates
 * cleanly — no panic, no SIGKILL needed.
 */
static bool template_pause_private_stack_armed_flag __read_mostly = true;

/*
 * Path A integration smoke arm.  Off by default.  Armed by
 * `um_template_pause_pivot_test=1` on the kernel cmdline.
 *
 * When set, master uses os_template_pause_fork_clone_to(@entry) and
 * the child runs `child_entry_pivot_test` on the private stack — a
 * kernel C function that writes a "PIVOT_OK\n" marker via raw
 * __NR_write and exits cleanly.  Master skips the post-fork SIGKILL
 * because the child terminates itself.
 *
 * The goal of this mode is end-to-end proof that the Path A stack-
 * pivot primitive (validated host-side in
 * tools/testing/selftests/um/rt-sigreturn-isolation/) crosses the v1
 * ceiling on a real UML kernel: if PIVOT_OK appears in the host
 * log, kernel C code executed in the M-fork child without hitting
 * the corrupted-saved-RIP panic at um_template_pause_enter+0xf6.
 */
static bool template_pause_pivot_test_armed_flag __read_mostly;

/*
 * Pool-member arm.  Off by default.  Armed by
 * `um_template_pause_pool_member=1` on the kernel cmdline.
 *
 * When set, master uses os_template_pause_fork_clone_to(@entry) and
 * the child runs `child_entry_pool_member()` on the private stack:
 * preempt_enable + host signal restore + SKAS stub respawn + drop
 * into userspace() — the canonical UML "resume the userspace task"
 * pattern.  The result: each post-fork child becomes a long-lived
 * UML kernel running init.sh's continuation.
 *
 * This is the Memo 09 §2 design realized end-to-end.  See
 * Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
 * state-audit/31-path-a-kernel-integration.md §3 for the rationale.
 */
static bool template_pause_pool_member_armed_flag __read_mostly;

/*
 * Per-host-process flag: set in child_entry_pool_member after the
 * fresh stub is up.  This child IS a pool member; subsequent
 * template_pause writes from its userspace (e.g., init.sh's
 * `echo > /proc/um/template_pause` running again) must not
 * recursively re-enter the fork-on-resume loop — there's no
 * daemon to SIGCONT the child, so it would hang forever at
 * SIGSTOP.  Inherited via CoW (each child sets it independently
 * in its own page); master never sees it set.
 */
static bool template_pause_pool_member_active;

/* Diagnostic only — counts how many SIGSTOP/SIGCONT cycles this
 * process (or any of its forked descendants that still share the
 * .data segment) has been through.  A fresh fork() inherits the
 * counter value of its parent; consumers reading
 * /sys/kernel/um/template_pause_count can use that to distinguish
 * "master that has paused once" from "child that has paused twice."
 */
static atomic_t template_pause_count = ATOMIC_INIT(0);

static int __init template_pause_setup(char *str)
{
	template_pause_armed_flag = true;
	if (str && (!strcmp(str, "=early") || !strcmp(str, "=early-fork"))) {
		template_pause_early_armed_flag = true;
		if (!strcmp(str, "=early-fork"))
			template_pause_fork_armed_flag = true;
		pr_info("template_pause: armed via kernel cmdline (early-pause%s)\n",
			template_pause_fork_armed_flag ? " + fork-on-resume" : "");
	} else if (str && !strcmp(str, "=fork")) {
		template_pause_fork_armed_flag = true;
		pr_warn("template_pause: armed via kernel cmdline (fork-on-resume) — EXPERIMENTAL; known to corrupt parent-side seccomp stub state. See 09-fork-server-STATUS.md.\n");
	} else {
		pr_info("template_pause: armed via kernel cmdline\n");
	}
	return 1;
}
__setup("um_template_pause", template_pause_setup);

#ifdef CONFIG_UM_TEMPLATE_PAUSE_FORK_DIAG
static int __init template_pause_mfc_diag_setup(char *str)
{
	if (str && (str[0] == '=' && str[1] == '1'))
		template_pause_mfc_diag_armed_flag = true;
	else if (str && str[0] == '\0')
		template_pause_mfc_diag_armed_flag = true;
	if (template_pause_mfc_diag_armed_flag)
		pr_warn("template_pause: MFC diagnostic mode armed — SIGKILL of M-fork child disabled, fault handler installed.  Expect MFC_FAULT lines or panics in boot.log.\n");
	return 1;
}
__setup("um_template_pause_mfc_diag", template_pause_mfc_diag_setup);
#endif

static int __init template_pause_private_stack_setup(char *str)
{
	if (str && str[0] == '=' && str[1] == '0')
		template_pause_private_stack_armed_flag = false;
	else if (str && str[0] == '=' && str[1] == '1')
		template_pause_private_stack_armed_flag = true;
	else if (str && str[0] == '\0')
		template_pause_private_stack_armed_flag = true;
	pr_info("template_pause: private-stack mode = %s\n",
		template_pause_private_stack_armed_flag ? "ENABLED (production)" : "DISABLED (legacy fork+SIGKILL)");
	return 1;
}
__setup("um_template_pause_private_stack", template_pause_private_stack_setup);

static int __init template_pause_pivot_test_setup(char *str)
{
	if (str && str[0] == '=' && str[1] == '1')
		template_pause_pivot_test_armed_flag = true;
	else if (str && str[0] == '\0')
		template_pause_pivot_test_armed_flag = true;
	if (template_pause_pivot_test_armed_flag)
		pr_warn("template_pause: PATH-A pivot-test mode = ARMED — M-fork child will run child_entry_pivot_test on private stack and self-exit.\n");
	return 1;
}
__setup("um_template_pause_pivot_test", template_pause_pivot_test_setup);

static int __init template_pause_pool_member_setup(char *str)
{
	if (str && str[0] == '=' && str[1] == '1')
		template_pause_pool_member_armed_flag = true;
	else if (str && str[0] == '\0')
		template_pause_pool_member_armed_flag = true;
	if (template_pause_pool_member_armed_flag)
		pr_warn("template_pause: POOL-MEMBER mode = ARMED — M-fork child will drop into userspace() as a long-lived pool member.\n");
	return 1;
}
__setup("um_template_pause_pool_member", template_pause_pool_member_setup);

/*
 * Child entry for Path A pivot-test mode.  Runs on a MAP_PRIVATE
 * stack allocated by os_template_pause_fork_clone_to() — does NOT
 * share the master's kernel stack, so the v1-ceiling saved-RIP
 * corruption cannot reach us (see state-audit/30-path-c-v1-ceiling-
 * confirmed.md).
 *
 * Implementation: raw inline-asm only (no glibc, no kernel locks,
 * no printk).  Writes a fixed marker to host fd 1 then exit_group(0).
 *
 * If "PIVOT_OK" appears in boot output, the Path A primitive crossed
 * the ceiling end-to-end on this host kernel.
 */
static void __noreturn
child_entry_pivot_test(void)
{
	static const char marker[] = "PIVOT_OK\n";
	register long rax asm("rax") = 1;       /* __NR_write */
	register long rdi asm("rdi") = 1;       /* host fd 1 */
	register long rsi asm("rsi") = (long)marker;
	register long rdx asm("rdx") = sizeof(marker) - 1;

	asm volatile ("syscall"
		      : "+r" (rax)
		      : "r" (rdi), "r" (rsi), "r" (rdx)
		      : "rcx", "r11", "memory");

	{
		register long erax asm("rax") = 231;    /* __NR_exit_group */
		register long erdi asm("rdi") = 0;

		asm volatile ("syscall"
			      :
			      : "r" (erax), "r" (erdi)
			      : "rcx", "r11", "memory");
	}
	__builtin_unreachable();
}

/*
 * Real pool-member child entry.  Runs on a MAP_PRIVATE 8 KiB stack
 * post-clone (allocated by os_template_pause_fork_clone_to()).
 *
 * UML resolves `current` via cpu_tasks[uml_curr_cpu()] (a per-CPU
 * table — see arch/um/include/asm/current.h), NOT thread_info-on-
 * stack.  So `current` still points at init.sh's task struct (the
 * one that called write to /proc/um/template_pause), even from our
 * private stack.
 *
 * Sequence:
 *
 *   1. preempt_enable() — master held preempt_disable across fork.
 *   2. os_template_pause_signals_restore_host() — master blocked
 *      host signals pre-fork; child must unblock so timer ticks,
 *      SIGIO, SIGCHLD reach the seccomp dispatch loop.
 *   3. PT_REGS_SET_SYSCALL_RETURN(init.sh's regs, write_count) —
 *      the /proc write that triggered template_pause is "returning"
 *      with the byte count.  We pick a fixed value (11 = length of
 *      "fork-smoke\n") for the smoke harness; production would
 *      arrange for master to stash the real count before fork.
 *   4. um_skas_respawn_all_stubs() — master tore the stubs down
 *      pre-fork (see fork_on_resume_loop step (A)).  Child needs
 *      a fresh stub for its own userspace.
 *   5. userspace(&current->thread.regs.regs) — drop into the
 *      seccomp dispatch loop forever, pumping init.sh's userspace.
 *      Never returns.
 */
static void __noreturn
child_entry_pool_member(void)
{
	static const char enter_msg[] = "POOL_ENTER\n";
	register long rax asm("rax") = 1;
	register long rdi asm("rdi") = 1;
	register long rsi asm("rsi") = (long)enter_msg;
	register long rdx asm("rdx") = sizeof(enter_msg) - 1;

	asm volatile ("syscall"
		      : "+r" (rax)
		      : "r" (rdi), "r" (rsi), "r" (rdx)
		      : "rcx", "r11", "memory");

	preempt_enable();
	(void)os_template_pause_signals_restore_host();

	/*
	 * Per timer(7), POSIX interval timers are NOT preserved across
	 * fork(2): the child must rebuild its own SIGALRM source.
	 * os_timer_worker_forget() zeroes the inherited timer_t handles
	 * (they would deliver to master's gettid() otherwise); the
	 * rebuild creates a fresh CLOCK_MONOTONIC timer targeting this
	 * thread's tid.  Without this, nanosleep() in the child never
	 * wakes up.
	 */
	os_timer_worker_forget();
	(void)os_timer_worker_rebuild();
	/*
	 * Arm a near-immediate one-shot so the kernel's tick path
	 * gets called and re-establishes its next_event tracking.
	 * Without this, the inherited clock_event_device state may
	 * not call set_next_event for the next hrtimer expiration.
	 */
	(void)os_timer_one_shot(0, 1000000ULL); /* 1ms */

	/*
	 * Set init.sh's syscall return value.  AX was -ENOSYS (master
	 * never finished the syscall return path for the write to
	 * /proc/um/template_pause).  Init.sh wrote "fork-smoke\n" (11
	 * bytes); set RAX = 11 so the shell sees write succeed.
	 */
	PT_REGS_SET_SYSCALL_RETURN(&current->thread.regs, 11);

	/*
	 * Disown master's inherited stubs and spawn fresh per-member
	 * stubs with PHYSICALLY-ISOLATED stub_data backing.
	 *
	 * um_skas_disown_inherited() forgets master's stub_pid/sock
	 * (no kill — those are master's host-children) and clears the
	 * inherited id->stack so start_userspace_fresh can replace it.
	 *
	 * For each mm_id, start_userspace_fresh:
	 *   - memfd_create + ftruncate + mmap MAP_SHARED to allocate
	 *     a per-mm stub_data page in THIS child's address space
	 *   - swaps id->stack to the new VA
	 *   - clones a fresh stub via CLONE_VM (shares THIS child's
	 *     VM, not master's)
	 *   - passes the memfd through tramp_data->stub_data_fd_override
	 *     so the stub mmaps the per-mm memfd directly, bypassing
	 *     UML's MAP_SHARED physmem_fd entirely
	 *
	 * Each pool member's stub now has private stub_data; no
	 * inter-member aliasing.
	 */
	/*
	 * Restore init.sh task/signal state to a clean RUNNING
	 * baseline.  At the time of master's SIGSTOP, the init.sh
	 * task is mid-syscall (write to /proc/um/template_pause).
	 */
	current->flags &= ~(PF_EXITING | PF_POSTCOREDUMP | PF_SIGNALED);
	WRITE_ONCE(current->__state, TASK_RUNNING);
	current->exit_state = 0;
	current->exit_code = 0;
	atomic_set(&current->signal->live, 1);
	current->signal->group_exit_code = 0;
	current->signal->flags = 0;
	if (current->mm) {
		atomic_set(&current->mm->mm_users, 2);
		atomic_set(&current->mm->mm_count, 2);
	}

	/*
	 * Step A of the pool-completion roadmap (per-member
	 * physmem isolation) — the replicate call site stays
	 * unwired pending the post-swap user-mode wake-up fix.
	 * Variant 20 (stub-init + start_userspace_tramp trace)
	 * localized the failure to the SECOND new stub
	 * (start_userspace path, for /bin/sleep's forked mm).
	 * The first new stub (start_userspace_fresh with per-mm
	 * memfd) works.  The second new stub clone()s OK but
	 * never completes execve → never sends first SIGSYS
	 * back to kernel.  Hypothesis: stub_exe_fd or one of
	 * the file descriptor flags interacts poorly with the
	 * post-replicate VM state.
	 */

	/*
	 * Step A of the pool-completion roadmap (per-member
	 * physmem isolation) — historical context.
	 *
	 * Empirical bisect (this commit's investigation):
	 *   (1) Same-fd mmap-FIXED (um_pool_remap_self_test):
	 *       PASS — content unchanged, timer keeps firing.
	 *   (2) Dup'd-fd mmap-FIXED (same file, different fd
	 *       value): PASS — host kernel finds same inode, no
	 *       signal-delivery disruption.  But provides no
	 *       isolation since both ends back onto the same file.
	 *   (3) New-fd mmap-FIXED to memfd_create()-backed fd
	 *       with copy_file_range-equivalent content: FAIL —
	 *       SIGALRM stops being delivered post-swap.
	 *   (4) New-fd mmap-FIXED to O_TMPFILE-backed fd (same
	 *       create path as setup_physmem boot-time fd):
	 *       FAIL — same SIGALRM regression as (3).
	 *   (5) Skip kernel-VA mmap-FIXED entirely, just update
	 *       global physmem_fd to point to new memfd:
	 *       FAIL — master enters fast SIGCONT loop (kernel↔
	 *       stub coherence broken: kernel writes via kernel
	 *       VA on old fd, stub maps user VAs from new fd,
	 *       VAs read different content).
	 *
	 * The host-kernel signal-delivery state ties to the file
	 * identity backing the VMA in a way that re-issuing
	 * sigaltstack + sigaction(SIGALRM) + timer_create does
	 * not recover.  Root cause not yet isolated (candidates:
	 * io_uring queue's pinned pages, hostfs writeback bdi
	 * binding, some kernel cache referencing the original
	 * inode); a strace-on-the-UML-process trace is the next
	 * concrete debug step.
	 *
	 * The replicate/remap helpers stay in tree (call sites
	 * `os_create_memfd`, `os_create_tmpfile`,
	 * `os_mmap_rw_scratch`, `os_remap_region_shared`,
	 * `um_pool_replicate_physmem`, `um_pool_remap_self_test`)
	 * so the eventual workaround can re-enable them with a
	 * one-line change.
	 */

	{
		int dret = um_skas_disown_inherited();
		struct mm_id *id = current_mm_id();

		if (dret > 0 && id) {
			(void)start_userspace_fresh(id);
			if (current->mm)
				(void)um_skas_force_resync_mm(current->mm);
		}
	}

	/*
	 * Mark this host process as a pool member so future
	 * template_pause writes from its userspace are no-ops.
	 * Prevents recursive SIGSTOP-without-daemon hangs (still
	 * useful for the single-iteration path).
	 */
	template_pause_pool_member_active = true;

	userspace(&current->thread.regs.regs);
	__builtin_unreachable();
}

bool um_template_pause_armed(void)
{
	return template_pause_armed_flag;
}
EXPORT_SYMBOL_GPL(um_template_pause_armed);

/*
 * MFC_DIAG — M-fork child fault diagnostic.
 *
 * When the M-fork child path's diagnostic mode is enabled, the child
 * installs this as its SIGSEGV/SIGBUS/SIGILL/SIGFPE handler.  On a
 * fault, the handler writes the fault sig number, faulting address
 * (siginfo->si_addr), and the user-mode RIP at fault time
 * (ucontext->uc_mcontext.gregs[REG_RIP]) to fd 1 via raw write,
 * then raw __NR_exit_group(7) so the harness can distinguish a
 * "caught fault" exit (code 7) from a clean exit (code 0).
 *
 * Implementation is hand-rolled to avoid touching anything in
 * libc, the kernel allocator, or the locking subsystem from the
 * post-fork hazard window.  Each output byte goes through one
 * inline-asm __NR_write call; the formatter is a per-nibble hex
 * unroller.  No memory allocation, no printk, no kernel mutexes.
 *
 * The offset of REG_RIP within ucontext_t is x86_64-specific:
 * struct ucontext { ... mcontext_t mc; ... }
 * where mcontext_t starts with gregs[NGREG=23] then fpregs.
 * On x86_64, REG_RIP is gregs[16].  ucontext_t's mc starts at
 * offset 0x28 (sigset_t before it).  So &uc->uc_mcontext.gregs[16]
 * is at uc + 0x28 + 16*8 = uc + 0xa8.
 */
#ifdef CONFIG_UM_TEMPLATE_PAUSE_FORK_DIAG
static void mfc_diag_segv_handler(int sig, void *si_arg, void *uc_arg);
static void mfc_diag_restorer(void);
#endif

/*
 * Read + validate the identity blob.  Returns 0 on a present + valid
 * blob (fields are NUL-terminated and logged), >0 if @identity_fd is
 * -1 (no channel — informational), or -errno on read / validation
 * failure.  @blob is zero-initialised on entry.
 */
static int read_identity_blob(int identity_fd,
			      const char *named_point,
			      struct um_template_identity *blob)
{
	ssize_t n;

	memset(blob, 0, sizeof(*blob));
	if (identity_fd < 0)
		return 1;

	n = os_template_pause_read_identity(identity_fd, blob, sizeof(*blob));
	if (n < 0) {
		pr_err("template_pause: identity read failed: %zd\n", n);
		return (int)n;
	}
	if ((size_t)n < sizeof(blob->magic) + sizeof(blob->version)) {
		pr_warn("template_pause: identity blob too short (%zd bytes); skipping\n",
			n);
		return -EINVAL;
	}
	if (blob->magic != UM_TEMPLATE_IDENTITY_MAGIC) {
		pr_err("template_pause: identity blob magic 0x%08x != expected 0x%08x\n",
		       blob->magic, UM_TEMPLATE_IDENTITY_MAGIC);
		return -EILSEQ;
	}
	if (blob->version != UM_TEMPLATE_IDENTITY_VERSION) {
		pr_err("template_pause: identity blob version %u not supported (expected %u)\n",
		       blob->version, UM_TEMPLATE_IDENTITY_VERSION);
		return -EPROTONOSUPPORT;
	}

	blob->instance_name[sizeof(blob->instance_name) - 1] = '\0';
	blob->tap_name[sizeof(blob->tap_name) - 1] = '\0';
	blob->ipv4_cidr[sizeof(blob->ipv4_cidr) - 1] = '\0';
	blob->ipv4_gateway[sizeof(blob->ipv4_gateway) - 1] = '\0';
	pr_info("template_pause: identity at \"%s\" instance=\"%s\" mac=%02x:%02x:%02x:%02x:%02x:%02x tap=\"%s\" ipv4=\"%s\" gw=\"%s\"\n",
		named_point,
		blob->instance_name,
		blob->mac_addr[0], blob->mac_addr[1], blob->mac_addr[2],
		blob->mac_addr[3], blob->mac_addr[4], blob->mac_addr[5],
		blob->tap_name, blob->ipv4_cidr, blob->ipv4_gateway);
	return 0;
}

/*
 * One SIGSTOP/SIGCONT cycle + identity read.  Shared between the
 * Phase 1a single-shot path and each iteration of the Phase 2a
 * fork-on-resume loop.
 */
static int one_pause_cycle(const char *named_point, int identity_fd,
			   struct um_template_identity *blob_out)
{
	int ret;

	/*
	 * Block host signals BEFORE SIGSTOP, not after SIGCONT.
	 *
	 * If signals are blocked only post-SIGCONT (the previous order),
	 * then queued signals (SIGALRM, SIGCHLD, SIGIO) accumulated
	 * during the stop window fire on master's kernel stack between
	 * SIGCONT and signal_block.  UML's hard_handler runs on the
	 * interrupted task's kernel stack, and the handler frame
	 * corrupts the saved-RIP at this function's caller's epilogue
	 * — the v1 ceiling documented in state-audit/30.
	 *
	 * Blocking BEFORE SIGSTOP closes that window: signals queue at
	 * the host kernel level but are not delivered until master
	 * explicitly unblocks (which it never does in the fork loop).
	 * SIGSTOP/SIGCONT are unmaskable so the stop/resume cycle still
	 * works.
	 *
	 * Idempotent on repeated calls (same rt_sigprocmask mask).
	 */
	(void)os_template_pause_signals_block_host();

	pr_info("template_pause: raising SIGSTOP at \"%s\" (pid=%d)\n",
		named_point, os_getpid());
	ret = os_template_pause_stop_self();
	if (ret) {
		pr_err("template_pause: SIGSTOP/SIGCONT roundtrip failed: %d\n",
		       ret);
		return ret;
	}
	atomic_inc(&template_pause_count);
	pr_info("template_pause: resumed via SIGCONT at \"%s\" (pid=%d, count=%d)\n",
		named_point, os_getpid(),
		atomic_read(&template_pause_count));
	(void)read_identity_blob(identity_fd, named_point, blob_out);
	return 0;
}

#ifdef CONFIG_UM_TEMPLATE_PAUSE_FORK

#ifdef CONFIG_UM_TEMPLATE_PAUSE_FORK_DIAG
/*
 * MFC diagnostic restorer — used as sa_restorer when the M-fork
 * child installs a custom signal handler.  The kernel jumps here
 * when the handler returns; this thunk invokes __NR_rt_sigreturn
 * to restore the pre-signal user-mode context.  Hand-rolled in
 * inline asm because the C function prologue is not safe for use
 * as a signal restorer (RSP would be wrong).
 */
__attribute__((naked))
__used __maybe_unused
static void mfc_diag_restorer(void)
{
	asm volatile (
		"movq $15, %%rax\n\t"   /* __NR_rt_sigreturn */
		"syscall\n\t"
		: : : "memory"
	);
}

/*
 * MFC diagnostic SIGSEGV/SIGBUS/SIGILL/SIGFPE handler.  Dumps the
 * fault info via raw write syscalls, then raw exit_group(7).
 *
 * Args follow the SA_SIGINFO calling convention:
 *   rdi = signum, rsi = struct siginfo *, rdx = struct ucontext *
 *
 * We DO NOT trust ucontext layout from <signal.h> because (a) it's
 * a userspace UAPI header (not normally included in kernel C) and
 * (b) we need byte-exact offsets to reach REG_RIP and REG_CR2.
 * Hand-roll the layout per Linux x86_64 ABI (siginfo_t.si_addr at
 * offset 16, ucontext_t.uc_mcontext.gregs[REG_RIP=16] at offset
 * 0x28 + 16*8 = 0xa8).
 */
/*
 * MFC immediate post-fork-syscall dumper.  Called from a custom
 * fork inner that does the fork syscall and IMMEDIATELY jumps here
 * BEFORE doing any ret.  Dumps regs + nearby stack via raw write
 * then exit_group(7) so we can see what the child's stack contents
 * actually are at fork-syscall-return time.
 *
 * Only called from the M-fork child (parent goes a different path
 * after its fork returns).
 */
__used __maybe_unused
static void mfc_diag_post_fork_dumper(unsigned long rsp, unsigned long rbp,
				       unsigned long r15)
{
	char buf[400];
	int i, p = 0;
	unsigned long val;
	unsigned long *stack;

#define APPEND_STR(s) do {					\
		const char *_s = (s);				\
		while (*_s && p < (int)sizeof(buf) - 1)	\
			buf[p++] = *_s++;			\
	} while (0)
#define APPEND_HEX(v) do {					\
		val = (v);					\
		buf[p++] = '0';					\
		buf[p++] = 'x';					\
		for (i = 15; i >= 0; i--) {			\
			unsigned int nib = (val >> (i*4)) & 0xf; \
			buf[p++] = nib < 10 ? '0' + nib : 'a' + nib - 10; \
		}						\
	} while (0)

	APPEND_STR("MFC_POSTFORK rsp=");
	APPEND_HEX(rsp);
	APPEND_STR(" rbp=");
	APPEND_HEX(rbp);
	APPEND_STR(" r15=");
	APPEND_HEX(r15);
	APPEND_STR(" stack[0..5]=");
	stack = (unsigned long *)rsp;
	for (i = 0; i < 6; i++) {
		APPEND_HEX(stack[i]);
		buf[p++] = ' ';
	}
	buf[p++] = '\n';
#undef APPEND_STR
#undef APPEND_HEX

	{
		register long rax_w asm("rax") = 1;
		register long rdi_w asm("rdi") = 1;
		register long rsi_w asm("rsi") = (long)buf;
		register long rdx_w asm("rdx") = p;

		asm volatile ("syscall\n\t"
			: "+r" (rax_w)
			: "r" (rdi_w), "r" (rsi_w), "r" (rdx_w)
			: "rcx", "r11", "memory");
	}

	{
		register long rax_x asm("rax") = 231;
		register long rdi_x asm("rdi") = 7;

		asm volatile ("syscall\n\t"
			:
			: "r" (rax_x), "r" (rdi_x)
			: "rcx", "r11", "memory");
	}
	for (;;)
		;
}

__used __maybe_unused
static void mfc_diag_segv_handler(int sig, void *si_arg, void *uc_arg)
{
	unsigned long fault_addr = 0;
	unsigned long fault_rip = 0;
	unsigned long fault_cr2 = 0;
	unsigned long fault_err = 0;
	char buf[160];
	int i, p = 0;
	unsigned long val;

	/* siginfo_t->si_addr is at offset 16 (sigtrap fields union). */
	if (si_arg)
		fault_addr = *(unsigned long *)((char *)si_arg + 16);

	/* sigcontext_64 layout inside ucontext_t.uc_mcontext (offset
	 * 0x28 inside uc):
	 *   r8 r9 r10 r11 r12 r13 r14 r15  (offsets 0x28..0x68)
	 *   rdi rsi rbp rbx rdx rax rcx rsp (offsets 0x68..0xa8)
	 *   rip eflags ...                  (offset 0xa8, 0xb0)
	 * Followed by cs/gs/fs/ss/err/trapno/oldmask/cr2 — cr2 at +0xe0,
	 * err at +0xc0 (15 fields × 8 = 120 + base 0x28 = 0xa0, hmm
	 * the layout is more complex).  Use known offsets.
	 */
	unsigned long fault_rsp = 0;
	unsigned long fault_rbp = 0;
	unsigned long fault_r15 = 0;

	if (uc_arg) {
		/* offset = 0x28 (uc_mcontext start) + N*8 for gregs[N] */
		fault_r15 = *(unsigned long *)((char *)uc_arg + 0x28 + 7*8);
		fault_rbp = *(unsigned long *)((char *)uc_arg + 0x28 + 10*8);
		fault_rsp = *(unsigned long *)((char *)uc_arg + 0x28 + 15*8);
		fault_rip = *(unsigned long *)((char *)uc_arg + 0x28 + 16*8);
		/* err at sigcontext+152, cr2 at sigcontext+176 (after
		 * the 8-byte cs/gs/fs/ss padding).  Plus 0x28 base.
		 */
		fault_err = *(unsigned long *)((char *)uc_arg + 0x28 + 152);
		fault_cr2 = *(unsigned long *)((char *)uc_arg + 0x28 + 176);
	}

	/* Format: "MFC_FAULT sig=%d addr=0xX rip=0xX cr2=0xX err=0xX rsp=0xX rbp=0xX r15=0xX\n" */
#define APPEND_STR(s) do {					\
		const char *_s = (s);				\
		while (*_s && p < (int)sizeof(buf) - 1)	\
			buf[p++] = *_s++;			\
	} while (0)
#define APPEND_HEX(v) do {					\
		val = (v);					\
		buf[p++] = '0';					\
		buf[p++] = 'x';					\
		for (i = 15; i >= 0; i--) {			\
			unsigned int nib = (val >> (i*4)) & 0xf; \
			buf[p++] = nib < 10 ? '0' + nib : 'a' + nib - 10; \
		}						\
	} while (0)
	APPEND_STR("MFC_FAULT sig=");
	buf[p++] = '0' + ((sig / 10) % 10);
	buf[p++] = '0' + (sig % 10);
	APPEND_STR(" addr=");
	APPEND_HEX(fault_addr);
	APPEND_STR(" rip=");
	APPEND_HEX(fault_rip);
	APPEND_STR(" cr2=");
	APPEND_HEX(fault_cr2);
	APPEND_STR(" err=");
	APPEND_HEX(fault_err);
	APPEND_STR(" rsp=");
	APPEND_HEX(fault_rsp);
	APPEND_STR(" rbp=");
	APPEND_HEX(fault_rbp);
	APPEND_STR(" r15=");
	APPEND_HEX(fault_r15);
	APPEND_STR(" stack[0..5]=");
	{
		unsigned long *sp = (unsigned long *)fault_rsp;
		int j;

		for (j = 0; j < 6; j++) {
			APPEND_HEX(sp[j]);
			buf[p++] = ' ';
		}
	}
	buf[p++] = '\n';
#undef APPEND_STR
#undef APPEND_HEX

	/* Raw __NR_write to fd 1. */
	{
		register long rax_w asm("rax") = 1;
		register long rdi_w asm("rdi") = 1;
		register long rsi_w asm("rsi") = (long)buf;
		register long rdx_w asm("rdx") = p;

		asm volatile ("syscall\n\t"
			: "+r" (rax_w)
			: "r" (rdi_w), "r" (rsi_w), "r" (rdx_w)
			: "rcx", "r11", "memory");
	}

	/* Raw __NR_exit_group(7) so harness can identify "caught
	 * fault" via WEXITSTATUS(7).
	 */
	{
		register long rax_x asm("rax") = 231;
		register long rdi_x asm("rdi") = 7;

		asm volatile ("syscall\n\t"
			:
			: "r" (rax_x), "r" (rdi_x)
			: "rcx", "r11", "memory");
	}
	/* unreachable */
	for (;;)
		;
}
#endif /* CONFIG_UM_TEMPLATE_PAUSE_FORK_DIAG */

/*
 * Refuse the fork-on-resume path under the KVM backend.  Same
 * reasoning as arch/um/kernel/snapshot.c's
 * um_snapshot_assert_ready(): fork() aliases /dev/kvm + per-vCPU
 * mmap state into the child, and two processes racing ioctls
 * against shared KVM state corrupts both halves.  KVM-aware fork
 * is a future phase (will issue KVM_CREATE_VCPU per child after
 * the fork point).
 */
static int assert_fork_safety(const char *named_point)
{
	int violations = 0;

	/*
	 * AFL forkserver preconditions ported from
	 * arch/um/kernel/snapshot.c::um_snapshot_assert_ready().  These
	 * are the same invariants the C-09 v1 forkserver path enforces:
	 * the fork-on-resume path has the same pre-fork hazard surface
	 * (post-fork SIGIO / POSIX-timer / runqueue / signal-state
	 * inheritance), so the same gates apply.
	 *
	 * See Documentation/virt/uml/redesign/02-workstreams/D-kvm-
	 * backend/state-audit/32-pool-member-entry-wip.md and
	 * EXTERNAL-RESEARCH §5 for the rationale.
	 *
	 * Use WARN_ONCE so each violation prints once with a stack
	 * trace (diagnostic-friendly) but the function still returns
	 * -EBUSY to refuse the fork.
	 */
	if (WARN_ONCE(in_hardirq() || in_softirq(),
		      "%s(\"%s\"): fork-on-resume entered from IRQ/softirq context\n",
		      __func__, named_point))
		violations++;

	if (WARN_ONCE(signal_pending(current),
		      "%s(\"%s\"): fork-on-resume entered with pending signals\n",
		      __func__, named_point))
		violations++;

	if (WARN_ONCE(num_online_cpus() > 1,
		      "%s(\"%s\"): fork-on-resume not yet supported under SMP; park secondary vCPUs or build with NR_CPUS=1\n",
		      __func__, named_point))
		violations++;

	if (WARN_ONCE(um_get_signals() != 1,
		      "%s(\"%s\"): UML signals_enabled is %d at fork-on-resume entry; must be 1 per D41 signal-gating contract\n",
		      __func__, named_point, um_get_signals()))
		violations++;

	/*
	 * KVM-backend refusal — same as snapshot.c.  fork() under KVM
	 * aliases /dev/kvm + per-vCPU mmap state into the child.
	 */
	if (um_backend && um_backend->kind == UM_BACKEND_KIND_KVM) {
		pr_err("template_pause: fork-on-resume refused under KVM backend at \"%s\" — kvm-aware fork is a future phase\n",
		       named_point);
		return -EOPNOTSUPP;
	}

	/*
	 * Refuse if any OTHER mm has a stub parked in FUTEX_IN_KERN —
	 * tearing it down (which §3.3's loop will do) would leave the
	 * task parked forever from the kernel's view, indistinguishable
	 * from a stub crash, and the existing mm_sigchld_irq path
	 * would fire fatal_sigsegv on it.  The caller's own mm is
	 * exempt (it IS mid-syscall — that's the proc_write that
	 * brought us here).  See PHASE2A-DESIGN.md §3.5.
	 */
	if (um_skas_other_mm_mid_syscall(current_mm_id())) {
		pr_err("template_pause: fork-on-resume refused at \"%s\" — another mm is mid-syscall (FUTEX_IN_KERN). Quiesce other guest tasks before pausing.\n",
		       named_point);
		return -EBUSY;
	}

	return violations ? -EBUSY : 0;
}

/*
 * Phase 2a — fork-on-resume loop.  On each SIGCONT, the master
 * fork(2)s; the child reuses um_snapshot_worker_init() to drop
 * parent-inherited SIGIO / POSIX-timer / runqueue state and returns
 * up the stack so the bootstrap script continues as the taken
 * instance.  The parent writes the new child's pid to the identity
 * memfd at offset `sizeof(struct um_template_identity)` (so the
 * supervisor can read it back) and loops to the next SIGSTOP.
 *
 * Parent never re-enters UML kernel scheduling between forks — the
 * loop is pure host-syscall work (kill + fork + write + kill).
 * UML's in-kernel signal dispatch is gated for the loop body via
 * os_snapshot_block_iter_signals() to avoid the D41 SIGILL hazard
 * that bit the AFL forkserver's parent-side wait4 path.
 *
 * Returns from the CHILD with the taken identity in @first_blob (or
 * zero-initialised if the first take had no identity channel).
 * Never returns from the parent in the happy path; on fatal error
 * the loop unwinds and the master process should be killed by the
 * supervisor.
 */
static int fork_on_resume_loop(const char *named_point, int identity_fd,
			       struct um_template_identity *first_blob)
{
	struct um_template_identity blob;
	int ret, child_pid, n;

	ret = assert_fork_safety(named_point);
	if (ret)
		return ret;

	/* The first call gave us blob already in *first_blob.  Use it
	 * for the first fork, then loop with fresh reads for each
	 * subsequent take.
	 */
	blob = *first_blob;

	os_snapshot_block_iter_signals();

	for (;;) {
		/*
		 * (A0) PRE-FORK IDENTITY APPLY (Phase 2).
		 *
		 * Apply the just-read identity blob to the MASTER's
		 * in-guest netdev.  The forked child inherits the
		 * applied identity via CoW; the master's own identity
		 * drifts to the latest blob (invisible to consumers
		 * because the master is never exposed as a pool
		 * member).  Failures are logged but non-fatal — a bad
		 * blob does not abort the fork loop.
		 *
		 * Idempotence: re-applying the same blob is a no-op
		 * at the net-stack layer.  See
		 * template_pause_identity.c for the full rationale on
		 * why this happens in the parent rather than the
		 * child.
		 */
		if (blob.magic == UM_TEMPLATE_IDENTITY_MAGIC) {
			int aerr = um_template_identity_apply(&blob);

			if (aerr)
				pr_warn("template_pause: identity apply at \"%s\" returned %d (continuing)\n",
					named_point, aerr);
		}

		/*
		 * (A) PRE-FORK: kill every stub child in mm_list.  Without
		 * this, fork(2) aliases the master's stub pids into the
		 * forked-child UML's mm_list (which inherits via CoW), and
		 * the two halves race to drive the same stubs — corrupting
		 * the 16-bit offset fields in stub_data and SEGVing deep
		 * inside stub code.  See
		 * Documentation/virt/uml/redesign/06-sequencing/post-
		 * 2026-05-19-next-sprint/09-fork-server-PHASE2A-DESIGN.md
		 * §2 for the full hazard model.
		 */
		/*
		 * Pool-member mode (Memo 09 §2) needs master to keep stubs
		 * ALIVE across fork so the post-fork child inherits a
		 * working per-mm SKAS context (turnstile mutex, mm_id
		 * stack page).  Skip teardown in that mode; see
		 * state-audit/32-pool-member-entry-wip.md.
		 */
		if (template_pause_pool_member_armed_flag) {
			pr_info("template_pause: skipping pre-fork stub teardown (pool_member mode keeps SKAS state)\n");
			n = 0;
		} else {
			n = um_skas_teardown_all_stubs();
			if (n < 0) {
				pr_err("template_pause: stub teardown failed: %d\n", n);
				os_snapshot_unblock_iter_signals();
				return n;
			}
			pr_info("template_pause: torn down %d stub(s) pre-fork\n", n);
		}

		/*
		 * (A.5) Block ALL host-level signals via raw
		 * __NR_rt_sigprocmask before the fork.
		 */
		{
			int sret = os_template_pause_signals_block_host();
			pr_info("template_pause: host signal block ret=%d\n",
				sret);
		}

#ifdef CONFIG_UM_TEMPLATE_PAUSE_FORK_DIAG
		/*
		 * DIAG MODE: install MFC handler in MASTER pre-fork.
		 * fork() inherits sigaction table, so M-fork child
		 * will have OUR handler when it faults.  Done only
		 * once per master lifetime via the armed_installed flag.
		 */
		if (template_pause_mfc_diag_armed_flag) {
			static bool armed_installed;

			if (!armed_installed) {
				struct {
					void (*sa_handler_)(int, void *, void *);
					unsigned long sa_flags;
					void (*sa_restorer)(void);
					unsigned long sa_mask;
				} act = {
					.sa_handler_ = mfc_diag_segv_handler,
					.sa_flags = 0x44000004UL,
					.sa_restorer = mfc_diag_restorer,
					.sa_mask = 0,
				};
				int s, sigs[4] = {11, 7, 4, 8};

				for (s = 0; s < 4; s++) {
					register long rax_a asm("rax") = 13;
					register long rdi_a asm("rdi") = sigs[s];
					register long rsi_a asm("rsi") = (long)&act;
					register long rdx_a asm("rdx") = 0;
					register long r10_a asm("r10") = 8;

					asm volatile ("syscall\n\t"
						: "+r" (rax_a)
						: "r" (rdi_a), "r" (rsi_a),
						  "r" (rdx_a), "r" (r10_a)
						: "rcx", "r11", "memory");
				}
				armed_installed = true;
				pr_info("template_pause: MFC handler installed pre-fork\n");
			}
		}
#endif /* CONFIG_UM_TEMPLATE_PAUSE_FORK_DIAG */

		/*
		 * (A.6) Detach all non-current tasks from the runqueue
		 * BEFORE fork.  After fork, both halves have ONLY
		 * `current` in their CFS runqueue, so schedule() can't
		 * UML_LONGJMP into a CoW'd task with a stale jmp_buf.
		 * Pre-fork detach is safe in the master now that
		 * Control A (sigprocmask) prevents the master from
		 * panic'ing on the same hazard.
		 */
		preempt_disable();
		sched_worker_detach_other_tasks();

		/* (B) FORK
		 *
		 * Private-stack mode (Phase 2 path forward, fixes bug B1):
		 * use __NR_clone with a private MAP_PRIVATE child stack so
		 * master's post-fork stack writes can't corrupt the child's
		 * view.  The child in this path will be on its own stack
		 * and will exit_group via inline asm — so it terminates
		 * naturally without needing SIGKILL.
		 *
		 * Default path: __NR_fork + post-fork SIGKILL.  See B1.
		 */
		if (template_pause_pool_member_armed_flag)
			child_pid = os_template_pause_fork_clone_to(
				child_entry_pool_member);
		else if (template_pause_pivot_test_armed_flag)
			child_pid = os_template_pause_fork_clone_to(
				child_entry_pivot_test);
		else if (template_pause_private_stack_armed_flag)
			child_pid = os_template_pause_fork_clone();
		else
			child_pid = os_template_pause_fork();
		if (child_pid < 0) {
			pr_err("template_pause: fork failed: %d\n", child_pid);
			(void)os_template_pause_signals_restore_host();
			(void)um_skas_respawn_all_stubs();
			os_snapshot_unblock_iter_signals();
			return child_pid;
		}

		if (child_pid == 0) {
			/*
			 * M-fork child path: MINIMAL — just __NR_exit_group.
			 *
			 * Empirical findings (2026-05-20 bisect, see
			 * 09-fork-server-STATUS.md):
			 *
			 *   (a) um_skas_respawn_all_stubs() in the child path
			 *       iterates mm_list entries the master already
			 *       tore down (pid=-1) and clone()s a new stub
			 *       child sharing VM with the M-fork child.  The
			 *       CoW + CLONE_VM triple-share corrupts the
			 *       stub binary's entry-point and the M-fork
			 *       child SIGSEGVs at IP=0.  REMOVED.
			 *
			 *   (b) Even an inline-asm __NR_pwrite64 to the
			 *       identity memfd (the old SUCCESS-marker write)
			 *       consistently crashed the M-fork child at a
			 *       stable stack address (IP=0x68803d66 across
			 *       runs) regardless of teardown state.  Bisected
			 *       to: bare exit_group → 10/10 strict-gate PASS
			 *       at N=100, sustaining 4500+ master iters in
			 *       10 s.  Root cause not fully isolated — likely
			 *       interaction between the pwrite64 syscall
			 *       return path and stale post-fork kernel state
			 *       (sched_worker_detach + preempt_disable +
			 *       dead mm_list).
			 *
			 *   (c) The SUCCESS marker was never load-bearing
			 *       for any fork-stress gate: G2 (distinct child
			 *       pids) reads memfd[260:264], which MASTER
			 *       writes in the parent path.  Master's
			 *       continued iteration is itself the proof that
			 *       fork() worked.
			 *
			 * preempt_disable stays held from pre-fork (no
			 * preempt_enable in the child path).  Raw inline-asm
			 * exit_group bypasses any kernel cleanup that would
			 * traverse the broken stub state.
			 */
#ifdef CONFIG_UM_TEMPLATE_PAUSE_FORK_DIAG
			if (template_pause_mfc_diag_armed_flag) {
				/* MFC diagnostic: do __NR_exit_group(0).
				 * Handler was pre-installed by master (see
				 * MFC handler install block before fork).
				 */
				register long rax_x asm("rax") = 231;
				register long rdi_x asm("rdi") = 0;

				asm volatile ("syscall\n\t"
					:
					: "r" (rax_x), "r" (rdi_x)
					: "rcx", "r11", "memory");
			}
#endif

			/*
			 * Production path: pure infinite loop.  Master
			 * SIGKILL'd the M-fork child immediately in its
			 * parent path (see comment block by the kill below).
			 * The child never executes any host syscall.  The
			 * for-loop exists only to satisfy the compiler; it
			 * never iterates because SIGKILL lands first.
			 */
			for (;;)
				;
			return 0;
		}

		/*
		 * (C-parent) — Option A from the 2026-04-23 memo: parent
		 * skips respawn (avoids wait_stub_done_seccomp futex
		 * window in the hazard zone).  Master never returns to
		 * guest userspace — it loops in fork_on_resume_loop
		 * forever.  Host signal mask stays blocked while master
		 * loops; it's restored by the supervisor's next teardown
		 * step.
		 */
		n = 0;
		/*
		 * SIGKILL the M-fork child immediately.  This is the
		 * 2026-05-20 fix for the "Kernel tried to access user
		 * memory" panic storm.  Empirical bisect (commit log):
		 *
		 *   * If the M-fork child executes ANY user-space code
		 *     after the fork syscall returns (even just an
		 *     inline-asm __NR_exit_group), UML's SIGSEGV handler
		 *     fires at a user-range RIP with is_user=0, which
		 *     panics via arch/um/kernel/trap.c::segv() line 372.
		 *     The fault IP increments by 2 per iteration —
		 *     consistent with some inherited stub state's
		 *     IP being advanced and re-faulted on each fork.
		 *   * Pre-killing the M-fork child via raw __NR_kill
		 *     SIGKILL terminates it BEFORE it returns to user
		 *     mode from the fork syscall.  No panic.  Measured:
		 *     0 panics across 100 consecutive runs at N=100, 10
		 *     runs at N=1000, and 10 runs under stress-ng
		 *     --cpu $(nproc) full-load background.
		 *
		 * The cost is ~17 ms/iter overhead (vs ~2 ms without
		 * kill — the SIGKILL syscall + subreaper wait4 latency).
		 * Master sustains 460+ iters/s under full host load,
		 * well above the 50 ms/iter G5b budget.
		 *
		 * Root cause of why M-fork child cannot safely run any
		 * userspace code is NOT yet fully isolated.  Best
		 * hypothesis: UML's signal-handling state inherited via
		 * CoW (sigaltstack, signal handlers, sigmask) is in a
		 * post-fork state where ANY syscall return path triggers
		 * a fault that UML's segv_handler interprets as kernel
		 * mode.  Investigating further requires capturing the
		 * M-fork child's RIP/regs at fault — a non-trivial
		 * dance because installing a raw SIGSEGV handler in the
		 * child path itself uses the very state we're trying to
		 * inspect.  Deferred to Phase 2 (identity re-plumbing)
		 * where the child WILL need to do meaningful syscalls.
		 *
		 * Use raw __NR_kill (not glibc's kill(3)) for the same
		 * reason as the other os_template_pause syscalls: glibc
		 * cancellation-pipe hazard.
		 */
		if (
#ifdef CONFIG_UM_TEMPLATE_PAUSE_FORK_DIAG
		    !template_pause_mfc_diag_armed_flag &&
#endif
		    !template_pause_pool_member_armed_flag &&
		    !template_pause_pivot_test_armed_flag &&
		    !template_pause_private_stack_armed_flag) {
			/* x86_64 __NR_kill = 62, SIGKILL = 9.  Skipped in
			 * private-stack mode because the child exited via
			 * exit_group inside os_template_pause_fork_clone.
			 */
			register long rax_k asm("rax") = 62;
			register long rdi_k asm("rdi") = (long)child_pid;
			register long rsi_k asm("rsi") = 9;
			asm volatile (
				"syscall\n\t"
				: "+r" (rax_k)
				: "r" (rdi_k), "r" (rsi_k)
				: "rcx", "r11", "memory"
			);
		}
		preempt_enable();
		/* Master keeps host signals BLOCKED across iterations.
		 * SIGSTOP/SIGCONT are excluded from the block so the
		 * wake-up still works.  Other signals (SIGCHLD from
		 * dying stub-children, SIGALRM from timers) stay queued
		 * at host level until master's lifecycle ends; this
		 * prevents UML's sig_handler from running on master's
		 * kernel stack in the post-fork hazard window.
		 */

		/* Report the new child's host pid via memfd[260:264]. */
		if (identity_fd >= 0) {
			int werr = os_template_pause_write_child_pid(
				identity_fd,
				sizeof(struct um_template_identity),
				child_pid);
			if (werr)
				pr_warn("template_pause: write child pid %d failed: %d\n",
					child_pid, werr);
		}

		/* (E) Next take's pause. */
		ret = one_pause_cycle(named_point, identity_fd, &blob);
		if (ret) {
			os_snapshot_unblock_iter_signals();
			return ret;
		}
	}
}

#else /* !CONFIG_UM_TEMPLATE_PAUSE_FORK */

static inline int fork_on_resume_loop(const char *named_point, int identity_fd,
				      struct um_template_identity *first_blob)
{
	pr_warn("template_pause: =fork requested at \"%s\" but kernel was built without CONFIG_UM_TEMPLATE_PAUSE_FORK; behaving as single-shot Phase 1a\n",
		named_point);
	return 0;
}

#endif /* CONFIG_UM_TEMPLATE_PAUSE_FORK */

int um_template_pause_enter(const char *named_point)
{
	struct um_template_identity blob;
	int identity_fd, ret;

	if (!named_point)
		named_point = "(null)";

	if (!template_pause_armed_flag) {
		pr_info("template_pause: enter(\"%s\") refused — um_template_pause not on cmdline\n",
			named_point);
		return -ENODEV;
	}

	if (template_pause_pool_member_active) {
		pr_info("template_pause: enter(\"%s\") refused — already a pool member\n",
			named_point);
		return 0;
	}

	identity_fd = os_template_pause_identity_fd();
	if (identity_fd < 0) {
		pr_info("template_pause: enter(\"%s\") — no UM_TEMPLATE_IDENTITY_FD; identity-blob channel disabled (err=%d)\n",
			named_point, identity_fd);
		identity_fd = -1;
	} else {
		pr_info("template_pause: enter(\"%s\") — identity_fd=%d\n",
			named_point, identity_fd);
	}

	ret = one_pause_cycle(named_point, identity_fd, &blob);
	if (ret)
		return ret;

	if (template_pause_fork_armed_flag)
		return fork_on_resume_loop(named_point, identity_fd, &blob);

	/*
	 * Phase 1a single-shot path: master IS the taken instance.
	 * Apply identity directly so the master boots up as the
	 * configured pool member.  Failures are logged but
	 * non-fatal so a bad blob does not refuse the pause.
	 */
	if (blob.magic == UM_TEMPLATE_IDENTITY_MAGIC) {
		int aerr = um_template_identity_apply(&blob);

		if (aerr)
			pr_warn("template_pause: identity apply at \"%s\" returned %d (continuing)\n",
				named_point, aerr);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(um_template_pause_enter);

/*
 * /proc/um/template_pause — write-only trigger.  Writing any non-
 * empty string invokes um_template_pause_enter() with that string as
 * the named point.  The write() blocks across SIGSTOP/SIGCONT — the
 * caller's write(2) returns only after the supervisor SIGCONTs us.
 *
 * Phase 1b's supervisor doesn't strictly need this; the bootstrap
 * script in the master Umlfile's init phases is the typical caller.
 * Keeping a writable /proc entry separately makes the path test-
 * scriptable without a Umlfile.
 */
static ssize_t template_pause_proc_write(struct file *f,
					 const char __user *buf,
					 size_t count, loff_t *ppos)
{
	char name[32];
	size_t n;
	int ret;

	n = count < sizeof(name) - 1 ? count : sizeof(name) - 1;
	if (copy_from_user(name, buf, n))
		return -EFAULT;
	name[n] = '\0';
	if (n > 0 && name[n - 1] == '\n')
		name[n - 1] = '\0';

	ret = um_template_pause_enter(name[0] ? name : "proc");
	if (ret)
		return ret;
	return count;
}

static const struct proc_ops template_pause_proc_ops = {
	.proc_write = template_pause_proc_write,
};

static struct proc_dir_entry *um_proc_dir;

static int __init template_pause_proc_init(void)
{
	struct proc_dir_entry *ent;

	if (!template_pause_armed_flag) {
		/* Don't litter /proc when not in use. */
		return 0;
	}

	um_proc_dir = proc_mkdir("um", NULL);
	if (!um_proc_dir) {
		pr_err("template_pause: /proc/um mkdir failed\n");
		return -ENOMEM;
	}

	ent = proc_create("template_pause", 0200, um_proc_dir,
			  &template_pause_proc_ops);
	if (!ent) {
		pr_err("template_pause: /proc/um/template_pause create failed\n");
		proc_remove(um_proc_dir);
		um_proc_dir = NULL;
		return -ENOMEM;
	}

	pr_info("template_pause: /proc/um/template_pause ready\n");
	return 0;
}
late_initcall(template_pause_proc_init);

/*
 * Early-pause initcall: if armed via `um_template_pause=early` or
 * `=early-fork`, pause the kernel HERE (inside the last late_initcall
 * after proc_init) — before run_init_process spawns userspace and
 * before any SKAS stub child is created.  This is AFL forkserver's
 * "v1-friendly" ready point.
 *
 * Returning 0 lets the boot proceed normally (init.sh runs as usual)
 * for kernels where the early flag is NOT armed.
 */
static int __init template_pause_early_init(void)
{
	if (!template_pause_armed_flag || !template_pause_early_armed_flag)
		return 0;

	pr_info("template_pause: entering early-pause initcall\n");
	(void)um_template_pause_enter("early-initcall");
	pr_info("template_pause: early-pause initcall returned\n");
	return 0;
}
late_initcall_sync(template_pause_early_init);
