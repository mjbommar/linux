/* Copyright (C) 2005 - 2008 Jeff Dike <jdike@{linux.intel,addtoit}.com> */

/* Much of this ripped from drivers/char/hw_random.c, see there for other
 * copyright.
 *
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/hw_random.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/random.h>		/* GRND_NONBLOCK */
#include <init.h>
#include <os.h>

/*
 * core module information
 */
#define RNG_MODULE_NAME "hw_random"

static struct hwrng hwrng;

/*
 * SMP-T112 (post-2026-05-19 sprint memo 08): pre-sprint this driver
 * opened /dev/random and serialised reads through a SIGIO wakeup +
 * completion.  Post-5.6 mainline /dev/random is "identical to
 * /dev/urandom but with slower init semantics" so the SIGIO dance no
 * longer matches the underlying source.  The cleaner shape is to call
 * the host's getrandom(2) syscall directly: it handles blocking on
 * entropy init internally and never needs SIGIO plumbing.
 *
 * The host-side wrapper os_getrandom() already exists at
 * arch/um/os-Linux/util.c:99; it's the same call the kernel-internal
 * arch_get_random_longs() at arch/um/include/asm/archrandom.h uses.
 */
static int rng_dev_read(struct hwrng *rng, void *buf, size_t max, bool block)
{
	ssize_t ret;
	unsigned int flags = block ? 0 : GRND_NONBLOCK;

	do {
		ret = os_getrandom(buf, max, flags);
	} while (ret == -EINTR);

	/*
	 * hwrng core treats -EAGAIN as "try again later", which is what
	 * getrandom(GRND_NONBLOCK) returns before the host entropy pool is
	 * initialised.  Surface it unchanged so the core's retry semantics
	 * work.
	 */
	return ret < 0 ? (int)ret : (int)ret;
}

/*
 * rng_init - initialize RNG module
 */
static int __init rng_init(void)
{
	int err;

	hwrng.name = RNG_MODULE_NAME;
	hwrng.read = rng_dev_read;

	err = hwrng_register(&hwrng);
	if (err) {
		pr_err(RNG_MODULE_NAME " registering failed (%d)\n", err);
		return err;
	}
	return 0;
}

/*
 * rng_cleanup - shutdown RNG module
 */
static void __exit rng_cleanup(void)
{
	hwrng_unregister(&hwrng);
}

module_init(rng_init);
module_exit(rng_cleanup);

MODULE_DESCRIPTION("UML Host Random Number Generator (RNG) driver");
MODULE_LICENSE("GPL");
