# C-01 profile matrix — measured build + boot

**Captured:** 2026-04-18
**Host:** 4-CPU, 61 GiB RAM, Ubuntu 26.04, gcc 15.2
**Method:** sequential `make ARCH=um O=/tmp/uml-profile-<p> uml/<p>` +
`make ARCH=um -j4`; each profile starts from an empty `O=` tree
(cold ccache). Boot verified with `init=/bin/true`. KUnit oks
counts the `    ok N` lines from the backend-contract KUnit suite
(0 means KUnit support is not enabled in that profile's
Kconfig — expected for most profiles; only the DYNAMIC configs
built by the workstream A dev matrix enable
`CONFIG_UM_BACKEND_CONTRACT_TEST=y`).

| Profile | Configure | Build (cold) | vmlinux (bytes) | Boots | Feature-selftest |
|---|---:|---:|---:|---|---|
| prod-fast       | 7s  | 329s | 87,519,864  | ok | PASS (4 features) |
| prod-with-hooks | 7s  | 179s | 97,005,072  | ok | PASS (6 features) |
| research        | 5s  | 577s | 167,279,024 | ok | PASS (7 features) |
| fuzz            | 5s  | 267s | 149,604,264 | ok | PASS (6 features) |
| fuzz-deep       | 4s  | 298s | 160,647,400 | ok | PASS (4 features) |
| sandbox         | 5s  | 133s | 7,326,496   | ok | PASS (5 features) |
| embedded        | 5s  | 154s | 86,830,992  | ok | PASS (2 features) |
| race            | 5s  | 246s | 138,787,680 | ok | PASS (5 features) |
| time-travel     | 5s  | 236s | 141,361,104 | ok | PASS (3 features) |

Build times were measured cold (empty `O=` tree, warm ccache disabled
implicitly — no shared host-compile cache). Sequential execution on a
4-CPU host with `-j4`. Boot was verified at `mem=256M`: the 64M default
was insufficient for the fuzz-deep and research KASAN-instrumented
binaries (initial matrix run caught this — see
`notes/timings.md`). All eight `init=/bin/true` boots complete cleanly
and the kernel's expected "Attempted to kill init!" panic fires (the
signature of a successful end-of-workload `/bin/true` exit under
init=1).

Size spread (7.3 MB sandbox → 167 MB research, **23×**) validates
the profile thesis: the same source tree produces radically different
shipping artifacts depending on which observability / sanitizer
surfaces are compiled in.

Numbers filled in after the build matrix completes. Feature-selftest
refers to the `tools/testing/selftests/um/profiles/run-profile-checks.sh`
verdict: PASS means every `FEATURE name STATE` line from
`probe-features.sh` inside the guest matches this profile's promise
(see the assertion table inside `run-profile-checks.sh`).

## Expected size ordering

From smallest to largest (approximate):

1. sandbox (minimum TCB, no modules, no debug)
2. embedded (similar to sandbox but with modules + mconsole)
3. prod-fast
4. prod-with-hooks (adds debugfs + tracing core)
5. time-travel (adds time-travel machinery + tracing)
6. fuzz (adds KCOV + KASAN)
7. fuzz-deep (fuzz + KASAN_INLINE + lockdep)
8. research (everything on: KASAN + UBSAN + FTRACE + PROVE_LOCKING + DEBUG_VM + ...)

## Expected boot ordering

All profiles target init=/bin/true boot in under 5 seconds.
Research, fuzz-deep, and time-travel are the slowest because of
sanitizer/lockdep overhead at init. Sandbox and prod-fast should be
the fastest to boot.

## How this document is generated

1. Run `/tmp/build-all-profiles.sh` (checked in alongside this
   note) which writes the raw CSV.
2. Numbers pasted into the table above manually. Re-run on a fresh
   checkout to validate; the absolute numbers will drift but the
   ordering should be stable.
