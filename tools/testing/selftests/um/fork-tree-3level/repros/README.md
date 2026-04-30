# fork-tree-3level reproducer suite

Bisection variants used to narrow the v2 fork-state bug (memo §E.4
follow-up to #95 / #96). All built statically with `-O0
-fno-stack-protector`. `raw_fork` additionally with `-nostdlib`.

| Reproducer            | Backend     | Result      | Notes |
|-----------------------|-------------|-------------|-------|
| `raw_fork.c`          | seccomp/v2  | PASS / PASS | No libc; raw asm syscalls. v2's syscall path itself is correct. |
| `libc_simple.c`       | seccomp/v2  | PASS / PASS | libc + printf; no fork. Excludes generic libc startup as the cause. |
| `libc_fork_no_wait.c` | seccomp/v2  | PASS / FAIL ~80% | Parent forks, exits without waiting; child _exit(0). Stack-smashing fires in CHILD only — confirmed by RC=0 (parent ok) plus stack-smashing-detected lines. |
| `libc_no_clone.c`     | seccomp/v2  | PASS / FAIL ~50% | libc + raw `clone(SIGCHLD, ...)` syscall (no CLONE_CHILD_*TID flags). Bug still fires — CLONE_CHILD_* is not the trigger. |
| `child_simple.c`      | seccomp/v2  | PASS / FAIL ~80% | Parent exits immediately; child does just `printf("CHILD_OK\\n")` and `_exit(0)`. Stack-smashing detected in child. Confirms bug is in v2's child-process setup, not parent. |
| `child_only_canary.c` | seccomp/v2  | PASS / FAIL 10/10 | Child calls `strlen(memset(buf, 0xaa, 64))` — heavy XMM/SSE-accelerated glibc internals. Always fires. Suggests v2's FPU/XMM state for the child task is wrong. |

Conclusion (2026-04-30): the bug is the CHILD process's stack /
register state being corrupted on first dispatch after fork. Most
likely candidates:
  - FPU/XMM state inherited from a different task on the same
    per-CPU vCPU (v2's `kvm_v2_fpu_install_on_first_run` may not
    handle the fresh-fork case)
  - CR3 or memslot inconsistency for child's address space
  - Child's gs.base / fs.base stale from previous task on same vCPU

The FS_BASE round-trip fix (commit 6e52574cca6c) closes one
related v1→v2 regression but does not resolve this bug — empirical
trace shows fs.base = 0x0 throughout child execution.

To reproduce:
```
gcc -static -O0 -fno-stack-protector -o child_simple child_simple.c
cat > /tmp/init.sh <<'EOF'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
./child_simple
echo "RC=$?"
sync
EOF
chmod +x /tmp/init.sh
$UML_BINARY backend=force=kvm-v2 mem=256M rootfstype=hostfs \
    root=/dev/root rw con=null con0=fd:0,fd:1 panic=-1 \
    init=/tmp/init.sh
```

Expected on seccomp: `CHILD_OK` then `RC=0`.
Expected on v2: `*** stack smashing detected ***: terminated`
followed by `RC=0` (8/10 runs at tip 6e52574cca6c).
