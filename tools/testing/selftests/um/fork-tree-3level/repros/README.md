# fork-tree-3level reproducer suite

These standalone programs isolate process-tree, fork/exec, and child
address-space behavior used by the fork-tree-3level regression gate.
They are intentionally small, static when practical, and built without
optimization so failures are easy to attribute.

The suite is useful for separating these classes of failure:

| Reproducer            | Purpose |
|-----------------------|---------|
| `raw_fork.c`          | Raw syscall fork path without libc startup. |
| `libc_simple.c`       | libc startup and printf without fork. |
| `libc_fork_no_wait.c` | Parent exits without waiting for the child. |
| `libc_no_clone.c`     | Raw `clone(SIGCHLD, ...)` without child TID flags. |
| `child_simple.c`      | Child performs minimal stdio and exits. |
| `child_only_canary.c` | Child exercises stack and SIMD-heavy libc code. |
| `child_no_write.c`    | Child exits without stack writes. |
| `canary_v2.c`         | Compares parent and child stack-canary state. |
| `canary_locate.c`     | Attempts to capture the abort site. |
| `sigblock.c`          | Blocks all signals before fork. |
| `child_delay.c`       | Delays child execution before first libc call. |
| `parent_busy.c`       | Adds parent syscall churn before fork. |

The expected baseline is that seccomp passes these reproducers. A
kvm-v2 failure here points at fork-state isolation, child address-space
state, signal-frame setup, or user-state restore rather than generic
libc startup.

To reproduce one case manually:

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

Expected successful output is `CHILD_OK` followed by `RC=0`.
