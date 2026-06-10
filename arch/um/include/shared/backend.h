/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML backend ops table contract - shared header.
 *
 * Included by both kernel-side translation units and host-built UML
 * translation units. Holds the full struct definitions and the dispatch
 * surface so code that needs um_backend_dispatch(), such as the trap loop
 * in arch/um/os-Linux/skas/process.c, can dereference um_backend fields
 * directly in DYNAMIC builds.
 *
 * Per-op semantics: see Documentation/virt/uml/backend-contract.rst.
 *
 * The kernel-only init API (init_backend() declaration plus
 * helpers that touch kernel structs in their argument lists) lives
 * in <asm/backend.h>, which #includes this header.
 */
#ifndef __SHARED_UM_BACKEND_H
#define __SHARED_UM_BACKEND_H

/*
 * Host-built UML translation units have __KERNEL__ + __UM_HOST__ defined
 * and pull in <stddef.h>+<sys/types.h> via user.h, not <linux/types.h>.
 * Use the kernel u64 definition directly in kernel context, and fall back
 * to stdint in host context.
 */
#ifdef __UM_HOST__
#include <stdint.h>
typedef uint64_t u64;
typedef uint32_t u32;
#include <stdbool.h>
#else
#include <linux/types.h>
#include <linux/kconfig.h>
#endif

struct uml_pt_regs;
struct task_struct;
struct thread_struct;
struct pt_regs;
struct mm_struct;
struct mm_id;
struct um_memory_region;

/*
 * Bumped on op signature changes. Adding new ops at the end of
 * struct um_backend_ops is source-compatible, but the dispatch macro does
 * not synthesize -ENOSYS for missing ops. All in-tree backends must
 * populate every required dispatch op; required ops are validated at
 * init by validate_required_ops() in arch/um/kernel/backend.c. See
 * Documentation/virt/uml/backend-contract.rst.
 */
#define UM_BACKEND_CONTRACT_VERSION  2u

enum um_backend_kind {
	UM_BACKEND_KIND_NONE = 0,
	UM_BACKEND_KIND_PTRACE,
	UM_BACKEND_KIND_SECCOMP,
	UM_BACKEND_KIND_KVM,
};

/*
 * Forwarded from kernel command line by init_backend(). All fields
 * have a "0 means 'no preference'" semantics. Populated by
 * uml_backend_config() in arch/um/os-Linux/start_up.c from the
 * backend= boot param.
 */
struct um_backend_args {
	enum um_backend_kind requested;	/* boot-param override; 0 = auto */
	bool force;			/* if requested unavailable, panic */
	const char *runtime_opts;	/* opaque per-backend knobs */
};

/*
 * Timer mode tag for um_backend_dispatch(set_timer, ...).
 *
 *   UM_TIMER_DISABLE   - disable timer; deadline_ns ignored
 *   UM_TIMER_ONE_SHOT  - fire once after deadline_ns nanoseconds
 *   UM_TIMER_PERIODIC  - fire every deadline_ns nanoseconds
 */
enum um_timer_mode {
	UM_TIMER_DISABLE = 0,
	UM_TIMER_ONE_SHOT,
	UM_TIMER_PERIODIC,
};

/*
 * struct um_backend_ops - every backend implements this.
 *
 * Conventions:
 *   - All ops are synchronous.
 *   - Required dispatch ops must be non-NULL for in-tree backends.
 *     init_backend() validates them at boot and panics otherwise;
 *     unsupported optional ops should return -EOPNOTSUPP
 *     (e.g. read/write_guest_regs awaiting KGDB integration).
 *   - Required dispatch ops are inlined to direct calls in
 *     single-backend builds via the dispatch macro below.
 *   - Per-backend state is global (singletons). Per-mm and per-thread
 *     state are looked up by the backend from the struct mm_struct *
 *     and struct task_struct * arguments respectively. Backend
 *     callbacks use struct mm_struct * so each backend can manage
 *     its own per-mm storage layout (seccomp uses mm->context.id; v2's
 *     per-mm worker process model uses a hash off the mm pointer).
 */
struct um_backend_ops {
	const char			*name;
	enum um_backend_kind		kind;
	u32				contract_version;

	/*
	 * Capability flags: observable metadata that host-side
	 * (os-Linux/) code consults instead of open-coded backend checks.
	 * Each flag is populated alongside the backend's ops struct and
	 * read via um_backend->flag.
	 *
	 * uses_stub_reaper: true when the backend's mm_attach
	 * creates a host child (mm_id->pid > 0) whose lifecycle
	 * is driven by a SIGCHLD-registered reaper IRQ. Seccomp
	 * sets this because its stub is reaped asynchronously;
	 * KVM leaves it false because it has no host stub child.
	 * Consulted by signal setup so only backends with a stub reaper
	 * reserve SIGCHLD for child lifecycle events.
	 *
	 * has_syscall_stub_fd_map: true when the backend's stub-
	 * syscall ABI carries per-mm FD indirection
	 * (mm_id->syscall_fd_map / syscall_fd_num) for
	 * SCM_RIGHTS fd-passing into the stub child. Seccomp
	 * sets this because its stub uses a per-mm sendmsg-delivered
	 * fd table; KVM leaves it false. Consulted by
	 * seccomp stub error reporting, fd-slot lookup, map coalescing,
	 * and batch reset paths.
	 *
	 * stub_syscall_uses_futex: true when the backend wakes
	 * the stub child to process a batched syscall queue via
	 * a futex + wait_stub_done_seccomp round-trip; false when
	 * it uses PTRACE_SETREGS + PTRACE_CONT + wait_stub_done
	 * instead. Seccomp true, ptrace false, KVM false (KVM
	 * does not use do_syscall_stub at all). Consulted by the
	 * seccomp stub-dispatch path and the initial stub wait/setup
	 * path.
	 *
	 * stub_child_runs_seccomp: true when the stub child
	 * installs its own SIGSYS-filter (and dispatches via
	 * stub_signal_interrupt); false when the parent traces
	 * it via ptrace (and dispatches via stub_segv_handler).
	 * Seccomp true, ptrace false, KVM false (no stub child
	 * on KVM). Consulted when building the clone-trampoline init
	 * data sent over the tramp sockpair.
	 */
	bool				uses_stub_reaper;
	bool				has_syscall_stub_fd_map;
	bool				stub_syscall_uses_futex;
	bool				stub_child_runs_seccomp;

	/* Lifecycle and trap */
	int  (*probe)(void);
	int  (*init)(const struct um_backend_args *args);
	void (*shutdown)(void);
	void (*vcpu_run)(struct uml_pt_regs *regs);		/* HOT */

	/*
	 * Memory. Backend allocates whatever per-mm state it needs:
	 *   seccomp:  fork stub-child host process, key on mm->context.id
	 *   kvm-v2:   fork per-mm worker process + per-mm KVM context
	 *
	 * mm_region_added / mm_region_removed / mm_region_protected take
	 * a const struct um_memory_region *. The struct is owned by
	 * the mm-arbiter and short-lived (single drain pass); backends
	 * consume it inline and may stash per-region state in
	 * region->backend_data (set by mm_region_added; cleared by
	 * mm_region_removed). The seccomp backend ignores backend_data;
	 * v2 uses it as the memslot ID.
	 *
	 * mm_region_protected lets a backend receive notifications when
	 * an existing region's protection changes. Backends may leave it
	 * NULL; the mm-arbiter falls back to the remove+add sequence.
	 */
	int  (*mm_create)(struct mm_struct *mm);
	void (*mm_destroy)(struct mm_struct *mm);
	int  (*mm_region_added)(struct mm_struct *mm,		/* HOT */
				const struct um_memory_region *region);
	int  (*mm_region_removed)(struct mm_struct *mm,		/* HOT */
				  const struct um_memory_region *region);
	int  (*mm_region_protected)(struct mm_struct *mm,
				    const struct um_memory_region *region);

	/* Scheduling */
	int  (*thread_create)(struct task_struct *p,
			      void *stack, void (*handler)(void));
	int  (*thread_start_idle)(void *stack, struct thread_struct *t);
	void (*context_switch)(struct task_struct *prev,	/* HOT */
			       struct task_struct *next);
	int  (*ipi_send)(int cpu, int vector);

	/*
	 * Optional cross-vCPU TLB-flush kick. Called from um_tlb_sync
	 * after a successful drain. Backends with per-vCPU guest TLBs
	 * (kvm-v2) must wake other UML CPUs so they dispatch and
	 * flush their guest TLBs. seccomp leaves this NULL; its host
	 * mm operations already kick all CPUs via mmu_notifier. Marked
	 * may-be-NULL; um_tlb_sync NULL-checks before calling.
	 */
	void (*tlb_kick_others)(struct mm_struct *mm);

	/* Time */
	u64  (*read_clock_ns)(void);				/* HOT */
	int  (*set_timer)(int cpu, u64 deadline_ns,
			  enum um_timer_mode mode);
	u64  (*read_persistent_clock_ns)(void);

	/* Debug and introspection */
	void (*init_thread_regs)(unsigned long *gp, unsigned long *fp);
	int  (*read_guest_regs)(struct task_struct *t, struct pt_regs *regs);
	int  (*write_guest_regs)(struct task_struct *t,
				 const struct pt_regs *regs);
};

/*
 * The boot-time-selected backend. NULL until init_backend() runs;
 * thereafter immutable. In *_ONLY builds the dispatch macro bypasses
 * this entirely; the pointer is still set so debug output can still
 * report um_backend->name.
 */
extern const struct um_backend_ops *um_backend;

/*
 * Per-backend op symbols follow the naming convention <kind>_<op>,
 * e.g. seccomp_vcpu_run(). The dispatch macro below references them by
 * token-paste; declarations must be visible at every call site, gated by
 * CONFIG_UM_BACKEND_<kind>.
 */
#ifdef CONFIG_UM_BACKEND_SECCOMP
int seccomp_probe(void);
int seccomp_init(const struct um_backend_args *args);
void seccomp_shutdown(void);
void seccomp_vcpu_run(struct uml_pt_regs *regs);
int seccomp_mm_create(struct mm_struct *mm);
void seccomp_mm_destroy(struct mm_struct *mm);
int seccomp_mm_region_added(struct mm_struct *mm,
			    const struct um_memory_region *region);
int seccomp_mm_region_removed(struct mm_struct *mm,
			      const struct um_memory_region *region);
int seccomp_thread_create(struct task_struct *p, void *stack,
			  void (*handler)(void));
int seccomp_thread_start_idle(void *stack, struct thread_struct *t);
void seccomp_context_switch(struct task_struct *prev, struct task_struct *next);
int seccomp_ipi_send(int cpu, int vector);
u64 seccomp_read_clock_ns(void);
int seccomp_set_timer(int cpu, u64 deadline_ns, enum um_timer_mode mode);
u64 seccomp_read_persistent_clock_ns(void);
void seccomp_init_thread_regs(unsigned long *gp, unsigned long *fp);
int seccomp_read_guest_regs(struct task_struct *t, struct pt_regs *regs);
int seccomp_write_guest_regs(struct task_struct *t, const struct pt_regs *regs);
#endif

/*
 * KVM v2 backend ops live under arch/um/backend/kvm-v2 and are
 * selected through the dynamic backend table.
 */

/*
 * The dispatch macro. In single-backend-only builds, expands to the
 * named backend symbol (direct call, zero indirect-dispatch cost). In
 * dynamic builds, expands to a function-pointer call through
 * um_backend.
 *
 * Usage:
 *     um_backend_dispatch(read_clock_ns);
 *     um_backend_dispatch(mm_region_added, mm, &region);
 */
#if defined(CONFIG_UM_BACKEND_SECCOMP_ONLY)
# define um_backend_dispatch(op, ...) seccomp_##op(__VA_ARGS__)
#else
# define um_backend_dispatch(op, ...) (um_backend->op(__VA_ARGS__))
#endif

#endif /* __SHARED_UM_BACKEND_H */
