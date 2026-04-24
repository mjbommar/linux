// SPDX-License-Identifier: GPL-2.0
//
// Declared spine event schemas (memo 13 Phase O1.3).
//
// Each entry names an ECS-shaped event type the spine knows
// how to produce or consume. Schemas are versioned and frozen
// once shipped: add fields additively until you need a `.v2`.
// Consumers read this table via `umlctl schema` so they can
// build filters / predicates against a stable surface, even
// for schemas whose *producer* hasn't landed yet.
//
// For O1.3 only `uml.lifecycle.v1` actually gets emitted —
// panic/OOM land when the dmesg parser (O1.2-adjacent) or the
// control socket (O-later) arrives. Declaring them early keeps
// the downstream `umlctl assert --no-panic`-style predicates
// from being accidentally reinvented.

use serde::Serialize;

#[derive(Serialize, Debug, Clone, Copy)]
pub struct SchemaDecl {
    pub name: &'static str,
    pub category: &'static str,
    pub severity: &'static str,
    pub source: &'static str,
    pub status: &'static str,
    pub description: &'static str,
}

pub const REGISTRY: &[SchemaDecl] = &[
    SchemaDecl {
        name: "uml.lifecycle.v1",
        category: "lifecycle",
        severity: "info",
        source: "umlctl",
        status: "emitted",
        description:
            "umlctl lifecycle verb fired: create / start / stop / rm. Carries pid + \
             signal + exit_status where applicable.",
    },
    SchemaDecl {
        name: "uml.panic.v1",
        category: "crash",
        severity: "error",
        source: "guest kernel (via dmesg parser or control socket)",
        status: "declared",
        description:
            "panic() called in the guest. Payload carries the panic message and \
             stack if recoverable. Not yet produced — dmesg parser lands in a \
             later O1 sub-lift.",
    },
    SchemaDecl {
        name: "uml.oom.v1",
        category: "resource",
        severity: "warning",
        source: "guest kernel (via dmesg parser)",
        status: "declared",
        description:
            "OOM killer invoked in the guest. Payload carries the victim comm + pid + \
             oom_score_adj. Not yet produced — dmesg parser lands in a later O1 \
             sub-lift.",
    },
];

pub fn print_human() {
    println!(
        "{:<22} {:<10} {:<9} {:<10} {}",
        "SCHEMA", "CATEGORY", "SEVERITY", "STATUS", "SOURCE"
    );
    for s in REGISTRY {
        println!(
            "{:<22} {:<10} {:<9} {:<10} {}",
            s.name, s.category, s.severity, s.status, s.source
        );
    }
}

pub fn print_json() -> anyhow::Result<()> {
    use std::io::Write;
    let stdout = std::io::stdout();
    let mut lock = stdout.lock();
    for s in REGISTRY {
        writeln!(lock, "{}", serde_json::to_string(s)?)?;
    }
    Ok(())
}
