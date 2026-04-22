// SPDX-License-Identifier: GPL-2.0
//
// Per-device vhost-user backend dispatcher (C-10 v2).
//
// `uml-launcher backend <class>` dispatches to the matching per-
// class handler. Each class lives in its own sub-module and pulls
// in the rust-vmm stack (vhost, vhost-user-backend, vm-memory,
// virtio-queue) through a single-binary multi-call shape — see
// decisions-log D52 for the rationale and commit plan.

use anyhow::Result;

use crate::cli::BackendClass;

pub mod block;
pub mod console;
pub mod net;
pub mod seccomp;

/// Entry point for `uml-launcher backend <class>`.
///
/// Returns the exit code the launcher should propagate to its
/// parent. 0 on successful shutdown (including "backend ran to
/// graceful EOF"), non-zero on unrecoverable setup error.
pub fn dispatch(class: BackendClass) -> Result<i32> {
    match class {
        BackendClass::Console(args) => run_console(args),
        BackendClass::Net(args) => run_net(args),
        BackendClass::Block(args) => run_block(args),
    }
}

fn run_console(args: crate::cli::BackendConsoleArgs) -> Result<i32> {
    console::run(args)
}

fn run_net(args: crate::cli::BackendNetArgs) -> Result<i32> {
    net::run(args)
}

fn run_block(args: crate::cli::BackendBlockArgs) -> Result<i32> {
    block::run(args)
}

// Every backend class now runs a real VhostUserDaemon::serve()
// that would block on accept() if exercised from `cargo test`.
// Per-class unit tests live in backend/{console,net,block}.rs
// and exercise the protocol surface + data path in isolation.
// The dispatch function itself is a trivial three-arm match;
// `cargo build` alone verifies the arms compile correctly.
