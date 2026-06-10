// SPDX-License-Identifier: GPL-2.0
//
// Declared UML event schemas.
//
// Each entry names an ECS-shaped event type the spine knows
// how to produce or consume. Schemas are versioned and frozen
// once shipped: add fields additively until you need a `.v2`.
// Consumers read this table via `umlctl schema` so they can
// build filters / predicates against a stable surface, even
// for schemas whose *producer* hasn't landed yet.
//
// `uml.lifecycle.v1` is emitted by umlctl lifecycle verbs.  The dmesg
// parser emits panic, OOM, sanitizer, stall, and lockdep schemas at
// `umlctl stop` time by scanning `kernel.log` for canonical
// BUG/WARNING tokens.

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
        description: "umlctl lifecycle verb fired: create / start / stop / rm. Carries pid + \
             signal + exit_status where applicable.",
    },
    SchemaDecl {
        name: "uml.panic.v1",
        category: "crash",
        severity: "error",
        source: "guest kernel (via dmesg parser)",
        status: "emitted",
        description: "panic() called in the guest. Payload carries the panic message; \
             produced by the dmesg parser at stop time.",
    },
    SchemaDecl {
        name: "uml.oom.v1",
        category: "resource",
        severity: "warning",
        source: "guest kernel (via dmesg parser)",
        status: "emitted",
        description: "OOM killer invoked in the guest. Produced by the dmesg parser \
             at stop time.",
    },
    SchemaDecl {
        name: "uml.sanitizer.kasan.v1",
        category: "sanitizer",
        severity: "error",
        source: "guest kernel (via dmesg parser)",
        status: "emitted",
        description: "KASAN reported a memory-safety bug. Produced by the dmesg parser.",
    },
    SchemaDecl {
        name: "uml.sanitizer.kfence.v1",
        category: "sanitizer",
        severity: "error",
        source: "guest kernel (via dmesg parser)",
        status: "emitted",
        description: "KFENCE reported a bounds/use-after-free hit. Produced by the dmesg parser.",
    },
    SchemaDecl {
        name: "uml.sanitizer.kcsan.v1",
        category: "sanitizer",
        severity: "error",
        source: "guest kernel (via dmesg parser)",
        status: "emitted",
        description: "KCSAN reported a data-race. Produced by the dmesg parser.",
    },
    SchemaDecl {
        name: "uml.sanitizer.kmsan.v1",
        category: "sanitizer",
        severity: "error",
        source: "guest kernel (via dmesg parser)",
        status: "emitted",
        description: "KMSAN reported a use-of-uninitialized-value. Produced by the dmesg parser.",
    },
    SchemaDecl {
        name: "uml.sanitizer.ubsan.v1",
        category: "sanitizer",
        severity: "error",
        source: "guest kernel (via dmesg parser)",
        status: "emitted",
        description: "UBSAN reported undefined behavior. Produced by the dmesg parser.",
    },
    SchemaDecl {
        name: "uml.rcu_stall.v1",
        category: "stall",
        severity: "warning",
        source: "guest kernel (via dmesg parser)",
        status: "emitted",
        description: "RCU stall detected. Produced by the dmesg parser.",
    },
    SchemaDecl {
        name: "uml.lockdep.v1",
        category: "lockdep",
        severity: "warning",
        source: "guest kernel (via dmesg parser)",
        status: "emitted",
        description: "Lockdep reported a recursive / circular / inconsistent lock-state \
             hazard. Produced by the dmesg parser.",
    },
    SchemaDecl {
        name: "uml.watchdog_stall.v1",
        category: "stall",
        severity: "warning",
        source: "guest kernel (via dmesg parser)",
        status: "emitted",
        description: "Soft/hard lockup watchdog fired. Produced by the dmesg parser.",
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
