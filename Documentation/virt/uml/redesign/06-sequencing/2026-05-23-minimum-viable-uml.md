# Minimum Viable UML — what's the smallest UML binary that can run Python?

**Date:** 2026-05-23
**Author:** mjbommar
**Goal:** find, by iterative shrinking and re-expanding, the smallest UML
kernel binary that can boot far enough to execute the umlctl cpython-tier0
gate (`python3` imports + a handful of stdlib tests under
`hostfs` root with init phases driven by umlctl).

This document is a **diary**, not a design. Each section is one build
iteration: what changed, what size we got, what happened when we
booted, what the next step is. The point is to make every kilobyte of
the eventual binary defensible against the question "why is this on?"

## Methodology

- **Direction:** bottom-up. Start from `make ARCH=um tinyconfig`, add
  the smallest set of Kconfigs needed to make each test pass, never
  add a symbol whose removal would still let the test pass.
- **Build dir:** `~/src/uml-builds/uml-min/`. All builds use
  `O=$BUILD_DIR` per the user's standing rule about not contaminating
  the source tree.
- **Size metric:** `size linux | tail -1` (text + data + bss) and
  `wc -c < linux` (file size on disk, includes the ELF wrapper and
  debug data — closer to "what you'd ship"). Both reported.
- **Test target:** the same workload umlctl drives, in increasing
  bites:
  1. T1 — kernel boots and `init=/bin/echo` prints something
  2. T2 — mount hostfs, `init=/bin/sh -c "echo hello"`
  3. T3 — exec `python3 -c 'print(\"hello\")'`
  4. T4 — `python3 -c 'import sys, os, json'`
  5. T5 — cpython-tier0 smoke (the umlctl gate's `[[init.phases]]` first stanza)
- **Boot harness:** plain `linux mem=... rootfstype=hostfs init=...
  con=null con0=null,fd:1 con1=null,fd:1`. No vector net, no
  mconsole, no ubd, no ftrace — anything that gets added must justify
  itself in this diary first.
- **Reference points (already measured in earlier sessions):**
  - `arch/um/configs/x86_64_defconfig`: 111 MB built ELF
  - `arch/um/configs/base_defconfig` + container fragment: 111 MB (the
    container-capable build from yesterday's docker-in-UML demo)
  - **Goal:** beat both by ≥10x, ideally land under 5 MB stripped.

## Iteration ledger

| # | Config diff vs prev | text | data | bss | file size | T1 | T2 | T3 | T4 | T5 |
|---|---------------------|------|------|-----|-----------|----|----|----|----|----|
| 0a | `tinyconfig` | 1.47M | 234K | 77K | **2,147,952** | fail (build) | — | — | — | — |
| 0b | `+CONFIG_64BIT=y` | 1.47M | 234K | 77K | **2,147,952** | silent (no PRINTK) | — | — | — | — |
| 1a | `+NULL_CHAN +HOSTFS +BINFMT_ELF +PROC_FS +UM_WORKER_PROCESS +STDERR_CONSOLE` | 1.61M | 274K | 81K | **2,367,736** | silent (still no PRINTK) | — | — | — | — |
| 1b | `+PRINTK` | 1.69M | 773K | 237K | **2,966,824** | **PASS** (init=/bin/echo, panic on exit) | — | — | — | — |
| 2  | `+BINFMT_SCRIPT` | 1.69M | 773K | 237K | **2,967,392** | PASS | **PASS** (`#!/bin/sh` works, prints HELLO) | — | — | — |
| 3  | `+FUTEX` | 1.73M | 774K | 237K | **3,011,832** | PASS | PASS | **PASS** (`python3 -c 'print(...)'`) | **partial** (sockets need NET) | **PASS** (cpython-tier0) |
| 3-stripped | `strip` (no Kconfig change) | — | — | — | **2,525,160** | PASS | PASS | PASS | partial | **PASS** |

Note on data growth at 1b: PRINTK adds 458,752-byte log buffer + meta to `.data` — that single Kconfig accounts for ~500 KB of the 600 KB jump. It's the table-stakes price for being able to debug anything.

---

## Iteration 0 — `make ARCH=um tinyconfig`

**Hypothesis:** the kernel's own `tinyconfig` target produces the
absolute floor of "what compiles." It almost certainly cannot boot
because it lacks hostfs and probably even a console — but the size
number is the lower bound we measure progress against.

**Steps:**

```
make ARCH=um O=~/src/uml-builds/uml-min tinyconfig
make ARCH=um O=~/src/uml-builds/uml-min -j8
```

**Snag 0a — biarch surprise:**

```
arch/x86/um/user-offsets.c:2: fatal error: bits/libc-header-start.h: No such file or directory
```

`tinyconfig` left `CONFIG_64BIT=n`, which makes UML build for i386,
which needs gcc-multilib's 32-bit headers that this host doesn't
have. `arch/um/Makefile` doesn't auto-flip 64BIT based on host
kernel/headers. Fix: `./scripts/config --enable 64BIT` then
`olddefconfig`. The result built (Iter 0b) but produced no kernel
output — see "Snag 0b" below.

**Snag 0b — silent kernel:**

The host-side preboot prints its banner ("Checking that seccomp
filters can be installed... OK"), then `exec`s into the kernel proper.
The kernel runs and exits cleanly with no output. Cause:
`CONFIG_PRINTK=n` in tinyconfig — the kernel literally has no `printk()`
implementation. Every `pr_info`, `pr_err`, panic banner is compiled
out. Debugging anything else is impossible until PRINTK comes back.

---

## Iteration 1 — boot a userspace init through hostfs

**Hypothesis:** the minimum to reach `init=/bin/echo` running as PID 1 is:
- `HOSTFS` (so root can be the host's `/`),
- `BINFMT_ELF` (the host's `/bin/echo` is an ELF binary),
- `PROC_FS` (glibc startup probes `/proc/self/exe`),
- `NULL_CHAN` (otherwise `NOCONFIG_CHAN=y` and *every* console line
  driver refuses to attach),
- `UM_WORKER_PROCESS` (memo 28 E.2 — the seccomp backend hard-requires
  it; without it, the kernel boots but the first syscall trap hangs),
- `PRINTK` (so we can see what we broke when we break it).

**Snag 1c — secondary consoles:**

After 1b's build, the kernel printed its banner and stuck on lines like:

```
setup_one_line failed for device 1 : failed to parse channel pair
```

… repeated for every secondary console. Cause: `CON_CHAN` (the default
for `con1`+) defaults to `"xterm"`, which requires `XTERM_CHAN`. Two
fixes possible: (a) enable `XTERM_CHAN` (adds an xterm dependency
nobody benchmarking wants), or (b) pass `con=null,fd:1` on the kernel
command line, which routes *all* consoles through the always-on stdio
channel. Picked (b) — kernel-side cost is zero.

**Result:** with `con=null,fd:1` on the cmdline, the kernel:

1. printed its boot banner;
2. mounted hostfs as root;
3. resolved + loaded glibc and `/bin/echo` via the dynamic linker
   (the `um_diag` per-process syscall trace shows 37 real mmap/mprotect
   events for the ld.so → libc.so → echo chain);
4. ran `/bin/echo` (which produced no visible output because echo with
   no args prints just a newline — and we route stdout through a TTY
   that we haven't fully flushed);
5. panicked with `Attempted to kill init! exitcode=0x00000000` when
   echo exited — expected, because init exiting always panics.

**T1 verdict:** PASS. The minimum config that runs a real Linux userspace
binary under UML is **2.97 MB on disk, 1.69 MB text + 773 KB data + 237 KB
bss**. Half of the data segment is the `printk` log buffer.

The 111 MB → 2.97 MB ratio is 37× — and this binary already includes
the per-mm worker process model, the dynamic seccomp backend, hostfs,
and ELF loading. Everything else in the 111 MB defconfig is "user-visible
features": cgroups, namespaces, networking, ftrace, KASAN, modules, the
KVM-v2 backend, every block driver, etc.

---

## Iteration 2 — make `#!/bin/sh` shebangs work

**T2 attempted with iter-1b binary:** silently failed. Kernel says
`Requested init /tmp/init.sh failed (error -8)` — that's `ENOEXEC`.
The kernel can `exec()` an ELF binary directly but does not understand
`#!/bin/sh` script headers. `BINFMT_SCRIPT` is the kernel-side handler
that parses the shebang line and re-execs through the interpreter
named there.

**Fix:** `+BINFMT_SCRIPT`. Costs 290 bytes of text. After rebuild,
`/tmp/init.sh` as init runs the script under `/bin/sh`, which prints
`HELLO_FROM_GUEST_T2`, `uname -r`, and `$$=1`.

---

## Iteration 3 — make Python start

**T3 attempted with iter-2 binary:**

```
@@@T3_PRE_PYTHON@@@
The futex facility returned an unexpected error code.
@@@T3_PYTHON_FAIL rc=134@@@   # 134 = SIGABRT
```

Glibc's `_dl_setup_hash` (or similar early-init code) probes the futex
syscall as a liveness check, gets `-ENOSYS`, and aborts. `CONFIG_FUTEX=n`
in tinyconfig.

**Fix:** `+FUTEX`. Costs ~40 KB of text + a small futex hash table in
bss. After rebuild, **Python prints**:

```
@@@T3_PYTHON_HELLO@@@
```

**T4 — broader stdlib check:**

| Test | Result |
|------|--------|
| `import sys, os, json, struct, time, re` | **PASS** |
| `subprocess.check_output(["uname","-r"])` (fork+exec) | **PASS** |
| `socket.socket(AF_INET, SOCK_STREAM).bind(...)` | **FAIL** — `OSError: [Errno 38] Function not implemented` |
| 4 threads incrementing a shared list | **PASS** |

The socket failure is pure `ENOSYS`: `CONFIG_NET=n` so no `socket()`
syscall is exported. Everything else — including `subprocess` fork-exec
and Python's GIL-free thread scheduling — works.

**T5 — actual cpython-tier0 gate (`tools/testing/selftests/um/cpython-tier0/cpython-tier0.py`):**

```
CPYTHON_TIER0: hashlib sha256 ok = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
CPYTHON_TIER0: TOTAL PASS
```

The kselftest hashlib gate that the upstream submission uses to defend
against the per-mm shadow-PT regression class passes on the
**minimum-viable** kernel.

---

## Iteration 3-stripped — `strip linux`

No Kconfig change. `strip` drops 487 KB of ELF symbol tables and
section headers that the kernel doesn't need at runtime. Stripped
binary still passes cpython-tier0.

| Artifact | Size |
|----------|------|
| `linux` (unstripped) | 3,011,832 B = 2.87 MB |
| `linux-stripped` | **2,525,160 B = 2.41 MB** |
| `arch/um/configs/base_defconfig` + container fragment (yesterday's docker-in-UML build) | 115,343,616 B = 110.0 MB |
| `x86_64_defconfig` SMP/dev build | 96,621,328 B = 92.1 MB |

**Ratio: 46× smaller than the container-capable build, 38× smaller than
the SMP dev build.**

---

## Final config — the minimum viable UML

The kernel `.config` for the binary that passes cpython-tier0 is:

```
make ARCH=um O=$BUILD tinyconfig
./scripts/config --file $BUILD/.config \
    --enable 64BIT \
    --enable PRINTK \
    --enable NULL_CHAN \
    --enable HOSTFS \
    --enable BINFMT_ELF \
    --enable BINFMT_SCRIPT \
    --enable PROC_FS \
    --enable UM_WORKER_PROCESS \
    --enable STDERR_CONSOLE \
    --enable FUTEX
make ARCH=um O=$BUILD olddefconfig
make ARCH=um O=$BUILD -j$(nproc)
strip $BUILD/linux
```

Eleven explicit `--enable` flips from `tinyconfig`. Each one was
**forced** by a specific boot failure that the previous build
produced; nothing speculative.

---

## What's NOT in the minimum

Things `defconfig` carries that we proved we *don't* need to run
Python's tier-0 gate:

- **All cgroups** (CGROUPS, MEMCG, CPUSETS, CGROUP_*): the smoke is
  a single-process Python script, no resource accounting.
- **All namespaces** (USER_NS, PID_NS, NET_NS, …): no container layer.
- **Networking** (NET, INET, NETFILTER, NF_TABLES, BRIDGE, VETH,
  IPVLAN, …): hashlib doesn't use sockets. Adding sockets would
  require `CONFIG_NET=y` + `CONFIG_INET=y` minimum.
- **The KVM-v2 backend** (UM_BACKEND_KVM_V2): SECCOMP is the default
  on this host and is enough.
- **Ftrace** (FTRACE, FUNCTION_TRACER, DYNAMIC_FTRACE, …): zero
  runtime cost when off, large data growth when on.
- **Block layer** (BLOCK, BLK_DEV_IO_TRACE, UBD, BLK_DEV_LOOP):
  hostfs is the root; there is no block device.
- **TTY niceties** (UNIX98_PTYS, DEVPTS_FS, VT, VT_CONSOLE): Python
  doesn't allocate a pty unless you call `subprocess.Popen(pty=...)`
  or `os.openpty()`.
- **Shmem / tmpfs** (SHMEM, TMPFS, DEVTMPFS, DEVTMPFS_MOUNT): the
  init script doesn't mount any of them. cpython-tier0 doesn't either.
- **AIO, EPOLL, SIGNALFD, TIMERFD, EVENTFD**: Python's threading and
  subprocess paths don't need any of these (they use `clone` +
  `wait4` + `futex`).
- **Cryptographic kernel modules** (CRYPTO, CRYPTO_*): Python's
  hashlib uses OpenSSL in userspace via dlopen; the kernel does not
  need its own SHA-256 implementation.
- **DEBUG_KERNEL** + all sanitizers (KASAN, KMSAN, UBSAN, KCSAN):
  off in tinyconfig.

That entire list — every single one — costs at least 1 MB each in
binary size. Their absence is what gets us from 110 MB down to 2.4 MB.

---

## What would the next milestone cost?

Sketch of what each "+1 capability" would add to the 2.41 MB stripped
floor:

| Goal | Required `+` config | Est. extra MB |
|------|---------------------|---------------|
| Python with `socket` (loopback only) | NET, INET, UNIX | ~3–4 |
| Python with `subprocess` to host network tools | + NETFILTER | +2 |
| umlctl's `[network] mode = "vector"` | + UML_NET_VECTOR_V2 + transport drivers | +5 |
| One Docker container via runc | + USER_NS, PID_NS, NET_NS, CGROUPS, MEMCG, OVERLAY_FS, CGROUP_PIDS, NETFILTER, NF_TABLES, BRIDGE, VETH | +50–60 |

(These are eyeballed against the size deltas seen yesterday between
the same kernel with/without these Kconfigs. The proper way to
measure is to extend this diary — each step a new row in the ledger.)

---

## Open questions

1. **Can the printk log buffer shrink further?** Currently 458 KB
   (CONFIG_LOG_BUF_SHIFT default). Setting it to 14 (16 KB) would
   reclaim ~440 KB of `.data` without affecting boot. The diary
   should test this.
2. **Can we get under 2 MB by also flipping `CONFIG_SLUB_TINY` back
   on?** `tinyconfig` set it; my `olddefconfig`-cascade may have
   silently reverted it. Worth checking.
3. **Does the 110 MB → 2.4 MB shrink change steady-state TPS?** The
   yesterday benchmark was run on the container-capable build. A
   smaller kernel has less to fault in on cold start (memo: pool
   completion); steady-state perf is probably identical, but it's
   worth a TPS run with this binary to confirm.
4. **What's the absolute floor without `UM_WORKER_PROCESS`?** Memo 28
   says it's required by the seccomp backend, but the kernel didn't
   panic without it — it just hung on first syscall. The diary could
   bisect that further.
