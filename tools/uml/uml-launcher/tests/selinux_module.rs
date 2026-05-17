// SPDX-License-Identifier: GPL-2.0
//
// Integration test: compile-check the in-tree SELinux
// reference policy using `make -f /usr/share/selinux/devel/
// Makefile` if selinux-policy-dev is installed. Skipped
// cleanly on hosts without the SELinux devel toolchain so CI
// matrices without it don't regress.
//
// Workstream C-10 v2 commit 7: the module itself lives at
// `tools/uml/uml-launcher/selinux/{uml_launcher.te,.fc,.if}`;
// this test guards it against syntax + reference drift as
// the files evolve.

use std::path::PathBuf;
use std::process::Command;

fn selinux_dir() -> PathBuf {
    // CARGO_MANIFEST_DIR points at `tools/uml/uml-launcher/`.
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("selinux")
}

/// Return true iff the refpolicy devel Makefile is present.
/// On Debian/Ubuntu that means `selinux-policy-dev` is
/// installed; on Fedora/RHEL it's `selinux-policy-devel`.
fn refpolicy_devel_available() -> bool {
    std::path::Path::new("/usr/share/selinux/devel/Makefile").exists()
}

#[test]
fn module_compiles_cleanly() {
    if !refpolicy_devel_available() {
        eprintln!("selinux-policy-dev not installed; skipping SELinux module compile test");
        return;
    }

    let dir = selinux_dir();
    for f in ["uml_launcher.te", "uml_launcher.fc", "uml_launcher.if"] {
        let p = dir.join(f);
        assert!(p.exists(), "expected {}", p.display());
    }

    // `make -f .../Makefile uml_launcher.pp` produces the
    // compiled module. Keep the test self-contained by
    // building in the source dir (the Makefile respects cwd),
    // then clean up the generated files.
    let output = Command::new("make")
        .current_dir(&dir)
        .args(["-f", "/usr/share/selinux/devel/Makefile", "uml_launcher.pp"])
        .output()
        .expect("run make");

    let out = String::from_utf8_lossy(&output.stdout);
    let err = String::from_utf8_lossy(&output.stderr);

    assert!(
        output.status.success(),
        "make uml_launcher.pp failed\nstdout:\n{out}\nstderr:\n{err}"
    );

    let pp = dir.join("uml_launcher.pp");
    assert!(
        pp.exists(),
        "uml_launcher.pp not produced at {}",
        pp.display()
    );
    let size = std::fs::metadata(&pp).map(|m| m.len()).unwrap_or(0);
    assert!(
        size > 1024,
        "uml_launcher.pp suspiciously small: {size} bytes"
    );

    // Clean up — the .pp + tmp/ are in the dir's .gitignore
    // but leaving them behind between test runs bloats the
    // working tree.
    let _ = std::fs::remove_file(&pp);
    let _ = std::fs::remove_dir_all(dir.join("tmp"));
}

#[test]
fn policy_declares_expected_types() {
    // Sanity check independent of the refpolicy toolchain:
    // the .te must still declare `uml_launcher_t` and
    // `uml_launcher_exec_t`; the .fc must still label the
    // upstream install paths. Catches an accidental rename
    // or file-paste that the compile test above wouldn't see
    // (a syntax-clean but semantically-wrong module).
    let dir = selinux_dir();

    let te = std::fs::read_to_string(dir.join("uml_launcher.te"))
        .unwrap_or_else(|e| panic!("read uml_launcher.te: {e}"));
    for needle in ["type uml_launcher_t", "type uml_launcher_exec_t"] {
        assert!(te.contains(needle), ".te should contain '{needle}'");
    }

    let fc = std::fs::read_to_string(dir.join("uml_launcher.fc"))
        .unwrap_or_else(|e| panic!("read uml_launcher.fc: {e}"));
    for path in ["/usr/bin/uml-launcher", "/usr/local/bin/uml-launcher"] {
        assert!(fc.contains(path), ".fc should label {path}");
    }

    let interface = std::fs::read_to_string(dir.join("uml_launcher.if"))
        .unwrap_or_else(|e| panic!("read uml_launcher.if: {e}"));
    for iface in ["uml_launcher_domtrans", "uml_launcher_run"] {
        assert!(interface.contains(iface), ".if should expose {iface}");
    }
}
