// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend tracepoint instantiations (memo 25 R7).
 *
 * Single TU that defines CREATE_TRACE_POINTS so the linker emits
 * the tracepoint stubs exactly once. All other call sites just
 * include <asm/trace/um_backend.h> for the trace_um_backend_*
 * function-call wrappers.
 */

#include <linux/mm_types.h>
#include <linux/types.h>
#include <sysdep/ptrace.h>

#define CREATE_TRACE_POINTS
#include <asm/trace/um_backend.h>
