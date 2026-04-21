// SPDX-License-Identifier: GPL-2.0
//
// Signal forwarding: dedicated thread that catches SIGINT /
// SIGTERM / SIGHUP and forwards them to the UML child via
// shared_child.
//
// shared_child is the key piece — it lets the main thread
// (doing .wait()) and the signal thread (doing .send_signal())
// both hold the Child handle safely. std::process::Child alone
// cannot be split across threads like that.

use std::sync::Arc;
use std::thread;

use anyhow::{Context, Result};
use shared_child::SharedChild;
use signal_hook::consts::{SIGHUP, SIGINT, SIGQUIT, SIGTERM};
use signal_hook::iterator::Signals;

/// Handle returned by install_forwarder. Dropping it detaches
/// the thread; calling close() first lets the signal thread
/// exit cleanly.
pub struct Forwarder {
    handle: signal_hook::iterator::Handle,
    join: Option<thread::JoinHandle<()>>,
}

impl Forwarder {
    /// Ask the signal thread to stop watching and join it.
    /// Called after the child has already exited (on the happy
    /// path); we no longer need to forward anything.
    pub fn close(mut self) {
        self.handle.close();
        if let Some(j) = self.join.take() {
            let _ = j.join();
        }
    }
}

impl Drop for Forwarder {
    fn drop(&mut self) {
        // Defensive: if the caller didn't call close() (e.g.
        // early-return path), still close the iterator so the
        // thread unblocks. Don't join in Drop — blocking in Drop
        // is a footgun.
        self.handle.close();
    }
}

/// Spawn a thread that watches SIGINT/SIGTERM/SIGHUP/SIGQUIT and
/// forwards each to the UML child. The child's waitpid is handled
/// by the main thread; we don't trap SIGCHLD here.
pub fn install_forwarder(child: Arc<SharedChild>) -> Result<Forwarder> {
    let signals = Signals::new([SIGINT, SIGTERM, SIGHUP, SIGQUIT])
        .context("registering SIGINT/SIGTERM/SIGHUP/SIGQUIT with signal-hook")?;
    let handle = signals.handle();

    let join = thread::Builder::new()
        .name("uml-signal-fwd".to_string())
        .spawn(move || forward_loop(signals, child))
        .context("spawning signal-forwarder thread")?;

    Ok(Forwarder {
        handle,
        join: Some(join),
    })
}

fn forward_loop(mut signals: Signals, child: Arc<SharedChild>) {
    for sig in signals.forever() {
        let name = match sig {
            SIGINT => "SIGINT",
            SIGTERM => "SIGTERM",
            SIGHUP => "SIGHUP",
            SIGQUIT => "SIGQUIT",
            _ => "unknown",
        };
        tracing::info!(signal = name, "forwarding signal to UML child");

        // send_signal via libc::kill on the child pid. We don't
        // use Child::kill() because it's SIGKILL-only — we want
        // the original signal to propagate so the guest kernel
        // can do a graceful shutdown if its init handles it.
        //
        // Safety: child pid is a valid pid for the lifetime of
        // SharedChild; forward_loop exits when signals.handle()
        // is closed by the parent after .wait() returns.
        let pid = child.id() as libc::pid_t;
        let rc = unsafe { libc::kill(pid, sig) };
        if rc != 0 {
            // ESRCH means the child already exited; that's the
            // expected shutdown race, not an error worth a fuss.
            let err = std::io::Error::last_os_error();
            if err.raw_os_error() != Some(libc::ESRCH) {
                tracing::warn!(signal = name, error = ?err, "kill(child) failed");
            }
        }
    }
}
