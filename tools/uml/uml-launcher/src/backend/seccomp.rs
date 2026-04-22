// SPDX-License-Identifier: GPL-2.0
//
// Per-backend BPF seccomp filters for C-10 v2 vhost-user backends.
//
// Design (D52):
//   - Allow-list posture. Every syscall outside the configured set
//     triggers SECCOMP_RET_KILL_PROCESS — the same default that
//     Firecracker and crosvm use for their backend processes.
//   - Each device class composes its filter from a shared "event
//     loop" baseline (epoll + read/write + memory-map setup + a few
//     bookkeeping primitives the rust-vmm stack relies on) plus a
//     class-specific allowance set (tap ioctls for net,
//     preadv/pwritev for block, etc.).
//   - Applied after `VhostUserDaemon::new()` — which does its own
//     eventfd / mmap setup — and before `.serve()` enters the
//     run loop. The short window between those two points is the
//     "fd plumbing done, attack surface about to open" moment
//     Firecracker's per-thread apply guard also targets.
//   - No `dev` fallback knob; once shipped, every backend class
//     runs with its filter active. A debug flag that swaps
//     KILL_PROCESS for LOG is cheap to add later if we need it;
//     the principle "production never ships with a bypass knob"
//     defers it.

use anyhow::{Context, Result};
use seccompiler::{
    apply_filter, BpfProgram, SeccompAction, SeccompFilter, SeccompRule, TargetArch,
};

/// Build a per-class filter from an allow-list of syscall numbers.
///
/// The builder is intentionally tiny: collect syscalls, fold into a
/// `SeccompFilter`, compile to a `BpfProgram`. Call sites layer
/// the common event-loop allowlist + class-specific extras, then
/// call `apply()` once, just before the event loop enters.
pub struct FilterBuilder {
    allow: Vec<libc::c_long>,
}

impl FilterBuilder {
    pub fn new() -> Self {
        Self { allow: Vec::new() }
    }

    /// Add the syscalls every rust-vmm vhost-user backend needs
    /// to serve a frontend connection. Curated empirically against
    /// `vhost-user-backend 0.22` under strace; kept deliberately
    /// narrow (every extra syscall is attack surface).
    pub fn with_vhost_user_event_loop(mut self) -> Self {
        // Per-class filters always ride on top of this baseline.
        // If any rust-vmm crate starts using a new syscall, the
        // "banned syscall → SIGSYS" path will surface it loudly
        // before we ship — which is the point.
        const BASELINE: &[libc::c_long] = &[
            // I/O
            libc::SYS_read,
            libc::SYS_write,
            libc::SYS_readv,
            libc::SYS_writev,
            libc::SYS_close,
            libc::SYS_lseek,
            // Socket ops for the vhost-user control socket:
            // socket/bind/listen for the listener we create on
            // startup, accept4 for the incoming frontend
            // connection, recvmsg/sendmsg (+ SCM_RIGHTS) for the
            // protocol. unlink/unlinkat clears any stale socket
            // file from a prior crashed run before bind.
            libc::SYS_socket,
            libc::SYS_bind,
            libc::SYS_listen,
            libc::SYS_accept4,
            libc::SYS_recvmsg,
            libc::SYS_sendmsg,
            libc::SYS_getsockopt,
            libc::SYS_setsockopt,
            libc::SYS_unlink,
            libc::SYS_unlinkat,
            // Polling + eventfds (rust-vmm's run loop).
            libc::SYS_epoll_create1,
            libc::SYS_epoll_ctl,
            libc::SYS_epoll_wait,
            libc::SYS_epoll_pwait,
            libc::SYS_epoll_pwait2,
            libc::SYS_eventfd2,
            libc::SYS_ppoll,
            libc::SYS_poll,
            // Guest memory regions — the frontend sends fds via
            // SET_MEM_TABLE that we mmap into our address space.
            libc::SYS_mmap,
            libc::SYS_munmap,
            libc::SYS_mprotect,
            libc::SYS_madvise,
            // Process bookkeeping. futex for std synchronization;
            // getrandom so stacktrace / uuid / hashmap seeding
            // still works under allocation pressure.
            libc::SYS_futex,
            libc::SYS_getrandom,
            libc::SYS_sigaltstack,
            libc::SYS_rt_sigaction,
            libc::SYS_rt_sigprocmask,
            libc::SYS_rt_sigreturn,
            libc::SYS_nanosleep,
            libc::SYS_clock_gettime,
            libc::SYS_clock_nanosleep,
            libc::SYS_gettimeofday,
            libc::SYS_getpid,
            libc::SYS_gettid,
            libc::SYS_sched_yield,
            // glibc's NPTL + pthread thread-creation path: when
            // the daemon's internal request-handler thread spawns,
            // its parent calls `clone3` (435) to create it, and
            // the child's first syscalls include `set_robust_list`
            // (register the robust-futex list), `set_tid_address`
            // (where the kernel writes the exiting TID), `rseq`
            // (restartable sequences init), `sched_getaffinity`
            // (allocator topology), and `prctl` (the daemon's
            // `thread::Builder::name(...)` flows through
            // PR_SET_NAME). Without these in the baseline, the
            // post-accept thread spawn SIGSYSes before the event
            // loop starts. Verified end-to-end by
            // tests/frontend_handshake.rs running the real
            // binary through `Frontend::connect` + `get_features`.
            libc::SYS_clone3,
            libc::SYS_prctl,
            libc::SYS_set_robust_list,
            libc::SYS_set_tid_address,
            libc::SYS_rseq,
            libc::SYS_sched_getaffinity,
            // Orderly shutdown.
            libc::SYS_exit,
            libc::SYS_exit_group,
            // glibc uses these for stdio / allocator bookkeeping
            // from time to time; keep them in the baseline rather
            // than re-discovering them per class.
            libc::SYS_brk,
            libc::SYS_mremap,
            libc::SYS_fstat,
            libc::SYS_fcntl,
            libc::SYS_ioctl, // arg-filtered per class where possible
            libc::SYS_dup,
            libc::SYS_dup3,
            libc::SYS_fallocate,
        ];
        self.allow.extend_from_slice(BASELINE);
        self
    }

    /// Add a class-specific syscall to the allow list.
    ///
    /// Used by net (tap ioctls, recvmsg sockets) and block
    /// (preadv/pwritev, fallocate, fdatasync) backends landing
    /// in subsequent v2 commits. Currently only exercised by
    /// in-file tests; the `allow(dead_code)` silences that while
    /// the callers ship.
    #[allow(dead_code)]
    pub fn allow(mut self, syscall: libc::c_long) -> Self {
        self.allow.push(syscall);
        self
    }

    /// Add several class-specific syscalls at once.
    #[allow(dead_code)]
    pub fn allow_many(mut self, syscalls: &[libc::c_long]) -> Self {
        self.allow.extend_from_slice(syscalls);
        self
    }

    /// Compile the allowlist into a BPF program ready for
    /// `seccompiler::apply_filter()`.
    pub fn compile(self) -> Result<BpfProgram> {
        // Deduplicate first — callers can layer multiple "common"
        // baselines without tripping seccompiler's "duplicate
        // syscall" check.
        let mut allow = self.allow;
        allow.sort_unstable();
        allow.dedup();

        // Map every allowed syscall to an unconditional Allow
        // rule. Args-level filtering (e.g. restricting ioctl by
        // cmd number) is a follow-on refinement; the v2 commit 3
        // scope is "allow-list by syscall, deny everything else".
        let rules: std::collections::BTreeMap<
            libc::c_long,
            Vec<SeccompRule>,
        > = allow
            .into_iter()
            .map(|syscall| (syscall, Vec::new()))
            .collect();

        let filter = SeccompFilter::new(
            rules,
            SeccompAction::KillProcess,
            SeccompAction::Allow,
            ARCH,
        )
        .context("building seccomp filter")?;

        filter
            .try_into()
            .context("compiling seccomp filter to BPF program")
    }

    /// Compile and install the filter on the current thread.
    /// All descendant threads inherit the filter per
    /// SECCOMP_FILTER_FLAG_TSYNC; rust-vmm's VhostUserDaemon
    /// threads spawn after this point and thus inherit.
    pub fn apply(self) -> Result<()> {
        let prog = self.compile()?;
        apply_filter(&prog).context("applying seccomp filter to thread group")
    }
}

impl Default for FilterBuilder {
    fn default() -> Self {
        Self::new()
    }
}

// Target architecture for filter compilation. x86_64 is the only
// tier-1 UML target at time of writing; other arches can extend
// the match when they grow UML support.
#[cfg(target_arch = "x86_64")]
const ARCH: TargetArch = TargetArch::x86_64;
#[cfg(target_arch = "aarch64")]
const ARCH: TargetArch = TargetArch::aarch64;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn baseline_filter_compiles() {
        // Just building + compiling the baseline proves the rule
        // set is well-formed and the target arch is recognized.
        let prog = FilterBuilder::new()
            .with_vhost_user_event_loop()
            .compile()
            .expect("compile baseline filter");
        assert!(
            !prog.is_empty(),
            "compiled BPF program should have instructions"
        );
    }

    #[test]
    fn class_specific_syscalls_additive() {
        // A class layering its own syscalls on top of the baseline
        // produces a strictly larger program.
        let base = FilterBuilder::new()
            .with_vhost_user_event_loop()
            .compile()
            .expect("base");

        let with_extras = FilterBuilder::new()
            .with_vhost_user_event_loop()
            .allow(libc::SYS_preadv)
            .allow(libc::SYS_pwritev)
            .compile()
            .expect("class");

        assert!(
            with_extras.len() >= base.len(),
            "class-specific additions should not shrink the program"
        );
    }

    #[test]
    fn duplicate_syscalls_are_tolerated() {
        // Builders shouldn't panic if a caller layers a syscall
        // already in the baseline (this is a real scenario — a
        // class that's both byte-IO and mmap-heavy may re-list
        // `mmap` for documentation even though it's in the
        // baseline).
        FilterBuilder::new()
            .with_vhost_user_event_loop()
            .allow(libc::SYS_read) // already in baseline
            .compile()
            .expect("duplicate syscall dedup");
    }
}

/// Isolated process-level tests. Each fork()s a child that applies
/// the filter then attempts a banned syscall; parent asserts the
/// child died by SIGSYS (per SECCOMP_RET_KILL_PROCESS).
///
/// Keep these in a separate test module because they manipulate
/// process state in ways that don't compose with other tests
/// sharing the same process.
#[cfg(test)]
mod sigsys_tests {
    use super::*;
    use std::os::unix::process::ExitStatusExt;

    /// Run @body in a fresh child process; return its ExitStatus.
    /// Uses fork() directly so the filter applies only to the
    /// child and cannot leak into the cargo test harness.
    fn fork_run(body: impl FnOnce()) -> std::process::ExitStatus {
        use nix::sys::wait::{waitpid, WaitStatus};
        use nix::unistd::{fork, ForkResult};

        // SAFETY: fork() in a Rust binary with threads is
        // notoriously unsafe. cargo test may have spawned threads
        // before reaching us; we cannot rely on libraries being
        // fork-safe in the child. But we only run async-signal-
        // safe ops (prctl to apply seccomp, then an immediate
        // syscall); we don't touch allocators or locks from the
        // child side. This is the same pattern Firecracker's
        // integration tests use.
        let result = unsafe { fork() }.expect("fork");
        match result {
            ForkResult::Child => {
                body();
                // Unreachable on success paths — seccomp KILL_PROCESS
                // already took the child down. Guard anyway with _exit
                // (not exit, which calls atexit handlers).
                unsafe { libc::_exit(0) };
            }
            ForkResult::Parent { child } => match waitpid(child, None).expect("waitpid") {
                WaitStatus::Exited(_, code) => {
                    std::process::ExitStatus::from_raw(code << 8)
                }
                WaitStatus::Signaled(_, sig, _) => {
                    std::process::ExitStatus::from_raw(sig as i32)
                }
                other => panic!("unexpected wait status: {other:?}"),
            },
        }
    }

    #[test]
    fn banned_syscall_kills_child_with_sigsys() {
        let status = fork_run(|| {
            // Apply baseline filter only; ptrace is NOT in the
            // baseline, so attempting it should kill us with
            // SIGSYS per SECCOMP_RET_KILL_PROCESS.
            FilterBuilder::new()
                .with_vhost_user_event_loop()
                .apply()
                .expect("apply filter");

            // Attempt ptrace. Kernel dispatches to seccomp filter
            // first; our filter returns KILL_PROCESS; we never
            // reach the ptrace code.
            unsafe {
                libc::syscall(libc::SYS_ptrace, 0, 0, 0, 0);
            }
            // If we reach here, filter didn't bite — fail loud.
            eprintln!("seccomp did not block ptrace");
        });

        let signal = status.signal();
        assert_eq!(
            signal,
            Some(libc::SIGSYS),
            "expected SIGSYS, got status={status:?}"
        );
    }

    #[test]
    fn allowed_syscall_succeeds() {
        let status = fork_run(|| {
            FilterBuilder::new()
                .with_vhost_user_event_loop()
                .apply()
                .expect("apply filter");

            // getpid is in the baseline — should return cleanly.
            let pid = unsafe { libc::getpid() };
            assert!(pid > 0);
        });

        // Child should have exited normally (code 0 from _exit).
        assert!(
            status.success(),
            "allowed syscall should not have been killed, got {status:?}"
        );
    }
}
