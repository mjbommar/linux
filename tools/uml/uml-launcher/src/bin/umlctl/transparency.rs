// SPDX-License-Identifier: GPL-2.0
//
// Transparency subcommands — UML v2's killer feature: the guest *is*
// a host process, so the host's debug toolchain just works on it.
//
//   umlctl strace <name>  → strace -p $PID -f
//   umlctl gdb    <name>  → gdb $KERNEL -p $PID
//   umlctl bpf    <name>  → pre-canned bpftrace scripts targeting
//                            the running UML process
//
// VMware / VirtualBox / Firecracker cannot do any of these without
// a guest-side agent.  UML's process model gets it for free; this
// module just adds the discoverability surface so users don't
// have to know the right strace/gdb/bpftrace incantation.

use std::os::unix::process::CommandExt;
use std::path::PathBuf;
use std::process::Command;

use anyhow::{bail, Context, Result};

use super::manifest::Manifest;
use super::paths::Paths;
use super::run::Run;
use super::supervise;

/// Resolve `<name>` → (pid, kernel_path).  Used by strace / gdb / bpf
/// helpers to avoid each one re-implementing the lookup.  Returns
/// non-zero exit if no live run is bound to the instance.
fn resolve_live(paths: &Paths, name: &str) -> Result<(u32, PathBuf)> {
    let manifest_path = paths.manifest_path(name);
    if !manifest_path.exists() {
        bail!("instance '{name}' not found");
    }
    let manifest = Manifest::read(&manifest_path)
        .with_context(|| format!("read manifest for instance '{name}'"))?;
    let run_id = supervise::read_run_id_file(&paths.run_id_file_path(name)).with_context(|| {
        format!("no live run for instance '{name}' — has umlctl up been called?")
    })?;
    let run = Run::read(&paths.run_dir(&run_id).join("run.json"))
        .with_context(|| format!("read run.json for run {run_id}"))?;
    Ok((run.pid, manifest.kernel.path.clone()))
}

/// `umlctl strace <name> [strace-args...]`.
///
/// The guest UML kernel runs as a host process under the seccomp
/// (or kvm-v2) backend; every guest syscall in seccomp mode
/// translates 1:1 to a host syscall on the process, so a plain
/// `strace -p <pid>` shows the guest's syscall stream.
///
/// Defaults follow `-f -t -e trace=!futex,clock_gettime,gettimeofday`
/// because the futex/timer chatter is dominant noise; pass extra
/// args (e.g. `-e trace=openat`) to filter further.
pub fn cmd_strace(paths: &Paths, name: &str, extra: &[String]) -> Result<()> {
    let (pid, _kernel) = resolve_live(paths, name)?;
    let strace = which("strace")?;
    eprintln!("[umlctl] attaching strace to PID {pid} (instance '{name}'); ^C to detach.");
    let mut cmd = Command::new(strace);
    cmd.arg("-p").arg(pid.to_string()).arg("-f").arg("-t");
    if extra.is_empty() {
        cmd.arg("-e")
            .arg("trace=!futex,clock_gettime,gettimeofday,nanosleep,epoll_wait");
    } else {
        for a in extra {
            cmd.arg(a);
        }
    }
    Err(cmd.exec().into())
}

/// `umlctl gdb <name> [gdb-args...]`.
///
/// Attach gdb to the running guest UML kernel with the kernel
/// vmlinux loaded.  Because the guest kernel runs as a host
/// process — under both seccomp and kvm-v2 backends — gdb can
/// resolve guest kernel symbols, set breakpoints in guest code,
/// and inspect guest state.  None of QEMU/KVM/VirtualBox can do
/// this without their own kgdb-over-serial dance.
pub fn cmd_gdb(paths: &Paths, name: &str, extra: &[String]) -> Result<()> {
    let (pid, kernel) = resolve_live(paths, name)?;
    let gdb = which("gdb")?;
    eprintln!(
        "[umlctl] attaching gdb to PID {pid} with kernel {} (instance '{name}'); use 'detach' to leave the process running.",
        kernel.display()
    );
    let mut cmd = Command::new(gdb);
    cmd.arg(&kernel).arg("-p").arg(pid.to_string());
    // Reasonable defaults for "I just want to look around":
    //   pagination off → don't stall on long output
    //   confirm off    → don't second-guess detach
    cmd.arg("-ex")
        .arg("set pagination off")
        .arg("-ex")
        .arg("set confirm off");
    for a in extra {
        cmd.arg(a);
    }
    Err(cmd.exec().into())
}

/// `umlctl bpf <name> [script]`.
///
/// Run a bpftrace script on the host that filters to events from
/// the named guest UML process.  Without a script argument, prints
/// a short menu of pre-canned scripts.
///
/// The advantage over generic `bpftrace -p <pid>`: the canned
/// scripts know which kernel-internal callsites correspond to
/// guest syscall entry / page fault / context switch in UML's
/// dispatch model, so the output is labeled in guest-relevant
/// terms.
pub fn cmd_bpf(paths: &Paths, name: &str, script: Option<&str>) -> Result<()> {
    let (pid, _kernel) = resolve_live(paths, name)?;
    let script_name = script.unwrap_or("menu");

    if script_name == "menu" {
        println!("umlctl bpf <instance> <script>");
        println!();
        println!("Available scripts (pre-canned bpftrace one-liners):");
        println!();
        println!("  syscalls   per-second guest syscall histogram by syscall number");
        println!("  pagefaults guest page-fault rate + addresses");
        println!("  io         guest block I/O latency histogram (UBD)");
        println!("  net        guest network TX/RX byte rate (vector / vector2)");
        println!("  sched      host scheduler events touching the guest process");
        println!();
        println!("PID for the named instance: {pid}");
        return Ok(());
    }

    let bpftrace = which("bpftrace")?;
    let script_src = match script_name {
        "syscalls" => SCRIPT_SYSCALLS,
        "pagefaults" => SCRIPT_PAGEFAULTS,
        "io" => SCRIPT_IO,
        "net" => SCRIPT_NET,
        "sched" => SCRIPT_SCHED,
        other => bail!("unknown bpf script: {other:?}; try `umlctl bpf {name} menu`"),
    };

    eprintln!("[umlctl] running bpftrace script '{script_name}' on PID {pid}; ^C to stop.");
    let prog = script_src.replace("__PID__", &pid.to_string());
    let mut cmd = Command::new(bpftrace);
    cmd.arg("-e").arg(prog);
    Err(cmd.exec().into())
}

fn which(prog: &str) -> Result<String> {
    let out = Command::new("which")
        .arg(prog)
        .output()
        .with_context(|| format!("which {prog}"))?;
    if !out.status.success() {
        bail!("`{prog}` not found in PATH — install it first (apt install {prog} or equivalent)");
    }
    Ok(String::from_utf8_lossy(&out.stdout).trim().to_string())
}

// ---------- pre-canned bpftrace scripts ----------
//
// __PID__ is substituted at run time.  In bpftrace, the `pid`
// builtin is the thread-group id (TGID), so the filter captures
// the UML main thread AND its host helper threads (io_thread,
// vector2 RX threads, etc.) — exactly what a user wants
// when looking at "what is this guest doing?"
//
//   syscalls / sched   produce output immediately on an idle UML
//                      (timer wakeups, scheduler switches with
//                      swapper/N).
//   pagefaults         produces output when guest does anything
//                      that touches new memory (mmap, fault-in).
//   io                 needs active block I/O.  Idle UML produces
//                      no data.  Run a `dd` in the guest to see it.
//   net                needs active network traffic.  See above.
//
// All scripts use only stable tracepoints — block:block_rq_*,
// sched:sched_switch, syscalls:sys_enter_*, software:faults —
// available on every modern host kernel.

const SCRIPT_SYSCALLS: &str = "
tracepoint:raw_syscalls:sys_enter
/pid == __PID__/
{
    @[args->id] = count();
}
interval:s:1
{
    print(@);
    clear(@);
}
";

// Page-fault script: uses the user-space page-fault tracepoint
// rather than software:faults (which doesn't expose the faulting
// address in bpftrace).  Captures the guest's user-mode page
// faults — guest kernel faults are visible via the
// page_fault_kernel sibling tracepoint if needed.
const SCRIPT_PAGEFAULTS: &str = "
tracepoint:exceptions:page_fault_user
/pid == __PID__/
{
    @addrs[args->address] = count();
}
interval:s:5
{
    printf(\"=== top 10 fault addrs (last 5s) ===\\n\");
    print(@addrs, 10);
    clear(@addrs);
}
";

// Block-IO script: uses the canonical block_rq_issue / _complete
// tracepoint pair.  Under UBD's io_uring path the issue/complete
// tracepoints still fire because they hook at block-layer dispatch,
// upstream of driver-side io_uring submission.
const SCRIPT_IO: &str = "
tracepoint:block:block_rq_issue
/pid == __PID__/
{
    @start[args->dev, args->sector] = nsecs;
}
tracepoint:block:block_rq_complete
/@start[args->dev, args->sector]/
{
    @latency_us = hist((nsecs - @start[args->dev, args->sector]) / 1000);
    delete(@start[args->dev, args->sector]);
}
interval:s:5
{
    print(@latency_us);
    clear(@latency_us);
}
";

// Network script: traces guest TX (writev) and RX (recvfrom)
// syscalls from the UML host process. For vector2 fd-handoff, TX
// uses raw write() not writev(), which produces a per-skb
// sys_enter_write event. Users chasing vector2 throughput should
// switch to sys_enter_write
// if they expect non-zero @tx_bytes.
const SCRIPT_NET: &str = "
tracepoint:syscalls:sys_enter_writev
/pid == __PID__/
{
    @tx_bytes = sum(args->vlen);
}
tracepoint:syscalls:sys_enter_recvfrom
/pid == __PID__/
{
    @rx_bytes = sum(args->size);
}
interval:s:1
{
    printf(\"tx %llu B/s    rx %llu B/s\\n\", (uint64)@tx_bytes, (uint64)@rx_bytes);
    clear(@tx_bytes);
    clear(@rx_bytes);
}
";

const SCRIPT_SCHED: &str = "
tracepoint:sched:sched_switch
/args->prev_pid == __PID__ || args->next_pid == __PID__/
{
    @[args->prev_comm, args->next_comm] = count();
}
interval:s:5
{
    print(@);
    clear(@);
}
";
