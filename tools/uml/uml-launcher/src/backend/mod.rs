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

pub mod console;

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
    tracing::info!(
        socket = %args.common.socket.display(),
        tap = args.tap.as_deref().unwrap_or("<unset>"),
        "backend net: not yet implemented (C-10 v2 commit 1 scaffold)"
    );
    eprintln!(
        "uml-launcher backend net: not yet implemented (socket={}, tap={}).",
        args.common.socket.display(),
        args.tap.as_deref().unwrap_or("<unset>")
    );
    Ok(0)
}

fn run_block(args: crate::cli::BackendBlockArgs) -> Result<i32> {
    let image = args
        .image
        .as_ref()
        .map(|p| p.display().to_string())
        .unwrap_or_else(|| "<unset>".to_string());
    tracing::info!(
        socket = %args.common.socket.display(),
        image = %image,
        read_only = args.read_only,
        "backend block: not yet implemented (C-10 v2 commit 1 scaffold)"
    );
    eprintln!(
        "uml-launcher backend block: not yet implemented (socket={}, image={}, read_only={}).",
        args.common.socket.display(),
        image,
        args.read_only
    );
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cli::{BackendBlockArgs, BackendCommonArgs, BackendNetArgs};
    use std::path::PathBuf;

    fn common(path: &str) -> BackendCommonArgs {
        BackendCommonArgs {
            socket: PathBuf::from(path),
        }
    }

    // The Console dispatch path spawns a real VhostUserDaemon in
    // console::run() and would block on accept(); unit tests for
    // the actual backend live in backend/console.rs. End-to-end
    // coverage ships in the selftest.

    #[test]
    fn dispatch_net_returns_zero() {
        let class = BackendClass::Net(BackendNetArgs {
            common: common("/tmp/uml-net.sock"),
            tap: Some("tap0".to_string()),
        });
        assert_eq!(dispatch(class).unwrap(), 0);
    }

    #[test]
    fn dispatch_block_returns_zero() {
        let class = BackendClass::Block(BackendBlockArgs {
            common: common("/tmp/uml-block.sock"),
            image: Some(PathBuf::from("/tmp/rootfs.img")),
            read_only: true,
        });
        assert_eq!(dispatch(class).unwrap(), 0);
    }
}
