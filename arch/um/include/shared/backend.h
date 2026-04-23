/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML backend ops table contract — shared header.
 *
 * Included by both kernel-side TUs and USER-side TUs (those built
 * with USER_CFLAGS via arch/um/scripts/Makefile.rules). Holds the
 * full struct definitions and the dispatch surface so that USER TUs
 * which need `um_backend_dispatch(...)` (e.g. the trap loop in
 * arch/um/os-Linux/skas/process.c::userspace) can deref `um_backend`
 * fields directly in DYNAMIC builds.
 *
 * Per-op semantics: see Documentation/virt/uml/backend-contract.rst.
 *
 * The kernel-only init API (`init_backend()` declaration plus
 * helpers that touch kernel structs in their argument lists) lives
 * in <asm/backend.h>, which `#include`s this header.
 */
#ifndef __SHARED_UM_BACKEND_H
#define __SHARED_UM_BACKEND_H

/*
 * USER TUs in arch/um/ have __KERNEL__ + __UM_HOST__ defined and
 * pull in <stddef.h>+<sys/types.h> via user.h, not <linux/types.h>.
 * Use the kernel u64 definition directly in kernel context, and fall
 * back to stdint in USER context.
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
struct mm_id;

/*
 * Bumped on op signature changes (breaking). Adding new ops at the
 * end of struct um_backend_ops is non-breaking; older backends that
 * lack the new field would have it as NULL — but the dispatch macro
 * does NOT synthesize -ENOSYS for missing ops (it is a function-
 * pointer call with no NULL check). All in-tree backends MUST
 * populate every op; HOT ops are validated at init by
 * validate_hot_ops() in arch/um/kernel/backend.c. See
 * Documentation/virt/uml/backend-contract.rst.
 */
#define UM_BACKEND_CONTRACT_VERSION  1u

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
 * `backend=` / `backend_force=` boot params.
 */
struct um_backend_args {
	enum um_backend_kind requested;	/* boot-param override; 0 = auto */
	bool force;			/* if requested unavailable, panic */
	const char *runtime_opts;	/* opaque per-backend knobs */
};

/*
 * Timer mode tag for um_backend_dispatch(set_timer, ...).
 *
 *   UM_TIMER_DISABLE   — disable timer; deadline_ns ignored
 *   UM_TIMER_ONE_SHOT  — fire once after deadline_ns nanoseconds
 *   UM_TIMER_PERIODIC  — fire every deadline_ns nanoseconds
 */
enum um_timer_mode {
	UM_TIMER_DISABLE = 0,
	UM_TIMER_ONE_SHOT,
	UM_TIMER_PERIODIC,
};

/*
 * struct um_backend_ops — every backend implements this.
 *
 * Conventions:
 *   - All ops are synchronous.
 *   - All ops MUST be non-NULL for in-tree backends.
 *     init_backend() validates HOT ops at boot and panics otherwise;
 *     cold ops that aren't yet implemented should return -EOPNOTSUPP
 *     (e.g. read/write_guest_regs awaiting KGDB integration).
 *   - HOT ops are inlined to direct calls in single-backend builds
 *     via the dispatch macro below.
 *   - Per-backend state is global (singletons). Per-mm state lives
 *     in `struct mm_id`; per-thread state lives in `task->thread`.
 */
struct um_backend_ops {
	const char			*name;
	enum um_backend_kind		kind;
	u32				contract_version;

	/*
	 * Capability flags — observable metadata that host-side
	 * (os-Linux/) code consults instead of the legacy
	 * `using_seccomp` int. Added as the first step of the
	 * A-workstream Phase 2 extraction per D59; each flag is
	 * populated alongside the backend's ops struct and read
	 * via `um_backend->flag`.
	 *
	 * uses_stub_reaper: true when the backend's mm_attach
	 * creates a host child (`mm_id->pid > 0`) whose lifecycle
	 * is driven by a SIGCHLD-registered reaper IRQ. Seccomp
	 * sets this because Berg's seccomp-mode stub is reaped
	 * asynchronously; ptrace and KVM leave it false (ptrace
	 * handles reaping inline within the trap loop via the
	 * ptrace stop mechanism; KVM has no host stub child).
	 * Consulted by arch/um/os-Linux/signal.c::set_handler
	 * (mask SIGCHLD in other handlers) and arch/um/os-Linux/
	 * process.c::init_new_thread_signals (install the SIGCHLD
	 * handler itself).
	 *
	 * has_syscall_stub_fd_map: true when the backend's stub-
	 * syscall ABI carries per-mm FD indirection
	 * (`mm_id->syscall_fd_map` / `syscall_fd_num`) for
	 * SCM_RIGHTS fd-passing into the stub child. Seccomp
	 * sets this (Berg's stub uses a per-mm sendmsg-delivered
	 * fd table); ptrace and KVM leave it false. Consulted by
	 * arch/um/os-Linux/skas/mem.c::syscall_stub_dump_error
	 * (dump the fd-map on error), get_stub_fd (fd→slot
	 * indirection), um_stub_mm_map (coalesce-previous lookup),
	 * and do_syscall_stub (fd_num reset after a batch).
	 *
	 * stub_syscall_uses_futex: true when the backend wakes
	 * the stub child to process a batched syscall queue via
	 * a futex + wait_stub_done_seccomp round-trip; false when
	 * it uses PTRACE_SETREGS + PTRACE_CONT + wait_stub_done
	 * instead. Seccomp true, ptrace false, KVM false (KVM
	 * doesn't use do_syscall_stub at all). Consulted by the
	 * dispatch-mechanism branch of arch/um/os-Linux/skas/
	 * mem.c::do_syscall_stub.
	 */
	bool				uses_stub_reaper;
	bool				has_syscall_stub_fd_map;
	bool				stub_syscall_uses_futex;

	/* Lifecycle and trap (4) */
	int  (*probe)(void);
	int  (*init)(const struct um_backend_args *args);
	void (*shutdown)(void);
	void (*run_userspace)(struct uml_pt_regs *regs);	/* HOT */

	/* Memory (4) */
	int  (*mm_attach)(struct mm_id *id);
	void (*mm_detach)(struct mm_id *id);
	int  (*mm_map)(struct mm_id *id,			/* HOT */
		       unsigned long va, unsigned long len,
		       int prot, int phys_fd, u64 offset);
	int  (*mm_unmap)(struct mm_id *id,			/* HOT */
			 unsigned long va, unsigned long len);

	/* Scheduling (4) */
	int  (*thread_create)(struct task_struct *p,
			      void *stack, void (*handler)(void));
	int  (*thread_start_idle)(void *stack, struct thread_struct *t);
	void (*context_switch)(struct task_struct *prev,	/* HOT */
			       struct task_struct *next);
	int  (*ipi_send)(int cpu, int vector);

	/* Time (3) */
	u64  (*read_clock_ns)(void);				/* HOT */
	int  (*set_timer)(int cpu, u64 deadline_ns,
			  enum um_timer_mode mode);
	u64  (*read_persistent_clock_ns)(void);

	/* Debug / introspection (3) */
	void (*init_thread_regs)(unsigned long *gp, unsigned long *fp);
	int  (*read_guest_regs)(struct task_struct *t, struct pt_regs *regs);
	int  (*write_guest_regs)(struct task_struct *t,
				 const struct pt_regs *regs);
};

/*
 * The boot-time-selected backend. NULL until init_backend() runs;
 * thereafter immutable. In *_ONLY builds the dispatch macro bypasses
 * this entirely; the pointer is still set so introspection works
 * (e.g. `um_backend->name` for diagnostics).
 */
extern const struct um_backend_ops *um_backend;

/*
 * Per-backend op symbols follow the naming convention `<kind>_<op>`,
 * e.g. seccomp_run_userspace(). The dispatch macro below references
 * them by token-paste; the prototypes must be visible at every call
 * site, gated by CONFIG_UM_BACKEND_<kind>.
 */
#ifdef CONFIG_UM_BACKEND_PTRACE
/* A-02.6 */
u64 ptrace_read_persistent_clock_ns(void);
/* A-02.HOT-1 */
void ptrace_run_userspace(struct uml_pt_regs *regs);
/* A-02.HOT-2 */
int ptrace_mm_map(struct mm_id *id, unsigned long va, unsigned long len,
		  int prot, int phys_fd, u64 offset);
int ptrace_mm_unmap(struct mm_id *id, unsigned long va, unsigned long len);
/* A-02.HOT-3 */
void ptrace_context_switch(struct task_struct *prev, struct task_struct *next);
int ptrace_thread_create(struct task_struct *p, void *stack,
			 void (*handler)(void));
int ptrace_thread_start_idle(void *stack, struct thread_struct *t);
/* A-02.COLD-1 */
u64 ptrace_read_clock_ns(void);
int ptrace_set_timer(int cpu, u64 deadline_ns, enum um_timer_mode mode);
/* A-02.COLD-2 */
int ptrace_ipi_send(int cpu, int vector);
void ptrace_init_thread_regs(unsigned long *gp, unsigned long *fp);
int ptrace_read_guest_regs(struct task_struct *t, struct pt_regs *regs);
int ptrace_write_guest_regs(struct task_struct *t, const struct pt_regs *regs);
/* A-02.COLD-3 */
int ptrace_probe(void);
int ptrace_init(const struct um_backend_args *args);
void ptrace_shutdown(void);
int ptrace_mm_attach(struct mm_id *id);
void ptrace_mm_detach(struct mm_id *id);
#endif

#ifdef CONFIG_UM_BACKEND_SECCOMP
/* A-03.S1 */
int seccomp_probe(void);
int seccomp_init(const struct um_backend_args *args);
void seccomp_shutdown(void);
void seccomp_run_userspace(struct uml_pt_regs *regs);
int seccomp_mm_attach(struct mm_id *id);
void seccomp_mm_detach(struct mm_id *id);
int seccomp_mm_map(struct mm_id *id, unsigned long va, unsigned long len,
		   int prot, int phys_fd, u64 offset);
int seccomp_mm_unmap(struct mm_id *id, unsigned long va, unsigned long len);
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

#ifdef CONFIG_UM_BACKEND_KVM
/*
 * D-02 scaffold. Every op lives in arch/um/backend/kvm/. `probe` and
 * `init` are the only ones that do real work today: probe opens
 * /dev/kvm to confirm the host has KVM available, init stashes the
 * fd for the real runtime code that lands in D-03..D-06. Every other
 * op returns -EOPNOTSUPP for now; dispatch to them would panic per
 * the contract, so this backend is only selectable when the operator
 * has explicitly asked for it via Kconfig + boot arg and is ready
 * for "it doesn't boot yet" semantics.
 */
int kvm_probe(void);
int kvm_init(const struct um_backend_args *args);
void kvm_shutdown(void);
void kvm_run_userspace(struct uml_pt_regs *regs);
int kvm_mm_attach(struct mm_id *id);
void kvm_mm_detach(struct mm_id *id);
int kvm_mm_map(struct mm_id *id, unsigned long va, unsigned long len,
	       int prot, int phys_fd, u64 offset);
int kvm_mm_unmap(struct mm_id *id, unsigned long va, unsigned long len);
int kvm_thread_create(struct task_struct *p, void *stack,
		      void (*handler)(void));
int kvm_thread_start_idle(void *stack, struct thread_struct *t);
void kvm_context_switch(struct task_struct *prev, struct task_struct *next);
int kvm_ipi_send(int cpu, int vector);
u64 kvm_read_clock_ns(void);
int kvm_set_timer(int cpu, u64 deadline_ns, enum um_timer_mode mode);
u64 kvm_read_persistent_clock_ns(void);
void kvm_init_thread_regs(unsigned long *gp, unsigned long *fp);
int kvm_read_guest_regs(struct task_struct *t, struct pt_regs *regs);
int kvm_write_guest_regs(struct task_struct *t, const struct pt_regs *regs);
#endif

/*
 * The dispatch macro. In single-backend-only builds, expands to the
 * named backend symbol (direct call, zero indirect-dispatch cost). In
 * dynamic builds, expands to a function-pointer call through
 * `um_backend`.
 *
 * Usage:
 *     um_backend_dispatch(read_clock_ns);
 *     um_backend_dispatch(mm_map, id, va, len, prot, fd, off);
 */
#if defined(CONFIG_UM_BACKEND_PTRACE_ONLY)
# define um_backend_dispatch(op, ...) ptrace_##op(__VA_ARGS__)
#elif defined(CONFIG_UM_BACKEND_SECCOMP_ONLY)
# define um_backend_dispatch(op, ...) seccomp_##op(__VA_ARGS__)
#elif defined(CONFIG_UM_BACKEND_KVM_ONLY)
# define um_backend_dispatch(op, ...) kvm_##op(__VA_ARGS__)
#else
# define um_backend_dispatch(op, ...) (um_backend->op(__VA_ARGS__))
#endif

#endif /* __SHARED_UM_BACKEND_H */
