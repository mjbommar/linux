# fork-tree-3level reproducer suite

Bisection variants used to narrow the v2 fork-state bug (memo §E.4
follow-up to #95 / #96). All built statically with `-O0
-fno-stack-protector`. `raw_fork` additionally with `-nostdlib`.

| Reproducer            | Backend     | Result      | Notes |
|-----------------------|-------------|-------------|-------|
| `raw_fork.c`          | seccomp/v2  | PASS / PASS | No libc; raw asm syscalls. v2's syscall path itself is correct. |
| `libc_simple.c`       | seccomp/v2  | PASS / PASS | libc + printf; no fork. Excludes generic libc startup as the cause. |
| `libc_fork_no_wait.c` | seccomp/v2  | PASS / FAIL ~80% | Parent forks, exits without waiting; child _exit(0). Stack-smashing fires in CHILD only. |
| `libc_no_clone.c`     | seccomp/v2  | PASS / FAIL ~50% | libc + raw `clone(SIGCHLD, ...)` syscall (no CLONE_CHILD_*TID flags). Bug still fires — CLONE_CHILD_* is not the trigger. |
| `child_simple.c`      | seccomp/v2  | PASS / FAIL ~70% | Parent exits immediately; child does just `printf("CHILD_OK\n")` and `_exit(0)`. |
| `child_only_canary.c` | seccomp/v2  | PASS / FAIL 10/10 | Child calls `strlen(memset(buf, 0xaa, 64))` — heavy XMM/SSE workload. |
| `child_no_write.c`    | seccomp/v2  | PASS / FAIL ~90% | Child does ZERO stack writes — straight asm to exit_group(0). Bug still fires; rules out CoW-on-first-stack-write hypothesis. |
| `canary_v2.c`         | seccomp/v2  | PASS / FAIL ~20% | Reads `%fs:0x28` in parent + child + parent_post, logs to a file. **DECISIVE FINDING: when child's canary read succeeds, parent and child see IDENTICAL canary values.** So `fs.base` and the canary GLOBAL are correct. The bug is **stack memory corruption** somewhere — function-entry canary save vs function-epilogue canary check disagree because some path between them overwrites the saved canary on stack. |
| `canary_locate.c`     | seccomp/v2  | (handler not caught) | Tries to install a SIGABRT handler that captures the RIP at __stack_chk_fail. Handler doesn't fire — glibc's abort path may not deliver SIGABRT here. |
| `sigblock.c`          | seccomp/v2  | PASS / FAIL ~70% | Blocks ALL signals via `sigprocmask(SIG_BLOCK, &all)` before fork. Bug still fires at the same rate. **Eliminates signal-delivery / sigframe setup as the cause.** |
| `child_delay.c`       | seccomp/v2  | PASS / FAIL 10/10 | Child does `usleep(100ms)` BEFORE its first stdio call. **Bug becomes deterministic 100% with delay** — the bug accumulates over time/dispatches in the child. |
| `parent_busy.c`       | seccomp/v2  | PASS / FAIL ~50% | 100 `getpid()` calls in parent BEFORE fork. Bug rate roughly unchanged — parent activity does not affect the trigger. |

Conclusion (2026-04-30, after canary_v2 narrowing): the bug is
**stack memory corruption** somewhere in v2's user-mode round-trip
path for child processes. NOT a canary-value bug, NOT an fs.base
bug, NOT a CoW-on-first-write bug.

What the canary_v2 reproducer pinpoints: the saved canary on the
child's stack at function-entry is overwritten by *something*
before the function-epilogue check. Possible mechanisms:
  - Signal frame setup writing to wrong offset (do_signal /
    sigframe under v2 may smash adjacent stack)
  - SYSRETQ / IRETQ return path corrupting user stack
  - Some other v2-specific kernel-side write to user memory

Eliminated this session via empirical test:
  - FPU / XMM stale state (forced arch defaults; no improvement)
  - Segment register cache leak (explicit re-set; no improvement)
  - Real KVM_SET_SREGS ioctl vs SYNC_REGS dirty-bit (no diff)
  - cr2 leak (defensive fix landed at 24a7f0575e18; minor effect)
  - FS_BASE round-trip (defensive fix landed at 6e52574cca6c)
  - interrupt_end placement (commit 31ba9c354063 bisected
    negative)
  - CLONE_CHILD_*TID flags
  - glibc atexit/stdio cleanup
  - CoW page allocation on first stack write
  - TLS canary value mismatch (canary_v2 confirms parent==child
    when measurable)

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
