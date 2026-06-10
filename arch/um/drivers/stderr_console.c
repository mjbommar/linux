// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/console.h>

#include "chan_user.h"

/* ----------------------------------------------------------------------------- */
/* trivial console driver -- simply dump everything to stderr                    */

/*
 * Don't register by default -- as this registers very early in the
 * boot process it becomes the default console.
 *
 * Initialized at init time.
 */
static unsigned int use_stderr_console;

static void stderr_console_write(struct console *console, const char *string,
				 unsigned len)
{
	generic_write(2 /* stderr */, string, len, NULL);
}

static struct console stderr_console = {
	.name		= "stderr",
	.write		= stderr_console_write,
	.flags		= CON_PRINTBUFFER,
};

static int __init stderr_console_init(void)
{
	if (use_stderr_console)
		register_console(&stderr_console);
	return 0;
}
console_initcall(stderr_console_init);

static int stderr_setup(char *str)
{
	if (!str)
		return 0;
	if (kstrtouint(str, 0, &use_stderr_console))
		use_stderr_console = 0;
	return 1;
}
__setup("stderr=", stderr_setup);

/*
 * Unregister stderr after the real console is available. Leaving it registered
 * can keep /dev/console from opening; unregistering late preserves early boot
 * output while allowing the normal console to become the default.
 */
static int __init unregister_stderr(void)
{
	unregister_console(&stderr_console);

	return 0;
}

__initcall(unregister_stderr);
