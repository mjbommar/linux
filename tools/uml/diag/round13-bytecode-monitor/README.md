# bc-monitor — bytecode-integrity LD_PRELOAD monitor

Round 13 diagnostic for the Django cache-flake investigation. Used to
catch the 16-byte-zero corruption pattern in CPython 3.14 bytecode
pages by wrapping `getpid` / `gettid` / `clock_gettime` (the
gadget-handled syscalls) and scanning rwxp anon pages in the UML
guest user VA range for transitions from "no big zero run" to ">=
20-byte zero run".

Build:

    gcc -O2 -fPIC -shared -o bc-monitor.so bc-monitor.c -ldl

Use (set env in the soak `.toml` `[env]` section or pass to the
guest's init script):

    LD_PRELOAD=/home/mjbommar/bc-monitor.so
    BC_MONITOR_LOG=/home/mjbommar/bc-monitor.log
    BC_MONITOR_INTERVAL=32
    BC_MIN_ZERO_RUN=20

Outcome (Round 13): caught a deterministic Python startup pattern
(first heap page, bytes [2404, 2888) — 484 bytes — after a
`clock_gettime`) but did not catch the actual cache-abort
corruption. The seed-on-first-non-zero observation race in v4 made
pages allocated via `brk(2)` after monitor init undetectable. The
actual root cause was pinned via comparative analysis with prior
SMP-T26/T27/T57 work and shipped as SMP-T73 (XSAVE swap).

Kept as reference for future investigations that need a similar
"page acquired a fresh zero run" signal.
