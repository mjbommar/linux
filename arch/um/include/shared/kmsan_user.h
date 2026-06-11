/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_KMSAN_USER_H__
#define __UM_KMSAN_USER_H__

#include <linux/kconfig.h>

#if IS_ENABLED(CONFIG_KMSAN)
#define UM_KMSAN_RETVAL_SIZE 800
#define UM_KMSAN_PARAM_SIZE 800

struct um_kmsan_context_state {
	char param_tls[UM_KMSAN_PARAM_SIZE];
	char retval_tls[UM_KMSAN_RETVAL_SIZE];
	char va_arg_tls[UM_KMSAN_PARAM_SIZE];
	char va_arg_origin_tls[UM_KMSAN_PARAM_SIZE];
	unsigned long long va_arg_overflow_size_tls;
	char param_origin_tls[UM_KMSAN_PARAM_SIZE];
	unsigned int retval_origin_tls;
};

void *__msan_get_context_state(void);

static inline void um_kmsan_clear_context_state(void)
{
	__builtin_memset(__msan_get_context_state(), 0,
			 sizeof(struct um_kmsan_context_state));
}

#undef UM_KMSAN_RETVAL_SIZE
#undef UM_KMSAN_PARAM_SIZE
#else
static inline void um_kmsan_clear_context_state(void)
{
}
#endif

#endif /* __UM_KMSAN_USER_H__ */
