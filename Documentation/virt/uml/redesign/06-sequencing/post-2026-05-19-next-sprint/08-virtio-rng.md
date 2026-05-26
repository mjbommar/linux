# 08 — virtio-rng modernisation

**Sprint:** post-2026-05-19
**Priority:** LOW
**Effort:** small (~50–150 LoC across `arch/um/drivers/random.c`,
the existing `arch/um/drivers/virtio_uml.c` infrastructure, and
`tools/uml/uml-launcher/`)
**Status:** Phase 1 DONE 2026-05-19 (`76c428c95d8e` — random.c now
calls os_getrandom() directly, dropping the /dev/random fd +
SIGIO loop).  Phase 2 (Rust vhost-user-rng backend) and Phase 3
(`umlctl` selection plumbing) deferred — Phase 1 already
delivers the entropy-availability fix the memo set out to land;
Phase 2's only added benefit is sandbox isolation (running rng
in a separate process), which is a follow-on architectural
choice rather than a sprint-blocking gap.
**Depends on:** none (`virtio_uml` infrastructure already in tree).

## Why this matters

Two related modernisations:

1. **Host entropy source.** Today's `arch/um/drivers/random.c`
   reads `/dev/random` and signals on SIGIO. Post-5.6 mainline
   `/dev/random` is "identical to `/dev/urandom` but with slower
   init semantics" — the historical "wait for entropy" behaviour
   no longer matches the underlying kernel. The right modern
   replacement is `getrandom()`.

2. **No virtio-rng device.** `arch/um/drivers/virtio_uml.c` is in
   tree (1545 LoC, vhost-user backend infrastructure). It exposes
   virtio devices to the UML guest via host-side daemons in
   `tools/uml/uml-launcher/`. But there's no virtio-rng device
   wired up — so a guest that's already running with
   `vector2 + virtio_uml + ...` still uses the legacy
   non-virtio random device.

Cheap, no architectural blockers, modernises the entropy story
without touching the rest of the random subsystem.

## Current state

| Surface | Path | State |
|---------|------|-------|
| UML legacy random driver | `arch/um/drivers/random.c` | reads `/dev/random`, SIGIO-driven |
| Host getrandom() syscall | host kernel | available everywhere UML runs |
| `virtio_uml` infrastructure | `arch/um/drivers/virtio_uml.c` | vhost-user infrastructure live |
| Host-side virtio-rng backend | `tools/uml/uml-launcher/src/virtio.rs` | virtio-blk and virtio-net backends exist; rng missing |
| `umlctl` virtio-rng selection | `tools/uml/uml-launcher/src/bin/umlctl/deploy.rs` | not exposed |

## Proposed change

### Phase 1 — Replace `/dev/random` read with `getrandom()`

```c
/* arch/um/drivers/random.c (paraphrased) */
static ssize_t random_read(struct file *file, char __user *buf,
                            size_t count, loff_t *ppos)
{
    /* OLD:
     *   read from random_fd (opened on /dev/random)
     *   wait for SIGIO if EAGAIN
     */

    /* NEW: */
    int err;
    do {
        err = getrandom(buf, count, GRND_NONBLOCK);
    } while (err < 0 && errno == EINTR);

    if (err < 0)
        return -errno;
    return err;
}
```

Behaviour:

- `getrandom(GRND_NONBLOCK)` returns immediately. No SIGIO
  plumbing needed; remove the random-driver IRQ-on-readable
  shim.
- The host kernel handles all the "block until entropy ready"
  semantics internally; UML just asks for bytes.

Removes `random_fd` and the SIGIO handler. Net diff: ~80 LoC
removal + ~30 LoC addition.

### Phase 2 — Add virtio-rng host backend

Mirror `tools/uml/uml-launcher/src/bin/uml-vhost-virtio-blk-backend/`
to create `tools/uml/uml-launcher/src/bin/uml-vhost-virtio-rng-backend/`:

```rust
// uml-vhost-virtio-rng-backend/main.rs
fn main() {
    let opts = parse_args();
    let socket = vhost_user::bind(&opts.socket_path).unwrap();
    let mut backend = VirtioRngBackend::new();
    loop {
        let req = socket.recv_request().unwrap();
        match req.op {
            VirtioRngOp::GetRandom { len } => {
                let mut buf = vec![0u8; len];
                getrandom::getrandom(&mut buf).unwrap();
                socket.send_response(VirtioRngResp { data: buf }).unwrap();
            }
        }
    }
}
```

### Phase 3 — `umlctl` virtio-rng selection

```toml
[virtio]
rng = "host-getrandom"   # one of "host-getrandom", "off"
```

Default: `"host-getrandom"` when `[virtio]` block is present at
all; legacy `arch/um/drivers/random.c` path otherwise.

## Effort breakdown

- Phase 1 (`getrandom` replacement): ~30 LoC + ~80 LoC of
  removal. Net negative.
- Phase 2 (virtio-rng backend in Rust): ~150 LoC.
- Phase 3 (`umlctl` schema + plumbing): ~30 LoC.

Total: ~210 LoC, with a net-negative C diff.

## Acceptance criteria

- **Functional gate:** `dd if=/dev/random bs=1 count=64 |
  hexdump | head -5` produces output unchanged in shape.
- **Behavioural gate:** boot-time entropy availability matches
  baseline (no boot stall waiting for entropy under `getrandom`
  semantics).
- **virtio-rng gate (Phase 2/3):** `umlctl up` with
  `[virtio].rng = "host-getrandom"` boots cleanly; guest's
  `/sys/class/misc/hw_random/rng_available` shows the
  virtio-rng device.

## Dependencies

- **Code:** none for Phase 1; `virtio_uml.c` (in tree) for
  Phase 2.
- **Host kernel:** `getrandom` requires ≥ 3.17 (universal).
- **Memo:** none.

## Risk notes

- **`getrandom(GRND_NONBLOCK)` blocking semantics.** Despite
  the name, returns `EAGAIN` at boot when the kernel entropy
  pool isn't initialised. Mitigation: drop the `GRND_NONBLOCK`
  flag if Phase 1 testing shows boot-time entropy starvation.
- **virtio-rng is a security-sensitive surface.** Exposing
  host entropy via vhost-user is fine, but a malicious guest
  could spam `GetRandom` requests. Mitigation: rate-limit on
  the host side (token bucket, 1 MiB/s default).
- **Rollback:** Phase 1 is a single-file revert. Phase 2/3 are
  opt-in via TOML; no rollback impact for users who don't
  enable.

## Cross-references

- Sub-agent's investigation: 2026-05-19 report (item #7).
- `arch/um/drivers/virtio_uml.c` (1545 LoC): the vhost-user
  infrastructure that Phase 2 plugs into.
- `tools/uml/uml-launcher/src/virtio.rs`: the host-side virtio
  orchestration code (where Phase 2's backend registers).
