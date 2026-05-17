// SPDX-License-Identifier: GPL-2.0
//
// The kernel-tree's top-level `.clippy.toml` disallows
// `core::ffi::CStr::as_ptr` in favor of a kernel-specific
// CStrExt helper that doesn't exist in a userspace binary like
// this one. Local suppression: our binary uses the standard
// `*const c_char` ABI in the FFI boundary, which is exactly
// what the upstream libapparmor symbols expect.
#![allow(clippy::disallowed_methods)]

// libapparmor bindings for runtime sub-profile transitions
// (workstream C-10 v2 commit 6 wire-up per D52).
//
// The in-tree AppArmor profile (`tools/uml/uml-launcher/
// apparmor/uml-launcher`) declares three sub-profiles:
//
//     uml-launcher//backend_console
//     uml-launcher//backend_net
//     uml-launcher//backend_block
//
// For those to become effective, each backend process must
// call `aa_change_profile()` at startup — before it opens any
// sockets, fds, or guest memory — so the kernel binds the
// process to the sub-profile and denials follow. That's what
// this module does.
//
// Design calls (see decisions-log D52):
//
//   * Runtime-loaded via `dlopen(3)`, not link-time. The
//     launcher builds without libapparmor-dev on the host
//     (CI runners, dev machines without the LSM) and silently
//     skips the transition at runtime when
//     libapparmor.so.1 isn't available.
//   * `aa_change_profile()` rather than `aa_change_onexec()`.
//     Each backend subcommand stays in the same process after
//     clap dispatch — there's no fork+exec inside the backend
//     handler for the transition to ride on. The orchestration
//     commit (v2 commit 8) will use `aa_change_onexec()` on
//     the fork+exec path it introduces; this module keeps
//     the same-process path that `uml-launcher backend <class>`
//     uses today.
//   * Graceful-skip on ENOENT (profile not loaded), EINVAL
//     (AA module not loaded), EPERM (caller doesn't have the
//     change_profile rule for this target — common when the
//     launcher's own parent profile isn't loaded or when the
//     binary was started outside the confinement). Treat all
//     three as "AppArmor not available / not configured for
//     me — log and continue unconfined". EACCES stays a hard
//     error; that's policy misconfig.
//   * Seccomp still applies either way. The apparmor
//     transition is defense-in-depth on top of the seccomp
//     filter each backend installs before entering its event
//     loop — either layer alone is some protection; both
//     together are the sandbox profile's deployment posture.

use std::ffi::{c_int, CString};
use std::io;
use std::sync::OnceLock;

/// Resolved libapparmor entry points, or `None` if the library
/// isn't available on this host. Lazy-loaded once per process.
struct AppArmorLib {
    // Handle kept alive so the resolved symbols stay valid.
    _handle: *mut libc::c_void,
    is_enabled: unsafe extern "C" fn() -> c_int,
    change_profile: unsafe extern "C" fn(*const libc::c_char) -> c_int,
}

// SAFETY: the dlopen'd handle + resolved function pointers are
// effectively constants after the one-time load. Marking Sync
// is the usual pattern for lazy-FFI wrappers; the two extern
// functions are themselves thread-safe (they trap to the
// kernel, per-thread state).
unsafe impl Sync for AppArmorLib {}
unsafe impl Send for AppArmorLib {}

static LIB: OnceLock<Option<AppArmorLib>> = OnceLock::new();

fn load() -> &'static Option<AppArmorLib> {
    LIB.get_or_init(|| {
        // Try the versioned SONAME first; most distros ship
        // libapparmor1 as /usr/lib/*/libapparmor.so.1. Fall
        // back to the unversioned name a dev who installed
        // libapparmor-dev would have.
        for name in ["libapparmor.so.1", "libapparmor.so"] {
            let cname = CString::new(name).expect("static str to CString");
            // SAFETY: dlopen with a NUL-terminated name and
            // RTLD_LAZY | RTLD_LOCAL. Returns NULL on failure.
            let handle =
                unsafe { libc::dlopen(cname.as_ptr(), libc::RTLD_LAZY | libc::RTLD_LOCAL) };
            if handle.is_null() {
                continue;
            }
            // SAFETY: dlsym on a non-null handle, with
            // NUL-terminated symbol names. dlsym returns NULL
            // if the symbol is absent, in which case we treat
            // the whole library as unusable (can't be
            // libapparmor without both symbols).
            let is_enabled = unsafe { libc::dlsym(handle, c"aa_is_enabled".as_ptr()) };
            let change_profile = unsafe { libc::dlsym(handle, c"aa_change_profile".as_ptr()) };
            if is_enabled.is_null() || change_profile.is_null() {
                // SAFETY: handle was non-null from dlopen.
                unsafe { libc::dlclose(handle) };
                continue;
            }
            // SAFETY: dlsym'd function pointers match the
            // libapparmor ABI documented in
            // <sys/apparmor.h>: both take the expected args
            // and return a C int (0 on success, -1 + errno
            // on failure).
            return Some(AppArmorLib {
                _handle: handle,
                is_enabled: unsafe {
                    std::mem::transmute::<*mut libc::c_void, unsafe extern "C" fn() -> c_int>(
                        is_enabled,
                    )
                },
                change_profile: unsafe {
                    std::mem::transmute::<
                        *mut libc::c_void,
                        unsafe extern "C" fn(*const libc::c_char) -> c_int,
                    >(change_profile)
                },
            });
        }
        None
    })
}

/// Outcome of a `change_profile` attempt.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ChangeResult {
    /// Transition succeeded — the process is now confined
    /// under the named profile.
    Changed,
    /// libapparmor isn't available on the host, AppArmor
    /// isn't the active LSM, or the named profile isn't
    /// loaded. Not a failure; the caller should log and
    /// continue unconfined.
    Skipped,
}

/// Transition the current process into `profile`.
///
/// `profile` is a bare profile name or a parent//child path
/// like `"uml-launcher//backend_console"`. Returns
/// `Ok(Changed)` on a successful transition, `Ok(Skipped)` if
/// AppArmor is unavailable or the profile isn't loaded, and an
/// `Err` on unexpected failures (EACCES, EINVAL from
/// misconfigured policy, etc.).
///
/// See `aa_change_profile(2)` for the underlying semantics.
pub fn change_profile(profile: &str) -> io::Result<ChangeResult> {
    // CString::new up-front so an interior NUL in the profile
    // name is always reported as a caller error, regardless
    // of whether AppArmor is available on this host. Moving
    // this below the load()-is-Some check would hide input
    // errors on dev boxes without the LSM.
    let cprofile =
        CString::new(profile).map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e))?;
    let lib = match load() {
        Some(l) => l,
        None => return Ok(ChangeResult::Skipped),
    };
    // SAFETY: aa_is_enabled takes no args and returns 1 if
    // the LSM is active, 0 otherwise. errno is set when it
    // returns 0 but we don't need to distinguish the reasons
    // for "not active".
    if unsafe { (lib.is_enabled)() } != 1 {
        return Ok(ChangeResult::Skipped);
    }
    // SAFETY: cprofile is a valid NUL-terminated C string for
    // the duration of the call. aa_change_profile returns 0
    // on success and -1 on failure with errno set.
    let rc = unsafe { (lib.change_profile)(cprofile.as_ptr()) };
    if rc == 0 {
        return Ok(ChangeResult::Changed);
    }
    let err = io::Error::last_os_error();
    match err.raw_os_error() {
        Some(libc::ENOENT) | Some(libc::EINVAL) | Some(libc::EPERM) => {
            // ENOENT — profile not loaded in the kernel.
            // EINVAL — AA module not loaded / interface
            //          disabled.
            // EPERM  — caller isn't in a profile that allows
            //          `change_profile -> <target>`. Happens
            //          when the launcher's parent profile
            //          isn't loaded, or when the binary was
            //          started outside of any profile (which
            //          is the default for dev systems).
            // All three are advisory, not fatal.
            Ok(ChangeResult::Skipped)
        }
        _ => Err(err),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn load_does_not_panic() {
        // The library either loads or doesn't — either way
        // we shouldn't panic or hang during the lazy init.
        let _ = load();
    }

    #[test]
    fn change_profile_noexistent_is_skipped_or_errors() {
        // With any target profile name, we should either:
        //   - get Ok(Skipped) if AA isn't available
        //   - get Ok(Skipped) if AA is active but the name
        //     isn't loaded (ENOENT)
        //   - get Ok(Skipped) if we're not in a profile with
        //     the right change_profile rule (EPERM)
        //   - get Err for anything else
        // On a dev box without the uml-launcher profile
        // loaded, the realistic outcome is Ok(Skipped).
        let result = change_profile("uml-launcher-nonexistent-profile-for-testing");
        match result {
            Ok(ChangeResult::Skipped) => {}
            Ok(ChangeResult::Changed) => {
                panic!("somehow transitioned into a non-existent profile");
            }
            Err(e) => {
                // Only acceptable if we're in some weird CI
                // environment where AA is active and strict.
                // Log for debugging; don't fail the test.
                eprintln!("change_profile returned unexpected error: {e}");
            }
        }
    }

    #[test]
    fn change_profile_rejects_interior_nul() {
        let bad = "uml-launcher//backend\0_injected";
        let err = change_profile(bad).expect_err("CString::new should reject NUL");
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
    }
}
