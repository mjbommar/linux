// SPDX-License-Identifier: GPL-2.0
//
// Per-device vhost-user backend dispatcher (C-10 v2 commit 1 —
// scaffolding only).
//
// `uml-launcher backend <class>` dispatches to the matching per-
// class handler. Each handler currently reports its args and
// returns 0 without doing any vhost-user work; the real protocol
// implementation, per-class seccomp filters, and LSM transitions
// land in the follow-on commits (see decisions-log D52 for the
// bisectable plan).
//
// This module exists so the subcommand surface and per-class arg
// plumbing are in place, testable, and bisectable before any
// runtime dependency on rust-vmm gets added. Building on a clean
// subcommand scaffold keeps every follow-on commit small enough
// to review in isolation.

use anyhow::Result;

use crate::cli::BackendClass;

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
    tracing::info!(
        socket = %args.common.socket.display(),
        "backend console: not yet implemented (C-10 v2 commit 1 scaffold)"
    );
    eprintln!(
        "uml-launcher backend console: not yet implemented (socket={}).",
        args.common.socket.display()
    );
    Ok(0)
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
    use crate::cli::{BackendBlockArgs, BackendCommonArgs, BackendConsoleArgs, BackendNetArgs};
    use std::path::PathBuf;

    fn common(path: &str) -> BackendCommonArgs {
        BackendCommonArgs {
            socket: PathBuf::from(path),
        }
    }

    #[test]
    fn dispatch_console_returns_zero() {
        let class = BackendClass::Console(BackendConsoleArgs {
            common: common("/tmp/uml-console.sock"),
        });
        assert_eq!(dispatch(class).unwrap(), 0);
    }

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
