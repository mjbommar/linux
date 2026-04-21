// SPDX-License-Identifier: GPL-2.0
/*
 * UML-local stubs for x86 symbols that arch/x86/net/bpf_jit_comp.c
 * references at link time but that live in arch/x86/kernel/ TUs
 * UML doesn't compile (alternative.c, cfi.c, retpoline.S, etc.).
 *
 * Each runtime path that would reach these stubs is gated by a
 * `cpu_feature_enabled(X86_FEATURE_*)` check that returns false
 * on UML (no retpoline, no ITS/BHB mitigations, no CFI in the
 * UML kernel's BPF JIT output). The stubs exist solely to satisfy
 * the linker; they should never be called at runtime. If any of
 * them runs, it means a cpu_feature gate is misconfigured and we
 * want to know about it immediately (hence the panic rather than
 * silent empty body).
 *
 * Workstream C-06 per Documentation/virt/uml/redesign/
 * 04-risks/decisions-log.md D43 fifth-view.
 */

#include <linux/export.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/bug.h>
#include <asm/cfi.h>

/*
 * Forward declarations matching the upstream x86 prototypes so
 * -Wmissing-prototypes stays clean under sparse / smatch / gcc.
 * The real prototypes live in:
 *   arch/x86/include/asm/nospec-branch.h  (clear_bhb_loop)
 *   arch/x86/include/asm/text-patching.h  (text_poke_set,
 *                                          smp_text_poke_single)
 * but those headers transitively pull in declarations UML's
 * include path doesn't fully assemble (mm_struct under
 * __ro_after_init, etc.), so we restate the signatures here.
 * A mismatch with upstream would be a caller-side build error;
 * the link-time resolution is the primary guardrail.
 */
extern void clear_bhb_loop(void);
extern void *text_poke_set(void *addr, int c, size_t len);
extern void smp_text_poke_single(void *addr, const void *opcode,
				  size_t len, const void *emulate);

/*
 * Retpoline indirect-thunk arrays. The BPF JIT's emit_rsb_call /
 * emit_indirect_jump paths take the address of these under the
 * X86_FEATURE_RETPOLINE / INDIRECT_THUNK_ITS cpu_feature gate.
 * On UML those features are never set in boot_cpu_data; the
 * paths are dead. Declare as ASM-style labeled arrays so the
 * compiler can take their address.
 */
char __x86_indirect_thunk_array[1];
EXPORT_SYMBOL_GPL(__x86_indirect_thunk_array);
char __x86_indirect_jump_thunk_array[1];
EXPORT_SYMBOL_GPL(__x86_indirect_jump_thunk_array);

/*
 * CFI mode. emit_cfi in bpf_jit_comp.c:479 switches on this.
 * UML builds with CFI off (for now); CFI_OFF leads the emitter
 * to skip all CFI prologue bytes.
 */
enum cfi_mode cfi_mode = CFI_OFF;
EXPORT_SYMBOL_GPL(cfi_mode);

/*
 * BHB loop clearing. Emitted as a call from the BPF program
 * prologue when X86_FEATURE_CLEAR_BHB_LOOP is enabled. UML
 * never sets that feature; the emission site is dead. If hit,
 * it's a bug elsewhere (a cpu_feature gate that shouldn't have
 * fired).
 */
void clear_bhb_loop(void)
{
	panic("UML: clear_bhb_loop reached — BHB mitigation should be "
	      "inert on UML. Check cpu_feature_enabled gates.");
}
EXPORT_SYMBOL_GPL(clear_bhb_loop);

/*
 * text_poke_set is called by bpf_arch_text_invalidate to
 * overwrite a JIT image with 0xcc (int3) bytes on unload. On
 * UML the JIT image lives in execmem-allocated memory which is
 * plain RWX host memory; memset is sufficient and semantically
 * correct. No host icache flush needed — self-modifying code on
 * the same CPU is coherent without explicit sync on x86.
 */
void *text_poke_set(void *addr, int c, size_t len)
{
	return memset(addr, c, len);
}
EXPORT_SYMBOL_GPL(text_poke_set);

/*
 * smp_text_poke_single is called by __bpf_arch_text_poke to
 * atomically rewrite a 5-byte CALL/JMP with cross-CPU sync.
 * UML is single-host-process (no SMP at the host-kernel layer),
 * so a plain memcpy is sufficient on x86 self-modifying-code
 * coherency rules.
 */
void smp_text_poke_single(void *addr, const void *opcode, size_t len,
			  const void *emulate)
{
	memcpy(addr, opcode, len);
}
EXPORT_SYMBOL_GPL(smp_text_poke_single);

/*
 * text_poke_copy is already provided by arch/um/kernel/um_arch.c:502
 * (workstream B-04's patching infrastructure). Do not redefine
 * here — the linker will reject a duplicate symbol. Keep this
 * comment so future editors don't add it back.
 */
