.. SPDX-License-Identifier: GPL-2.0

==============================
UML .text section split
=======================

The UML kernel image splits its executable text into two distinct
regions:

.. list-table::
   :header-rows: 1
   :widths: 25 15 60

   * - Region
     - Access
     - Contents
   * - ``.text`` (the classic region; bounded by ``__start_text_frozen``
       / ``__end_text_frozen``)
     - RX
     - The bulk of kernel code. Mapped read-execute by the host ELF
       loader and not modified at runtime.
   * - ``.um_patch_text`` (bounded by ``__start_um_patch_text`` /
       ``__end_um_patch_text``)
     - RX normally; temporarily RWX during a patch
     - Runtime-patchable code: static-branch NOPs, ftrace mcount
       stubs, and kprobe insertion points. It may be empty when no
       current consumer places code there.

Why two regions
===============

The kernel grows more runtime code-modification features over time
(jump labels, ftrace, kprobes, BPF JIT trampolines). Each needs a
brief writable window around an instruction it's rewriting. Without
a dedicated region, that window would have to expose a page from
the middle of ``.text`` — which on UML means weakening a page that
the host ELF loader had locked RX. The split keeps those writable
windows scoped to a named, bounded region, so every ``mprotect``
call can point at a small page-count range instead of the whole
``.text``.

This is the direct analog of x86's ``__ro_after_init`` +
``.text.*`` separation, adapted to UML's "kernel as a host userspace
ELF" execution model.

How to place a function in ``.um_patch_text``
=============================================

Include ``<asm/patchable.h>`` and decorate the function definition::

  #include <asm/patchable.h>

  void __patchable_function my_future_patched_fn(void)
  {
      /* linker puts this function in .um_patch_text */
      ...
  }

No other source changes are required. The attribute is a layout hint;
the function still behaves like any other kernel function.

mprotect helpers for poke windows
=================================

Two helpers, in ``arch/um/kernel/section_split.c``::

  int um_text_patch_begin(void *addr, unsigned long len);
  int um_text_patch_end(void *addr, unsigned long len);

``_begin`` mprotects the covering pages RWX and takes an internal
serialization lock; ``_end`` restores RX and drops the lock. Both
reject ranges outside ``.um_patch_text`` with ``-EINVAL``, so a
mistaken attempt to poke into frozen text fails early instead of
silently weakening the protection on arbitrary pages.

Typical usage (pseudo-code)::

  err = um_text_patch_begin(jump_site, 5);
  if (err)
      return err;
  memcpy(jump_site, new_insn_bytes, 5);
  barrier();
  um_text_patch_end(jump_site, 5);

Callers must already hold whatever higher-level lock the patching
mechanism requires (``text_mutex``, for instance); the begin/end
lock only serializes concurrent low-level poke windows within this
subsystem.

Boot-time finalization
======================

``mark_rodata_ro()`` (``arch/um/kernel/mem.c``) calls
``um_section_split_finalize()`` after initcalls complete. Today that
function only logs the layout for diagnostics::

  um: section split: .text.frozen 0x600ab000..0x60641000 (5720 KiB),
      .um_patch_text 0x60641000..0x60642000 (4 KiB)

When jump-label patching is enabled, the same call site is the
natural place to apply the initial batch of NOP installs.

Verifying the split
===================

Check that the section exists in the linked image::

  $ readelf -S vmlinux | grep -E '\.text|\.um_patch_text'
  [ 1] .text             PROGBITS         ...  00100000  ...
  [ 2] .um_patch_text    PROGBITS         ...  00700000  ...

Check that the two regions are disjoint and both boundary pairs
are non-negative::

  $ nm vmlinux | grep -E '__(start|end)_(text_frozen|um_patch_text)'
  0000000060641000 D __end_text_frozen
  0000000060642000 D __end_um_patch_text
  00000000600ab000 D __start_text_frozen
  0000000060641000 D __start_um_patch_text

An empty patchable region (``__start_um_patch_text == __end_um_patch_text``)
is expected when no runtime-patching consumer places code there.

Relation to static-key gates
============================

The Layer 2 static-key gates (``arch/um/include/asm/um-hooks.h``) are
the *first* intended consumers of ``.um_patch_text``. Today those
gates compile to the C fallback form (load + compare + predicted
branch) because UML does not yet ``select HAVE_ARCH_JUMP_LABEL``.
The missing piece is the arch glue in ``arch/um/kernel/jump_label.c``,
which will use the mprotect helpers above to enable in-place
NOP-to-JMP transformation at each gate site.

Once that lands, the gates turn into 5-byte NOPs when off (invariant
I3 in the letter, not just the spirit) and ``.um_patch_text`` starts
carrying the table of transformed call sites.
