// SPDX-License-Identifier: GPL-2.0
//
// Integration test: parse-check the in-tree AppArmor profile
// using the host's `apparmor_parser` if one is installed.
// Skipped cleanly on hosts without AppArmor so CI matrices
// without that LSM don't regress.
//
// Workstream C-10 v2: the profile itself lives at
// `tools/uml/uml-launcher/apparmor/uml-launcher`; this test
// guards it against syntax drift as the file evolves.

use std::path::PathBuf;
use std::process::Command;

fn profile_path() -> PathBuf {
    // CARGO_MANIFEST_DIR points at `tools/uml/uml-launcher/`.
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("apparmor/uml-launcher")
}

/// Return true iff `apparmor_parser` is on PATH. AppArmor is a
/// Linux LSM; the parser ships with the userspace tools package
/// (Debian/Ubuntu: `apparmor`, Fedora: `apparmor-parser`). Tests
/// that hard-depend on it would fail on most non-Debian CI;
/// gate instead.
fn apparmor_parser_available() -> bool {
    Command::new("apparmor_parser")
        .arg("--version")
        .output()
        .is_ok_and(|o| o.status.success())
}

#[test]
fn profile_preprocesses_cleanly() {
    if !apparmor_parser_available() {
        eprintln!(
            "apparmor_parser not found on PATH; skipping profile preprocess test"
        );
        return;
    }

    let path = profile_path();
    assert!(path.exists(), "expected profile at {}", path.display());

    let output = Command::new("apparmor_parser")
        .arg("--preprocess")
        .arg(&path)
        .output()
        .expect("run apparmor_parser --preprocess");

    assert!(
        output.status.success(),
        "apparmor_parser --preprocess failed for {}\nstdout:\n{}\nstderr:\n{}",
        path.display(),
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

// A `apparmor_parser -Q` (compile without load) test would be
// the stronger check, but `-Q` exits non-zero when
// /var/cache/apparmor isn't writable by the test user — a
// cosmetic cache failure distinct from the correctness gate we
// want to assert. `--preprocess` exercises the same syntax
// validation without touching the cache; rely on that as the
// automated check, and leave the full compile to installation
// time.

#[test]
fn profile_names_expected_subprofiles() {
    // Sanity check independent of the parser: the profile
    // file must still declare the three backend sub-profiles
    // the launcher transitions into. Catches accidental
    // renames / removals that a pure-syntax parser would
    // accept.
    let path = profile_path();
    let text = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("read {}: {e}", path.display()));

    for sub in ["backend_console", "backend_net", "backend_block"] {
        let needle = format!("profile {sub} {{");
        assert!(
            text.contains(&needle),
            "profile file should declare `{sub}` sub-profile"
        );
        let change_profile = format!("change_profile -> uml-launcher//{sub},");
        assert!(
            text.contains(&change_profile),
            "parent profile should allow change_profile into {sub}"
        );
    }
}
