# LTP curation walk done (2026-05-14)

## What

Phase J Track A §3.3 — LTP curation memo. Sub-agent produced
`phase-J-ltp-curation-2026-05-14.md` (761 lines).

## Headline findings

- **LTP not in apt** on Ubuntu 26.04 Resolute. No
  `ltp`/`ltp-source`/`ltp-tests`/`linux-test-project` package.
  Operator must `git clone https://github.com/linux-test-project/
  ltp` then `make install` once (~500 MB; lives at `/opt/ltp/`).
- **`runltp` is gone** in upstream LTP. Replaced by `kirk` (a
  separate repo `linux-test-project/kirk`). The Phase J design
  memo's `runltp -S skip -p -q` example is superseded; runner
  template now specs `kirk --run-suite <set> --skip-file
  <list> --json-report <file> --sut host`.
- **Most surprising hostile directory:** `testcases/kernel/kvm/`.
  LTP ships in-guest KVM tests (`kvm_svm0[1-4]`, `kvm_vmx0[12]`,
  `kvm_pagefault01`) that exercise the *host's* `/dev/kvm` from
  guest userspace. Under UML these would either ENOENT or, worse,
  reach through hostfs to poke the actual host hypervisor. **Hard
  SKIP**, called out as a top-priority deny-list entry.
- **Curated suite:** ~2 050 tests across smoketest + syscalls +
  syscalls-ipc + sched + pty + mm + nptl + math + capability.
  112-entry per-test deny-list covering modules, keyring, BPF,
  hugepages, fanotify, perf, kexec/reboot, netns, io_uring,
  mlock, landlock, swap.
- **Cadence:** 45 min target / 60 min hard-kill per LTP cycle.
  Per Phase J design memo §4.3, scheduled every 8 daemon
  rotations → ~4-6 LTP cycles per backend per 24 h soak.

## Bootstrap (one-time)

```sh
# On host:
git clone --depth 1 https://github.com/linux-test-project/ltp /opt/ltp-src
cd /opt/ltp-src
make autotools
./configure
make -j$(nproc)
sudo make install   # installs to /opt/ltp by default

# kirk runner:
git clone --depth 1 https://github.com/linux-test-project/kirk /opt/kirk
cd /opt/kirk
# kirk is Python — pip install --user OR use system python directly
```

## Open follow-ups (not blocking Phase J)

- `kirk` is Python — its dependencies (asyncio, paramiko if SSH
  SUT is used) may need a venv. For local-SUT use ("--sut host"),
  no SSH needed.
- LTP build inside the guest vs on the host: the curation memo
  picks host-build + hostfs-share, mirroring Tier 1/Tier 2's
  no-rebuild posture.

## Status

- LTP memo committed in this same commit.
- Runner template (`ltp-runner.toml.template`) NOT yet written —
  spec is in the memo §4. Deferred to a later landing.
- LTP installation on host NOT yet done — operator action; the
  memo's §1 carries the recipe.

## Implications for Track A (Phase J completion)

After Tier 3 + LTP runner template are written, Phase J has all
the workload types it needs. The 24 h soak run is then operator
time only.

## Next

Continue waiting on snapshot port (#168) sub-agent. Tier 3
template implementation is a bash-only follow-up.
