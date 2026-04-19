// SPDX-License-Identifier: GPL-2.0
/*
 * Workstream B-04: UML .text section split — mprotect helpers and
 * init-time finalization.
 *
 * Layout delivered by arch/um/kernel/{uml,dyn}.lds.S:
 *
 *   __start_text_frozen .. __end_text_frozen        — the bulk of
 *       kernel .text; mapped RX by the host ELF loader, stays RX.
 *   __start_um_patch_text .. __end_um_patch_text    — a distinct
 *       page-aligned range holding runtime-patchable code (static
 *       branch call sites, mcount stubs, kprobe insertion points).
 *       Currently empty; workstream C or a follow-up will populate
 *       it via the __patchable_function attribute.
 *
 * UML kernel .text is mmap'd from the ELF binary by the host loader.
 * That loader mprotects the image RX, so no special action is needed
 * to keep .text.frozen read-only. The patch helpers mprotect just the
 * .um_patch_text range RWX for the duration of a poke and restore RX
 * on completion. See Documentation/virt/uml/section-split.rst for the
 * full discussion.
 *
 * Today the only caller of these helpers is future code; their
 * existence makes the path trivial when workstream B-04 follow-ups
 * (jump-label JIT, ftrace patching, kprobes) land.
 */

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <asm/page.h>
#include <asm/patchable.h>
#include <os.h>

/* A single global lock — patch operations are rare and always
 * system-serialized in practice. If a future workstream needs finer
 * granularity (e.g. per-call-site batching), it can replace this
 * with text_mutex or a similar mechanism from the generic kernel.
 */
static DEFINE_SPINLOCK(um_patch_lock);

static bool addr_in_patchable(const void *addr)
{
	return (const char *)addr >= __start_um_patch_text &&
	       (const char *)addr <  __end_um_patch_text;
}

static bool range_in_patchable(const void *addr, unsigned long len)
{
	const char *start = addr;
	const char *end   = start + len;

	if (!len)
		return true;
	if (end < start)
		return false; /* overflow */
	return start >= __start_um_patch_text &&
	       end   <= __end_um_patch_text;
}

/* Kernel text spans [_text, _etext) per arch/um/kernel/uml.lds.S and
 * arch/um/include/asm/common.lds.S. This includes .init.text (which
 * lives before _stext and has its own patchable sites during the
 * window between ftrace_init() and free_initmem()), the main .text,
 * .um_patch_text, .syscall_stub, and .fini. Patching callers
 * (ftrace, future kprobes) target compiled function bodies anywhere
 * in that range; the at-most-two-page invariant below is the real
 * safeguard against wild writes.
 */
extern char _text[], _etext[];

static bool range_in_kernel_text(const void *addr, unsigned long len)
{
	const char *start = addr;
	const char *end   = start + len;

	if (!len)
		return true;
	if (end < start)
		return false; /* overflow */
	return start >= _text && end <= _etext;
}

/* A 5-byte ftrace patch can straddle a page boundary (compiler can
 * place the 5-byte NOP sequence right at a page edge). We allow the
 * RW window to cover at most two adjacent pages; longer ranges are
 * rejected to keep the blast radius bounded.
 */
static unsigned long kernel_text_pages(const void *addr, unsigned long len)
{
	unsigned long start_page, last_page;

	if (!len)
		return 0;
	start_page = (unsigned long)addr & ~(PAGE_SIZE - 1UL);
	last_page  = ((unsigned long)addr + len - 1UL) & ~(PAGE_SIZE - 1UL);
	return ((last_page - start_page) >> PAGE_SHIFT) + 1;
}

static unsigned long page_floor(unsigned long v)
{
	return v & ~(PAGE_SIZE - 1UL);
}

static unsigned long page_round_up(unsigned long v)
{
	return (v + PAGE_SIZE - 1UL) & ~(PAGE_SIZE - 1UL);
}

int um_text_patch_begin(void *addr, unsigned long len)
{
	unsigned long start, end, aligned_len;
	int err;

	if (!range_in_patchable(addr, len)) {
		pr_err_once("um: patch_begin: range %p+%lu not in .um_patch_text\n",
			    addr, len);
		return -EINVAL;
	}

	start = page_floor((unsigned long)addr);
	end   = page_round_up((unsigned long)addr + len);
	aligned_len = end - start;

	spin_lock(&um_patch_lock);
	/* r=1, w=1, x=1 — RWX only for the duration of the poke. */
	err = os_protect_memory((void *)start, aligned_len, 1, 1, 1);
	if (err)
		spin_unlock(&um_patch_lock);
	return err;
}

int um_text_patch_end(void *addr, unsigned long len)
{
	unsigned long start, end, aligned_len;
	int err;

	if (!range_in_patchable(addr, len))
		return -EINVAL;

	start = page_floor((unsigned long)addr);
	end   = page_round_up((unsigned long)addr + len);
	aligned_len = end - start;

	/* Back to RX. Pair with um_text_patch_begin(). */
	err = os_protect_memory((void *)start, aligned_len, 1, 0, 1);
	spin_unlock(&um_patch_lock);
	return err;
}

int um_kernel_text_patch_begin(void *addr, unsigned long len)
{
	unsigned long page_start, pages;
	int err;

	if (!range_in_kernel_text(addr, len)) {
		pr_err_once("um: kernel_text_patch_begin: range %p+%lu outside [_text, _etext)\n",
			    addr, len);
		return -EINVAL;
	}

	pages = kernel_text_pages(addr, len);
	if (pages > 2) {
		pr_err_once("um: kernel_text_patch_begin: range %p+%lu spans %lu pages (max 2)\n",
			    addr, len, pages);
		return -EINVAL;
	}

	page_start = (unsigned long)addr & ~(PAGE_SIZE - 1UL);

	spin_lock(&um_patch_lock);
	err = os_protect_memory((void *)page_start, pages * PAGE_SIZE,
				1, 1, 1);
	if (err)
		spin_unlock(&um_patch_lock);
	return err;
}

int um_kernel_text_patch_end(void *addr, unsigned long len)
{
	unsigned long page_start, pages;
	int err;

	if (!range_in_kernel_text(addr, len))
		return -EINVAL;

	pages = kernel_text_pages(addr, len);
	if (pages > 2)
		return -EINVAL;

	page_start = (unsigned long)addr & ~(PAGE_SIZE - 1UL);

	err = os_protect_memory((void *)page_start, pages * PAGE_SIZE,
				1, 0, 1);
	spin_unlock(&um_patch_lock);
	return err;
}

void um_section_split_finalize(void)
{
	unsigned long frozen_bytes =
		(unsigned long)__end_text_frozen -
		(unsigned long)__start_text_frozen;
	unsigned long patch_bytes =
		(unsigned long)__end_um_patch_text -
		(unsigned long)__start_um_patch_text;

	pr_info("um: section split: .text.frozen %p..%p (%lu KiB), .um_patch_text %p..%p (%lu KiB)\n",
		__start_text_frozen, __end_text_frozen,
		frozen_bytes >> 10,
		__start_um_patch_text, __end_um_patch_text,
		patch_bytes >> 10);
}

/* Silence -Wunused on a validator helper kept for future use. */
static __maybe_unused bool um_addr_in_patchable(const void *addr)
{
	return addr_in_patchable(addr);
}
