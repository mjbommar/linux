#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/template-pause-pivot-smoke - template-pause stack-pivot smoke.
#
# What this proves:
#
#   1. um_template_pause_pivot_test=1 arms the stack-pivot mode.
#   2. master forks via os_template_pause_fork_clone_to(@entry).
#   3. The child runs `child_entry_pivot_test` on a MAP_PRIVATE
#      stack, executes raw-syscall write of "PIVOT_OK\n" to host
#      fd 1, then exit_group(0).
#   4. No "Kernel tried to access user memory" panic at
#      um_template_pause_enter+0xf6.
#   5. Master sustains repeated iterations (multiple SIGCONTs ->
#      multiple PIVOT_OKs).
#
# Background:
#
#   The host-side primitive is validated in
#   tools/testing/selftests/um/rt-sigreturn-isolation/. This selftest
#   checks the same primitive in real kernel context after fork.
#
# Exit codes:
#   0  PASS  - >= 5 PIVOT_OK occurrences and zero access panics.
#   1  FAIL  - PIVOT_OK missing, panic present, or master crashed.
#   4  SKIP  - kernel binary missing or python3 unavailable.
#
# Environment:
#   UML_BINARY  UML kernel with CONFIG_UM_TEMPLATE_PAUSE_FORK=y.
#               Default: $HOME/src/uml-builds/uml-tplpause-fork/linux.

set -u

KERNEL=${UML_BINARY:-$HOME/src/uml-builds/uml-tplpause-fork/linux}

if [ ! -x "$KERNEL" ]; then
	echo "SKIP: UML binary $KERNEL not found (set UML_BINARY)"
	exit 4
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "SKIP: python3 required"
	exit 4
fi

OUT=$(mktemp -d -t template-pause-pivot-smoke.XXXXXX)
trap 'rm -rf "$OUT"' EXIT

cat >"$OUT/init.sh" <<'IEOF'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
echo TPP_PRE_PAUSE pid=$$
echo fork-smoke > /proc/um/template_pause
echo TPP_POST_PAUSE pid=$$ rc=$?
sleep 10
exit 0
IEOF
chmod +x "$OUT/init.sh"

PYRC=0
python3 - "$KERNEL" "$OUT/init.sh" "$OUT/boot.log" <<'PYEOF' || PYRC=$?
import ctypes, os, signal, sys, time

kernel, init_path, log_path = sys.argv[1], sys.argv[2], sys.argv[3]

libc = ctypes.CDLL(None)
libc.prctl(36, 1, 0, 0, 0)  # PR_SET_CHILD_SUBREAPER

log = open(log_path, "wb")
pid = os.fork()
if pid == 0:
    os.dup2(log.fileno(), 1)
    os.dup2(log.fileno(), 2)
    os.execve(kernel, [
        "linux", "mem=128M", "rootfstype=hostfs", "rootflags=/",
        "root=/dev/root", "rw", "ncpus=1",
        "um_template_pause=fork",
        "um_template_pause_pivot_test=1",
        f"init={init_path}",
    ], os.environ.copy())
    os._exit(127)

def state(p):
    try:
        with open(f"/proc/{p}/status") as fh:
            for ln in fh:
                if ln.startswith("State:"):
                    return ln.split()[1]
    except FileNotFoundError:
        return "X"
    return "?"

# Wait for master's first SIGSTOP.
for _ in range(120):
    s = state(pid)
    if s in ("T", "X"):
        break
    time.sleep(0.1)

if state(pid) == "X":
    log.close()
    sys.exit("master exited before reaching SIGSTOP")

# Drive 20 fork-on-resume iterations.
cont_count = 0
last_cont = 0.0
deadline = time.time() + 15
while time.time() < deadline and cont_count < 20:
    if state(pid) == "T" and time.time() - last_cont > 0.05:
        os.kill(pid, signal.SIGCONT)
        cont_count += 1
        last_cont = time.time()
    try:
        rpid, _ = os.waitpid(-1, os.WNOHANG)
        if rpid == 0:
            time.sleep(0.05)
    except ChildProcessError:
        time.sleep(0.05)

# Tear down master.
if state(pid) not in ("X", "?"):
    os.kill(pid, signal.SIGKILL)
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass
log.close()

# Reap orphans.
while True:
    try:
        rpid, _ = os.waitpid(-1, os.WNOHANG)
        if rpid == 0:
            break
    except ChildProcessError:
        break

with open(log_path, errors="replace") as fh:
    content = fh.read()

pivot_count = content.count("PIVOT_OK")
panic = "Kernel tried to access user memory" in content
ceiling = "um_template_pause_enter+0xf" in content

print(f"PIVOT_OK count   : {pivot_count}")
print(f"NULL-call panic  : {panic or ceiling}")
print(f"SIGCONT sent     : {cont_count}")

if panic or ceiling:
    print("FAIL: NULL-call panic present")
    sys.exit(1)
if pivot_count < 5:
    print(f"FAIL: too few PIVOT_OK ({pivot_count} < 5)")
    sys.exit(1)
print("PASS")
PYEOF

case $PYRC in
0) exit 0;;
4) exit 4;;
*) echo "FAIL: harness rc=$PYRC; see $OUT/boot.log"; cp "$OUT/boot.log" /tmp/tpp-smoke-fail.log 2>/dev/null || true; exit 1;;
esac
