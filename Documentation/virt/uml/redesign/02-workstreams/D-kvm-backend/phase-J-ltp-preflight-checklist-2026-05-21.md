# Phase J LTP preflight — operator copy-paste checklist (2026-05-21)

> Companion to `phase-J-ltp-curation-2026-05-14.md` (design DONE) and
> `phase-J-tier3-design-2026-05-14.md`. The curation memo settled
> KEEP/SKIP/TRIAGE classification (~2 050 active tests, 112-entry
> deny-list) and the runner template shape; this memo is the
> operator-facing preflight runbook that turns a clean host into a
> "ready for first LTP cycle" host. Every item below is either
> verified DONE in-tree at landing or has an exact shell command for
> the operator action.

## 0. TL;DR copy-paste checklist

```
[x] LTP curation memo DONE
    -> phase-J-ltp-curation-2026-05-14.md
[x] tools/testing/selftests/um/soak/ltp-skip.txt  (112 entries)
[x] tools/testing/selftests/um/soak/ltp-suite.txt (9 KEEP suites)
[x] tools/testing/selftests/um/soak/ltp-triage.txt (8 TRIAGE-LATER suites)
[x] tools/testing/selftests/um/soak/ltp-runner.toml.template
[x] tools/testing/selftests/um/soak/run-ltp-smoke.sh (single-suite probe)
[x] run-soak-daemon.sh ltp-runner timeout=3600 wired
[ ] CONFIG_UML_NET_VECTOR=y in active kernel build
    -> see §1 below; not in any defconfig fragment, operator-set
[ ] /opt/ltp populated (LTP HEAD 8cd7644, VERSION 20260130)
    -> see §2 below
[ ] /opt/kirk populated (kirk HEAD 07964a106584)
    -> see §2 below
[ ] $HOME/.cache/uml-soak-tier2-venv exists (Tier 2 prebuilt)
    -> DONE on this host; see §3
[ ] $HOME/.cache/uml-soak-tier3-django-venv exists
    -> NOT YET; see §3
[ ] $HOME/.cache/uml-soak-tier3-fastapi-venv exists
    -> NOT YET; see §3
[ ] First LTP smoke cycle  (./run-ltp-smoke.sh smoketest)
    -> see §4; ~30-60 s wall-clock
[ ] First curated LTP cycle (daemon ltp-runner workload)
    -> see §4; 45 min target / 60 min hard kill
```

The four `[ ]` lines below the in-tree `[x]` block are the only
operator steps remaining. Each has its exact shell command in the
section it points to.

## 1. CONFIG_UML_NET_VECTOR=y — operator action required

**Status: NOT in any in-tree defconfig.** Verified:

```
$ grep -l UML_NET_VECTOR arch/um/configs/*defconfig arch/um/configs/profiles/*.config
(no output)
```

The Kconfig entry exists at `arch/um/drivers/Kconfig:127` (`config
UML_NET_VECTOR`); it just defaults off and is not enabled by either
`base_defconfig`, `x86_64_defconfig`, `i386_defconfig`, or any of the
ten profile fragments under `arch/um/configs/profiles/`. This is the
**single biggest preflight item** because Tier 3 (Django/FastAPI
loopback) cannot bring up its TAP interface without it.

Operator command (per `tools/testing/selftests/um/soak/README.md`
§"Tier 3 bootstrap"):

```sh
cd ~/src/uml-builds/uml-smp-t41fix    # or whichever build dir
./scripts/config --enable CONFIG_UML_NET_VECTOR
make ARCH=um O=$(pwd) olddefconfig -j
make ARCH=um O=$(pwd) -j

# Verify:
grep CONFIG_UML_NET_VECTOR= .config
# Expect: CONFIG_UML_NET_VECTOR=y
```

Optional follow-up for the vector v2 Tier 3 aliases:

```sh
./scripts/config --enable CONFIG_UML_NET_VECTOR_V2
./scripts/config --enable CONFIG_UML_NET_VECTOR_V2_INPROC
make ARCH=um O=$(pwd) olddefconfig -j
make ARCH=um O=$(pwd) -j
grep -E 'CONFIG_UML_NET_VECTOR_V2(=|_INPROC=)y' .config
```

Why not land a defconfig flip here? The umbrella decision in
`02-workstreams/D-kvm-backend/post-2026-05-19-next-sprint/01-vector2-default-flip.md`
is to flip the default to vector2 (not legacy vector), so adding a
`CONFIG_UML_NET_VECTOR=y` line to `x86_64_defconfig` now would lock
the legacy driver into every profile right before that flip lands.
Operator-set is the right shape for Phase J validation; the defconfig
flip is the post-J cleanup step in the next-sprint plan.

LTP itself (the `syscalls` and `mm` KEEP suites) does **not** need
`UML_NET_VECTOR=y` — the LTP runner template uses `[network] mode =
"none"` (line 45 of `ltp-runner.toml.template`). The flag is only
required if the operator also runs the Tier 3 workloads in the same
soak rotation, which is the documented Phase J target.

## 2. LTP + kirk clone — operator action required

**Status: neither tree present on the host.** Verified:

```
$ ls /opt/ltp /opt/kirk
ls: cannot access '/opt/ltp': No such file or directory
ls: cannot access '/opt/kirk': No such file or directory
$ find $HOME -maxdepth 4 -type d \( -name ltp -o -name kirk \) 2>/dev/null
(no output)
```

Operator commands (per curation memo §1):

```sh
# LTP: clone + build + install at /opt/ltp.
sudo install -d -o $USER -g $USER /opt/ltp-src /opt/ltp /opt/kirk
git clone --depth 1 https://github.com/linux-test-project/ltp /opt/ltp-src
cd /opt/ltp-src
git fetch --unshallow                          # need history to pin
git checkout 8cd7644a52e3                      # curation memo HEAD
make autotools
./configure --prefix=/opt/ltp
make -j$(nproc)
sudo make install
# Verify:
test -d /opt/ltp/runtest && cat /opt/ltp/Version 2>/dev/null
# Expect VERSION ~ 20260130

# kirk: pure-Python; no build needed.
git clone --depth 1 https://github.com/linux-test-project/kirk /opt/kirk
cd /opt/kirk
git fetch --unshallow
git checkout 07964a106584                      # curation memo HEAD
# Verify:
python3 -m libkirk.main --help >/dev/null && echo KIRK_OK
```

Pin discipline (curation memo §6.5): the HEAD hashes above were
correct at curation memo landing (2026-05-14). Before kicking the
first full LTP cycle the operator should re-confirm the HEADs are
still on the upstream branches and bump these lines (and the soak
daemon's `config.json`) if a newer pinned release tag is preferred.
Most-recent release tag at curation memo landing: `20260130`.

Disk: LTP source ~250 MB; built `/opt/ltp/` ~150 MB; kirk ~5 MB.

Apt prerequisites (per curation memo §6.4) — Ubuntu Resolute:

```sh
sudo apt-get install -y bison flex autoconf automake pkg-config gcc \
                        libtool-bin make git \
                        $(apt list --installed 2>/dev/null \
                            | grep -q '^linux-headers-' \
                            || echo linux-headers-generic)
```

## 3. Tier 2 + Tier 3 venvs

**Tier 2 status: DONE.** Verified:

```
$ ls -d $HOME/.cache/uml-soak-tier2-venv
/home/mjbommar/.cache/uml-soak-tier2-venv
```

(Created during earlier Phase J Tier 2 work — see STATUS.md row J,
commit `b78ac72e0b3c` "80/80 smoke".) Refresh only needed when
bumping package versions.

**Tier 3 status: NOT YET.** Verified:

```
$ ls -d $HOME/.cache/uml-soak-tier3-django-venv $HOME/.cache/uml-soak-tier3-fastapi-venv
ls: cannot access '/home/mjbommar/.cache/uml-soak-tier3-django-venv': No such file or directory
ls: cannot access '/home/mjbommar/.cache/uml-soak-tier3-fastapi-venv': No such file or directory
```

Note: the **current** `tier3-{django,fastapi}.toml.template` bodies
use a Python stdlib `http.server` shim (lines 70-83 of the Django
template; lines 67-80 of the FastAPI template), so they do not
require Django/FastAPI to be installed for the first wiring smoke.
The venvs are only needed once the operator graduates the templates
to invoke real Django (`python3 -m django runserver`) or real
uvicorn (`python3 -m uvicorn app:app`), which is a follow-on step
per the template headers' own notes.

If the operator wants real Django/FastAPI now:

```sh
uv venv $HOME/.cache/uml-soak-tier3-django-venv
uv pip install --python $HOME/.cache/uml-soak-tier3-django-venv/bin/python django

uv venv $HOME/.cache/uml-soak-tier3-fastapi-venv
uv pip install --python $HOME/.cache/uml-soak-tier3-fastapi-venv/bin/python fastapi uvicorn
```

Then edit the templates' `tier3-django-up` / `tier3-fastapi-up`
phases to invoke the venv's `python3` instead of system `python3`,
following the Tier 2 template's `tier2-uv-pylibs` phase as the
shape reference (`cd /tmp && /home/.../bin/python ...`).

## 4. First-run procedure

### 4.1 Smoke (single suite, ~30-60 s)

```sh
cd ~/src/personal/linux
tools/testing/selftests/um/soak/run-ltp-smoke.sh smoketest
# Or any single suite name from ltp-suite.txt:
tools/testing/selftests/um/soak/run-ltp-smoke.sh math
tools/testing/selftests/um/soak/run-ltp-smoke.sh nptl
```

Expected first-run output shape (per curation memo §5.3):

```
[run-ltp-smoke] suite=smoketest -> /tmp/ltp-smoke-NNNN/smoketest.log
  total=15 pass=14 fail=0 skip=1 warn=0 (kirk_rc=0)
[run-ltp-smoke] SUMMARY suites=1 pass=14 fail=0 skip=1 warn=0
[run-ltp-smoke] VERDICT=PASS
```

Exit codes:
- `0` = all suites PASS (kirk fail count == 0).
- `1` = at least one suite FAIL.
- `2` = pre-flight broke (missing `/opt/ltp`, `/opt/kirk`, or
  `ltp-skip.txt`).

### 4.2 First full curated cycle (~45 min)

Via the soak daemon, with the `ltp-runner` workload alone:

```sh
UML_KERNEL=~/src/uml-builds/uml-smp-t41fix/linux \
  tools/testing/selftests/um/soak/run-soak-daemon.sh \
    --budget-sec 5400 --workloads ltp-runner \
    --workers 1 --iters-per-rotation 1 \
    --out /var/tmp/phase-J-ltp-first
```

Per-iter timeout for `ltp-runner` is hard-wired at 3600 s in the
daemon's `TIMEOUT_FOR` table (`run-soak-daemon.sh:171`); a single
LTP cycle runs to completion or hits the hard kill at 60 min. The
`--budget-sec 5400` gives the daemon 90 min — one full cycle plus
cooldown plus scoreboard write.

Expected first-cycle output shape (per curation memo §5.3, post
skip-list, clean kvm-v2):

| Counter | Expected range | Notes |
|---------|---------------|-------|
| total   | ~2 050        | After 112-entry skip-list |
| pass    | ~1 950-2 000  | Most of the high-signal mass |
| fail    | 5-50          | The real signal — triage post-cycle |
| skip    | ~50-100       | Runtime CONF (env detection) |
| warn    | ~5-10         | LTP warnings (non-fatal) |

Wall-clock target: 35-50 min on this rig (curation memo §5.1).

### 4.3 What to do if FAIL count > 50

Per curation memo §5.3, classify each FAIL into one of:

- (a) real v2 bug → file an SMP-T<n> issue;
- (b) UML-env mismatch → add to `ltp-skip.txt` with a section
  comment referencing this memo;
- (c) LTP test bug → upstream report.

The exit criterion for "runner integrated" is **FAIL count under 10**
(curation memo §4.4), not zero.

## 5. Where the in-tree pieces live

| File | Contents | Lines |
|------|----------|-------|
| `tools/testing/selftests/um/soak/ltp-runner.toml.template` | kirk-driven Umlfile | 130 |
| `tools/testing/selftests/um/soak/ltp-suite.txt` | 9 KEEP suite names | this memo §6 |
| `tools/testing/selftests/um/soak/ltp-skip.txt` | 112-entry deny-list | curation memo §3 |
| `tools/testing/selftests/um/soak/ltp-triage.txt` | 8 TRIAGE-LATER suites | curation memo §2.1 |
| `tools/testing/selftests/um/soak/run-ltp-smoke.sh` | single-suite probe | this memo §4.1 |
| `tools/testing/selftests/um/soak/run-soak-daemon.sh` | 3600 s timeout for `ltp-runner` | line 171 |
| `tools/testing/selftests/um/soak/README.md` "LTP bootstrap" | duplicates §2 of this memo | line 302 |

The template references `{{SOAK_DIR}}/ltp-suite.txt` and
`{{SOAK_DIR}}/ltp-skip.txt`; the daemon's `SOAK_DIR` substitution
expands to this `selftests/um/soak/` directory.

## 6. Open work the operator should not block on

These do not gate the first cycle but should land before declaring
the runner integrated (curation memo §6 open questions):

- §6.1 — `CONFIG_KEYS=y` in the reference build? If yes, ~15 of the
  20 key-ring entries in `ltp-skip.txt` should be un-skipped and
  graduate. Operator: `grep CONFIG_KEYS= ~/src/uml-builds/.../.config`
  before/after the rebuild in §1.
- §6.2 — kirk JSON schema validation. `run-ltp-smoke.sh` already
  accepts both `results` and `tests` top-level array names; the
  daemon's parser in the template (`ltp-runner.toml.template:104`)
  only handles `tests`. If kirk's actual JSON uses `results`, edit
  the template's phase-3 parser to match — `run-ltp-smoke.sh`'s
  parser is the reference implementation.
- §6.3 — seccomp baseline calibration cycle. Run one ltp-runner
  cycle under `--backends seccomp` only before the first kvm-v2 vs
  seccomp delta is meaningful.
- §6.7 — kirk per-test timeout under T55 perf-regression. Default
  here is `EXEC_TIMEOUT=120` (smoke) and `--exec-timeout 60s` (in
  template); revisit after first calibration cycle.

## 7. References

- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/phase-J-ltp-curation-2026-05-14.md` — design / KEEP / SKIP / TRIAGE.
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/phase-J-design-2026-05-07.md` §4 — LTP curation framework.
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/phase-J-tier3-design-2026-05-14.md` §"CONFIG_UML_NET_VECTOR pre-flight" — same flag, Tier 3 angle.
- `tools/testing/selftests/um/soak/README.md` §"Tier 3 bootstrap" / §"LTP bootstrap" — same commands, shorter form.
- Upstream LTP: `https://github.com/linux-test-project/ltp` HEAD `8cd7644a52e3` (VERSION 20260130).
- Upstream kirk: `https://github.com/linux-test-project/kirk` HEAD `07964a106584`.
