# UML KMSAN Runtime Alignment And Blocker

Date: 2026-06-11

## Objective

Close the first concrete KMSAN blocker on `next` without importing speculative
generic sanitizer changes. The immediate target was the early runtime panic in
the UML KMSAN smoke path, then a clean decision about what remains before KMSAN
can count as complete.

## Code Change

The landed code change is intentionally UML-local:

- `arch/um/include/asm/pgtable.h` now rounds `VMALLOC_QUARTER_SIZE` down with
  `PAGE_MASK`.
- That quarter size feeds `KMSAN_VMALLOC_SHADOW_START` and
  `KMSAN_VMALLOC_ORIGIN_START`.
- Generic KMSAN maps vmalloc shadow and origin metadata through page arrays, so
  those derived metadata starts must be page-aligned.

Before this change, the quarter size could be subpage-aligned. The observed
diagnostic values put the KMSAN shadow/origin starts at subpage offsets, and the
runtime failed very early in the generic vmap path:

```text
Starting KernelMemorySanitizer
WARNING: mm/vmalloc.c:554 at __vmap_pages_range_noflush...
vmalloc error: size 16384, failed to map pages
Kernel panic - not syncing: Segfault with no mm
```

## Validation

The main checkout could not be used directly for the full build because it has
in-tree generated state. The tested build used a detached clean worktree and a
separate output directory:

```bash
git worktree add --detach "$src" HEAD
git diff -- arch/um/include/asm/pgtable.h arch/um/kernel/process.c | \
	git -C "$src" apply -
make -C "$src" ARCH=um LLVM=1 O="$out" uml/research-kmsan
make -C "$src" ARCH=um LLVM=1 O="$out" -j"$(nproc)" linux
```

After the unlanded `process.c` experiment was removed from the main tree, the
clean test source was synced back to the exact one-file pgtable change and
rebuilt:

```bash
git -C "$src" diff -- arch/um/kernel/process.c | git -C "$src" apply -R -
make -C "$src" ARCH=um LLVM=1 O="$out" -j"$(nproc)" linux
```

Build result:

- `CONFIG_HAVE_ARCH_KMSAN=y`
- `CONFIG_KMSAN=y`
- `CONFIG_KMSAN_CHECK_PARAM_RETVAL=y`
- build passed
- expected KMSAN frame-size warnings appeared in generic instrumented code
- the final link emitted the expected UML RWX segment warning

Focused smoke command:

```bash
UML_BINARY="$out/linux" UML_MEM=2048M \
	tools/testing/selftests/um/kmsan-smoke/run-kmsan-smoke.sh \
	> /tmp/uml-kmsan-wrapper-exact.out 2>&1
```

Result:

```text
wrapper_rc=1
FAIL: no KMSAN_SMOKE PASS/FAIL/SKIP line in output
BUG: KMSAN: uninit-value in save_stack_trace+0x51/0x80
```

Bounded earlyprintk command:

```bash
timeout --kill-after=10 30 "$out/linux" \
	init=tools/testing/selftests/um/kmsan-smoke/kmsan-smoke.sh mem=2048M \
	earlyprintk stderr=1 con=null con1=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw panic=0 loglevel=8 \
	> /tmp/uml-kmsan-next-exact-earlyprintk-30.log 2>&1
```

Result:

```text
early_rc=134
14963 lines
476359 bytes
```

Important observations from the exact one-file run:

- The guest reached `VFS: Finished mounting rootfs on nullfs`.
- No `vmalloc error` was present.
- No `__vmap_pages_range_noflush` warning was present.
- The first KMSAN report was:

```text
BUG: KMSAN: uninit-value in save_stack_trace+0x51/0x80
 save_stack_trace+0x51/0x80
 stack_trace_save+0xd0/0x110
 kmsan_report+0x183/0x410
 __msan_warning+0x27/0x30
 sized_strscpy+0xe2/0x230
 copy_process+0x1388/0x74d0
 kernel_clone+0x407/0x10b0
 kernel_thread+0x1f1/0x220
 kthreadd+0x424/0x910
 new_thread_handler+0x137/0x240
```

The origin chain begins in a kthread-name allocation path:

```text
kmsan_slab_alloc+0xd2/0x230
__kmalloc_node_track_caller_noprof+0x511/0x11b0
kvasprintf+0xf8/0x360
__kthread_create_on_node+0x1eb/0x720
kthread_create_worker_on_node+0x302/0x680
wq_cpu_intensive_thresh_init+0x4c/0x3e4
workqueue_init+0x2d/0x725
```

Later reports include scheduler and credential setup paths such as
`prepare_creds`, `wake_up_new_task`, `post_init_entity_util_avg`, and
`cgroup_css_set_put_fork`.

## Result

The UML-specific vmalloc metadata alignment blocker is closed. The previous
early vmap metadata failure is absent from the focused log, and a clean
`research-kmsan` LLVM kernel build succeeds.

KMSAN is not complete yet. The runtime smoke still fails before it can print the
`KMSAN_SMOKE` marker, so the KMSAN row in the live inventory remains an open
runtime-fix item.

## Non-Landed Experiments

Two experiments were deliberately not landed:

- `arch/um/kernel/process.c`: unpoisoning the dynamic task-struct allocation was
  plausible, but the exact smoke run is blocked earlier and did not prove this
  annotation.
- `lib/kasprintf.c`: unpoisoning the `kvasprintf()` output advanced the first
  failure in an earlier experiment, but that is a generic KMSAN behavior change
  and did not make the smoke pass.

Both are investigation leads, not accepted code.

## Next Steps

1. Reproduce the first current report with a smaller early-boot trace around
   `kthread_create_worker_on_node()` and `copy_process()`.
2. Decide whether the kthread-name allocation should be initialized in generic
   code, by a narrower KMSAN helper, or by a UML-specific arch boundary.
3. Re-test after the first report is closed and continue through the follow-on
   scheduler and credential reports.
4. Require `kmsan-smoke` to reach a `KMSAN_SMOKE` result marker before changing
   the inventory status from `Present-needs-runtime-fix`.
5. Keep any generic KMSAN changes separate from the UML-local vmalloc alignment
   fix unless the evidence proves they are independently correct upstream.
