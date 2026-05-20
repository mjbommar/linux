// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Cambridge Greys Ltd
 * Copyright (C) 2015-2016 Anton Ivanov (aivanov@brocade.com)
 * Copyright (C) 2000 Jeff Dike (jdike@karaya.com)
 */

/* 2001-09-28...2002-04-17
 * Partition stuff by James_McMechan@hotmail.com
 * old style ubd by setting UBD_SHIFT to 0
 * 2002-09-27...2002-10-18 massive tinkering for 2.5
 * partitions have changed in 2.5
 * 2003-01-29 more tinkering for 2.5.59-1
 * This should now address the sysfs problems and has
 * the symlink for devfs to allow for booting with
 * the common /dev/ubd/discX/... names rather than
 * only /dev/ubdN/discN this version also has lots of
 * clean ups preparing for ubd-many.
 * James McMechan
 */

#define UBD_SHIFT 4

#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/ata.h>
#include <linux/hdreg.h>
#include <linux/major.h>
#include <linux/cdrom.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/ctype.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/uio.h>
#include <kern_util.h>
#include "mconsole_kern.h"
#include <init.h>
#include <irq_kern.h>
#include "ubd.h"
#include <os.h>
#include <os_io_ring.h>
#include "cow.h"

/* Max request size is determined by sector mask - 32K */
#define UBD_MAX_REQUEST (8 * sizeof(long))

struct io_desc {
	char *buffer;
	unsigned long length;
	unsigned long sector_mask;
	unsigned long long cow_offset;
	unsigned long bitmap_words[2];
};

struct io_thread_req {
	struct request *req;
	int fds[2];
	unsigned long offsets[2];
	unsigned long long offset;
	int sectorsize;
	int error;

	int desc_cnt;
	/* io_desc has to be the last element of the struct */
	struct io_desc io_desc[];
};


static struct io_thread_req **irq_req_buffer;
static struct io_thread_req *irq_remainder;
static int irq_remainder_size;

static struct io_thread_req **io_req_buffer;
static struct io_thread_req *io_remainder;
static int io_remainder_size;

/*
 * Phase 2a of memo #2 (UBD io_uring) — declarations hoisted here so
 * ubd_driver_init can initialise the ring (forward reference resolved
 * at file scope).  Definitions and the do_io_ring() body live next to
 * the legacy do_io() further down.
 */
#define UBD_RING_DEPTH 256
static struct os_io_ring *ubd_ring;

/*
 * Memo #2 perf-comparison knob.  Set via cmdline `um_ubd_no_uring=1`
 * to force the legacy synchronous helper-thread path even when the
 * host kernel could support io_uring.  Used by the A/B bench in
 * tools/testing/selftests/um/ubd-bench/.  Default 0 keeps the
 * io_uring path enabled.
 *
 * Note: name starts with `um_ubd_` rather than `ubd_` because
 * __setup("ubd", ubd_setup) above does a prefix match and would
 * otherwise capture this and try to parse it as a device spec.
 */
static int ubd_no_io_uring;
static int __init ubd_no_io_uring_setup(char *s)
{
	ubd_no_io_uring = simple_strtol(s, NULL, 0);
	return 1;
}
__setup("um_ubd_no_uring=", ubd_no_io_uring_setup);



static inline int ubd_test_bit(__u64 bit, unsigned char *data)
{
	__u64 n;
	int bits, off;

	bits = sizeof(data[0]) * 8;
	n = bit / bits;
	off = bit % bits;
	return (data[n] & (1 << off)) != 0;
}

static inline void ubd_set_bit(__u64 bit, unsigned char *data)
{
	__u64 n;
	int bits, off;

	bits = sizeof(data[0]) * 8;
	n = bit / bits;
	off = bit % bits;
	data[n] |= (1 << off);
}
/*End stuff from ubd_user.h*/

#define DRIVER_NAME "uml-blkdev"

static DEFINE_MUTEX(ubd_lock);

static int ubd_ioctl(struct block_device *bdev, blk_mode_t mode,
		     unsigned int cmd, unsigned long arg);
static int ubd_getgeo(struct gendisk *disk, struct hd_geometry *geo);

#define MAX_DEV (16)

static const struct block_device_operations ubd_blops = {
        .owner		= THIS_MODULE,
        .ioctl		= ubd_ioctl,
        .compat_ioctl	= blkdev_compat_ptr_ioctl,
	.getgeo		= ubd_getgeo,
};

#ifdef CONFIG_BLK_DEV_UBD_SYNC
#define OPEN_FLAGS ((struct openflags) { .r = 1, .w = 1, .s = 1, .c = 0, \
					 .cl = 1 })
#else
#define OPEN_FLAGS ((struct openflags) { .r = 1, .w = 1, .s = 0, .c = 0, \
					 .cl = 1 })
#endif
static struct openflags global_openflags = OPEN_FLAGS;

struct cow {
	/* backing file name */
	char *file;
	/* backing file fd */
	int fd;
	unsigned long *bitmap;
	unsigned long bitmap_len;
	int bitmap_offset;
	int data_offset;
};

#define MAX_SG 64

struct ubd {
	/* name (and fd, below) of the file opened for writing, either the
	 * backing or the cow file. */
	char *file;
	char *serial;
	int fd;
	__u64 size;
	struct openflags boot_openflags;
	struct openflags openflags;
	unsigned shared:1;
	unsigned no_cow:1;
	unsigned no_trim:1;
	struct cow cow;
	struct platform_device pdev;
	struct gendisk *disk;
	struct blk_mq_tag_set tag_set;
	spinlock_t lock;
};

#define DEFAULT_COW { \
	.file =			NULL, \
	.fd =			-1,	\
	.bitmap =		NULL, \
	.bitmap_offset =	0, \
	.data_offset =		0, \
}

#define DEFAULT_UBD { \
	.file = 		NULL, \
	.serial =		NULL, \
	.fd =			-1, \
	.size =			-1, \
	.boot_openflags =	OPEN_FLAGS, \
	.openflags =		OPEN_FLAGS, \
	.no_cow =               0, \
	.no_trim =		0, \
	.shared =		0, \
	.cow =			DEFAULT_COW, \
	.lock =			__SPIN_LOCK_UNLOCKED(ubd_devs.lock), \
}

/* Protected by ubd_lock */
static struct ubd ubd_devs[MAX_DEV] = { [0 ... MAX_DEV - 1] = DEFAULT_UBD };

static blk_status_t ubd_queue_rq(struct blk_mq_hw_ctx *hctx,
				 const struct blk_mq_queue_data *bd);

static int fake_ide_setup(char *str)
{
	pr_warn("The fake_ide option has been removed\n");
	return 1;
}
__setup("fake_ide", fake_ide_setup);

__uml_help(fake_ide_setup,
"fake_ide\n"
"    Obsolete stub.\n\n"
);

static int parse_unit(char **ptr)
{
	char *str = *ptr, *end;
	int n = -1;

	if(isdigit(*str)) {
		n = simple_strtoul(str, &end, 0);
		if(end == str)
			return -1;
		*ptr = end;
	}
	else if (('a' <= *str) && (*str <= 'z')) {
		n = *str - 'a';
		str++;
		*ptr = str;
	}
	return n;
}

/* If *index_out == -1 at exit, the passed option was a general one;
 * otherwise, the str pointer is used (and owned) inside ubd_devs array, so it
 * should not be freed on exit.
 */
static int ubd_setup_common(char *str, int *index_out, char **error_out)
{
	struct ubd *ubd_dev;
	struct openflags flags = global_openflags;
	char *file, *backing_file, *serial;
	int n, err = 0, i;

	if(index_out) *index_out = -1;
	n = *str;
	if(n == '='){
		str++;
		if(!strcmp(str, "sync")){
			global_openflags = of_sync(global_openflags);
			return err;
		}

		pr_warn("fake major not supported any more\n");
		return 0;
	}

	n = parse_unit(&str);
	if(n < 0){
		*error_out = "Couldn't parse device number";
		return -EINVAL;
	}
	if(n >= MAX_DEV){
		*error_out = "Device number out of range";
		return 1;
	}

	err = -EBUSY;
	mutex_lock(&ubd_lock);

	ubd_dev = &ubd_devs[n];
	if(ubd_dev->file != NULL){
		*error_out = "Device is already configured";
		goto out;
	}

	if (index_out)
		*index_out = n;

	err = -EINVAL;
	for (i = 0; i < sizeof("rscdtD="); i++) {
		switch (*str) {
		case 'r':
			flags.w = 0;
			break;
		case 's':
			flags.s = 1;
			break;
		case 'd':
			ubd_dev->no_cow = 1;
			break;
		case 'c':
			ubd_dev->shared = 1;
			break;
		case 't':
			ubd_dev->no_trim = 1;
			break;
		case 'D':
			/*
			 * memo #2 Phase 4: O_DIRECT on the backing file.
			 * Skips the host page cache so io_uring's queue
			 * depth (UBD_RING_DEPTH = 256) actually shows
			 * up as a measured win — buffered I/O lets the
			 * host pagecache absorb writes and mask the
			 * ring's advantage over the legacy depth-1
			 * helper thread.
			 *
			 * Requires page-aligned bvecs (the block layer
			 * already guarantees this for whole-page bios;
			 * sub-page sectored I/O via COW sector_mask
			 * could still trip O_DIRECT alignment on some
			 * host filesystems).
			 */
			flags.dr = 1;
			break;
		case '=':
			str++;
			goto break_loop;
		default:
			*error_out = "Expected '=' or flag letter "
				"(r, s, c, t, D, or d)";
			goto out;
		}
		str++;
	}

	if (*str == '=')
		*error_out = "Too many flags specified";
	else
		*error_out = "Missing '='";
	goto out;

break_loop:
	file = strsep(&str, ",:");
	if (*file == '\0')
		file = NULL;

	backing_file = strsep(&str, ",:");
	if (backing_file && *backing_file == '\0')
		backing_file = NULL;

	serial = strsep(&str, ",:");
	if (serial && *serial == '\0')
		serial = NULL;

	if (backing_file && ubd_dev->no_cow) {
		*error_out = "Can't specify both 'd' and a cow file";
		goto out;
	}

	err = 0;
	ubd_dev->file = file;
	ubd_dev->cow.file = backing_file;
	ubd_dev->serial = serial;
	ubd_dev->boot_openflags = flags;
out:
	mutex_unlock(&ubd_lock);
	return err;
}

static int ubd_setup(char *str)
{
	char *error;
	int err;

	err = ubd_setup_common(str, NULL, &error);
	if(err)
		printk(KERN_ERR "Failed to initialize device with \"%s\" : "
		       "%s\n", str, error);
	return 1;
}

__setup("ubd", ubd_setup);
__uml_help(ubd_setup,
"ubd<n><flags>=<filename>[(:|,)<filename2>][(:|,)<serial>]\n"
"    This is used to associate a device with a file in the underlying\n"
"    filesystem. When specifying two filenames, the first one is the\n"
"    COW name and the second is the backing file name. As separator you can\n"
"    use either a ':' or a ',': the first one allows writing things like;\n"
"	ubd0=~/Uml/root_cow:~/Uml/root_backing_file\n"
"    while with a ',' the shell would not expand the 2nd '~'.\n"
"    When using only one filename, UML will detect whether to treat it like\n"
"    a COW file or a backing file. To override this detection, add the 'd'\n"
"    flag:\n"
"	ubd0d=BackingFile\n"
"    Usually, there is a filesystem in the file, but \n"
"    that's not required. Swap devices containing swap files can be\n"
"    specified like this. Also, a file which doesn't contain a\n"
"    filesystem can have its contents read in the virtual \n"
"    machine by running 'dd' on the device. <n> must be in the range\n"
"    0 to 7. Appending an 'r' to the number will cause that device\n"
"    to be mounted read-only. For example ubd1r=./ext_fs. Appending\n"
"    an 's' will cause data to be written to disk on the host immediately.\n"
"    'c' will cause the device to be treated as being shared between multiple\n"
"    UMLs and file locking will be turned off - this is appropriate for a\n"
"    cluster filesystem and inappropriate at almost all other times.\n\n"
"    't' will disable trim/discard support on the device (enabled by default).\n\n"
"    'D' will open the backing file with O_DIRECT, bypassing the host page\n"
"    cache. Pairs naturally with the io_uring submission path (memo #2);\n"
"    the host's queue depth then drives throughput rather than the page\n"
"    cache. Requires page-aligned bvecs (default for whole-page block I/O\n"
"    on Linux); sub-page sectored I/O via COW may trip O_DIRECT alignment\n"
"    on some host filesystems.\n\n"
"    An optional device serial number can be exposed using the serial parameter\n"
"    on the cmdline which is exposed as a sysfs entry. This is particularly\n"
"    useful when a unique number should be given to the device. Note when\n"
"    specifying a label, the filename2 must be also presented. It can be\n"
"    an empty string, in which case the backing file is not used:\n"
"       ubd0=File,,Serial\n\n"
);

static int udb_setup(char *str)
{
	printk("udb%s specified on command line is almost certainly a ubd -> "
	       "udb TYPO\n", str);
	return 1;
}

__setup("udb", udb_setup);
__uml_help(udb_setup,
"udb\n"
"    This option is here solely to catch ubd -> udb typos, which can be\n"
"    to impossible to catch visually unless you specifically look for\n"
"    them.  The only result of any option starting with 'udb' is an error\n"
"    in the boot output.\n\n"
);

/* Only changed by ubd_init, which is an initcall. */
static int thread_fd = -1;

/* Function to read several request pointers at a time
* handling fractional reads if (and as) needed
*/

static int bulk_req_safe_read(
	int fd,
	struct io_thread_req **request_buffer,
	struct io_thread_req **remainder,
	int *remainder_size,
	int max_recs
	)
{
	int n = 0;
	int res = 0;

	if (*remainder_size > 0) {
		memmove(
			(char *) request_buffer,
			(char *) remainder, *remainder_size
		);
		n = *remainder_size;
	}

	res = os_read_file(
			fd,
			((char *) request_buffer) + *remainder_size,
			sizeof(struct io_thread_req *)*max_recs
				- *remainder_size
		);
	if (res > 0) {
		n += res;
		if ((n % sizeof(struct io_thread_req *)) > 0) {
			/*
			* Read somehow returned not a multiple of dword
			* theoretically possible, but never observed in the
			* wild, so read routine must be able to handle it
			*/
			*remainder_size = n % sizeof(struct io_thread_req *);
			WARN(*remainder_size > 0, "UBD IPC read returned a partial result");
			memmove(
				remainder,
				((char *) request_buffer) +
					(n/sizeof(struct io_thread_req *))*sizeof(struct io_thread_req *),
				*remainder_size
			);
			n = n - *remainder_size;
		}
	} else {
		n = res;
	}
	return n;
}

static void ubd_end_request(struct io_thread_req *io_req)
{
	if (io_req->error == BLK_STS_NOTSUPP) {
		if (req_op(io_req->req) == REQ_OP_DISCARD)
			blk_queue_disable_discard(io_req->req->q);
		else if (req_op(io_req->req) == REQ_OP_WRITE_ZEROES)
			blk_queue_disable_write_zeroes(io_req->req->q);
	}
	blk_mq_end_request(io_req->req, io_req->error);
	kfree(io_req);
}

static irqreturn_t ubd_intr(int irq, void *dev)
{
	int len, i;

	while ((len = bulk_req_safe_read(thread_fd, irq_req_buffer,
			&irq_remainder, &irq_remainder_size,
			UBD_REQ_BUFFER_SIZE)) >= 0) {
		for (i = 0; i < len / sizeof(struct io_thread_req *); i++)
			ubd_end_request(irq_req_buffer[i]);
	}

	if (len < 0 && len != -EAGAIN)
		pr_err("spurious interrupt in %s, err = %d\n", __func__, len);
	return IRQ_HANDLED;
}

/* Only changed by ubd_init, which is an initcall. */
static struct os_helper_thread *io_td;

static void kill_io_thread(void)
{
	if (io_td)
		os_kill_helper_thread(io_td);
	if (ubd_ring) {
		os_io_ring_destroy(ubd_ring);
		ubd_ring = NULL;
	}
}

__uml_exitcall(kill_io_thread);

static inline int ubd_file_size(struct ubd *ubd_dev, __u64 *size_out)
{
	char *file;
	int fd;
	int err;

	__u32 version;
	__u32 align;
	char *backing_file;
	time64_t mtime;
	unsigned long long size;
	int sector_size;
	int bitmap_offset;

	if (ubd_dev->file && ubd_dev->cow.file) {
		file = ubd_dev->cow.file;

		goto out;
	}

	fd = os_open_file(ubd_dev->file, of_read(OPENFLAGS()), 0);
	if (fd < 0)
		return fd;

	err = read_cow_header(file_reader, &fd, &version, &backing_file, \
		&mtime, &size, &sector_size, &align, &bitmap_offset);
	os_close_file(fd);

	if(err == -EINVAL)
		file = ubd_dev->file;
	else
		file = backing_file;

out:
	return os_file_size(file, size_out);
}

static int read_cow_bitmap(int fd, void *buf, int offset, int len)
{
	int err;

	err = os_pread_file(fd, buf, len, offset);
	if (err < 0)
		return err;

	return 0;
}

static int backing_file_mismatch(char *file, __u64 size, time64_t mtime)
{
	time64_t modtime;
	unsigned long long actual;
	int err;

	err = os_file_modtime(file, &modtime);
	if (err < 0) {
		printk(KERN_ERR "Failed to get modification time of backing "
		       "file \"%s\", err = %d\n", file, -err);
		return err;
	}

	err = os_file_size(file, &actual);
	if (err < 0) {
		printk(KERN_ERR "Failed to get size of backing file \"%s\", "
		       "err = %d\n", file, -err);
		return err;
	}

	if (actual != size) {
		/*__u64 can be a long on AMD64 and with %lu GCC complains; so
		 * the typecast.*/
		printk(KERN_ERR "Size mismatch (%llu vs %llu) of COW header "
		       "vs backing file\n", (unsigned long long) size, actual);
		return -EINVAL;
	}
	if (modtime != mtime) {
		printk(KERN_ERR "mtime mismatch (%lld vs %lld) of COW header vs "
		       "backing file\n", mtime, modtime);
		return -EINVAL;
	}
	return 0;
}

static int path_requires_switch(char *from_cmdline, char *from_cow, char *cow)
{
	struct uml_stat buf1, buf2;
	int err;

	if (from_cmdline == NULL)
		return 0;
	if (!strcmp(from_cmdline, from_cow))
		return 0;

	err = os_stat_file(from_cmdline, &buf1);
	if (err < 0) {
		printk(KERN_ERR "Couldn't stat '%s', err = %d\n", from_cmdline,
		       -err);
		return 0;
	}
	err = os_stat_file(from_cow, &buf2);
	if (err < 0) {
		printk(KERN_ERR "Couldn't stat '%s', err = %d\n", from_cow,
		       -err);
		return 1;
	}
	if ((buf1.ust_dev == buf2.ust_dev) && (buf1.ust_ino == buf2.ust_ino))
		return 0;

	printk(KERN_ERR "Backing file mismatch - \"%s\" requested, "
	       "\"%s\" specified in COW header of \"%s\"\n",
	       from_cmdline, from_cow, cow);
	return 1;
}

static int open_ubd_file(char *file, struct openflags *openflags, int shared,
		  char **backing_file_out, int *bitmap_offset_out,
		  unsigned long *bitmap_len_out, int *data_offset_out,
		  int *create_cow_out)
{
	time64_t mtime;
	unsigned long long size;
	__u32 version, align;
	char *backing_file;
	int fd, err, sectorsize, asked_switch, mode = 0644;

	fd = os_open_file(file, *openflags, mode);
	if (fd < 0) {
		if ((fd == -ENOENT) && (create_cow_out != NULL))
			*create_cow_out = 1;
		if (!openflags->w ||
		    ((fd != -EROFS) && (fd != -EACCES)))
			return fd;
		openflags->w = 0;
		fd = os_open_file(file, *openflags, mode);
		if (fd < 0)
			return fd;
	}

	if (shared)
		printk(KERN_INFO "Not locking \"%s\" on the host\n", file);
	else {
		err = os_lock_file(fd, openflags->w);
		if (err < 0) {
			printk(KERN_ERR "Failed to lock '%s', err = %d\n",
			       file, -err);
			goto out_close;
		}
	}

	/* Successful return case! */
	if (backing_file_out == NULL)
		return fd;

	err = read_cow_header(file_reader, &fd, &version, &backing_file, &mtime,
			      &size, &sectorsize, &align, bitmap_offset_out);
	if (err && (*backing_file_out != NULL)) {
		printk(KERN_ERR "Failed to read COW header from COW file "
		       "\"%s\", errno = %d\n", file, -err);
		goto out_close;
	}
	if (err)
		return fd;

	asked_switch = path_requires_switch(*backing_file_out, backing_file,
					    file);

	/* Allow switching only if no mismatch. */
	if (asked_switch && !backing_file_mismatch(*backing_file_out, size,
						   mtime)) {
		printk(KERN_ERR "Switching backing file to '%s'\n",
		       *backing_file_out);
		err = write_cow_header(file, fd, *backing_file_out,
				       sectorsize, align, &size);
		if (err) {
			printk(KERN_ERR "Switch failed, errno = %d\n", -err);
			goto out_close;
		}
	} else {
		*backing_file_out = backing_file;
		err = backing_file_mismatch(*backing_file_out, size, mtime);
		if (err)
			goto out_close;
	}

	cow_sizes(version, size, sectorsize, align, *bitmap_offset_out,
		  bitmap_len_out, data_offset_out);

	return fd;
 out_close:
	os_close_file(fd);
	return err;
}

static int create_cow_file(char *cow_file, char *backing_file,
		    struct openflags flags,
		    int sectorsize, int alignment, int *bitmap_offset_out,
		    unsigned long *bitmap_len_out, int *data_offset_out)
{
	int err, fd;

	flags.c = 1;
	fd = open_ubd_file(cow_file, &flags, 0, NULL, NULL, NULL, NULL, NULL);
	if (fd < 0) {
		err = fd;
		printk(KERN_ERR "Open of COW file '%s' failed, errno = %d\n",
		       cow_file, -err);
		goto out;
	}

	err = init_cow_file(fd, cow_file, backing_file, sectorsize, alignment,
			    bitmap_offset_out, bitmap_len_out,
			    data_offset_out);
	if (!err)
		return fd;
	os_close_file(fd);
 out:
	return err;
}

static void ubd_close_dev(struct ubd *ubd_dev)
{
	os_close_file(ubd_dev->fd);
	if(ubd_dev->cow.file == NULL)
		return;

	os_close_file(ubd_dev->cow.fd);
	vfree(ubd_dev->cow.bitmap);
	ubd_dev->cow.bitmap = NULL;
}

static int ubd_open_dev(struct ubd *ubd_dev)
{
	struct openflags flags;
	char **back_ptr;
	int err, create_cow, *create_ptr;
	int fd;

	ubd_dev->openflags = ubd_dev->boot_openflags;
	create_cow = 0;
	create_ptr = (ubd_dev->cow.file != NULL) ? &create_cow : NULL;
	back_ptr = ubd_dev->no_cow ? NULL : &ubd_dev->cow.file;

	fd = open_ubd_file(ubd_dev->file, &ubd_dev->openflags, ubd_dev->shared,
				back_ptr, &ubd_dev->cow.bitmap_offset,
				&ubd_dev->cow.bitmap_len, &ubd_dev->cow.data_offset,
				create_ptr);

	if((fd == -ENOENT) && create_cow){
		fd = create_cow_file(ubd_dev->file, ubd_dev->cow.file,
					  ubd_dev->openflags, SECTOR_SIZE, PAGE_SIZE,
					  &ubd_dev->cow.bitmap_offset,
					  &ubd_dev->cow.bitmap_len,
					  &ubd_dev->cow.data_offset);
		if(fd >= 0){
			printk(KERN_INFO "Creating \"%s\" as COW file for "
			       "\"%s\"\n", ubd_dev->file, ubd_dev->cow.file);
		}
	}

	if(fd < 0){
		printk("Failed to open '%s', errno = %d\n", ubd_dev->file,
		       -fd);
		return fd;
	}
	ubd_dev->fd = fd;

	if(ubd_dev->cow.file != NULL){
		err = -ENOMEM;
		ubd_dev->cow.bitmap = vmalloc(ubd_dev->cow.bitmap_len);
		if(ubd_dev->cow.bitmap == NULL){
			printk(KERN_ERR "Failed to vmalloc COW bitmap\n");
			goto error;
		}

		err = read_cow_bitmap(ubd_dev->fd, ubd_dev->cow.bitmap,
				      ubd_dev->cow.bitmap_offset,
				      ubd_dev->cow.bitmap_len);
		if(err < 0)
			goto error;

		flags = ubd_dev->openflags;
		flags.w = 0;
		err = open_ubd_file(ubd_dev->cow.file, &flags, ubd_dev->shared, NULL,
				    NULL, NULL, NULL, NULL);
		if(err < 0) goto error;
		ubd_dev->cow.fd = err;
	}
	return 0;
 error:
	os_close_file(ubd_dev->fd);
	return err;
}

static void ubd_device_release(struct device *dev)
{
	struct ubd *ubd_dev = container_of(dev, struct ubd, pdev.dev);

	blk_mq_free_tag_set(&ubd_dev->tag_set);
	*ubd_dev = ((struct ubd) DEFAULT_UBD);
}

static ssize_t serial_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct ubd *ubd_dev = disk->private_data;

	if (!ubd_dev)
		return 0;

	return sprintf(buf, "%s", ubd_dev->serial);
}

static DEVICE_ATTR_RO(serial);

static struct attribute *ubd_attrs[] = {
	&dev_attr_serial.attr,
	NULL,
};

static umode_t ubd_attrs_are_visible(struct kobject *kobj,
				     struct attribute *a, int n)
{
	return a->mode;
}

static const struct attribute_group ubd_attr_group = {
	.attrs = ubd_attrs,
	.is_visible = ubd_attrs_are_visible,
};

static const struct attribute_group *ubd_attr_groups[] = {
	&ubd_attr_group,
	NULL,
};

#define ROUND_BLOCK(n) ((n + (SECTOR_SIZE - 1)) & (-SECTOR_SIZE))

static const struct blk_mq_ops ubd_mq_ops = {
	.queue_rq = ubd_queue_rq,
};

static int ubd_add(int n, char **error_out)
{
	struct ubd *ubd_dev = &ubd_devs[n];
	struct queue_limits lim = {
		.max_segments		= MAX_SG,
		.seg_boundary_mask	= PAGE_SIZE - 1,
		.features		= BLK_FEAT_WRITE_CACHE,
	};
	struct gendisk *disk;
	int err = 0;

	if(ubd_dev->file == NULL)
		goto out;

	if (ubd_dev->cow.file)
		lim.max_hw_sectors = 8 * sizeof(long);
	if (!ubd_dev->no_trim) {
		lim.max_hw_discard_sectors = UBD_MAX_REQUEST;
		lim.max_write_zeroes_sectors = UBD_MAX_REQUEST;
	}

	err = ubd_file_size(ubd_dev, &ubd_dev->size);
	if(err < 0){
		*error_out = "Couldn't determine size of device's file";
		goto out;
	}

	err = ubd_open_dev(ubd_dev);
	if (err) {
		pr_err("ubd%c: Can't open \"%s\": errno = %d\n",
			'a' + n, ubd_dev->file, -err);
		goto out;
	}

	ubd_dev->size = ROUND_BLOCK(ubd_dev->size);

	ubd_dev->tag_set.ops = &ubd_mq_ops;
	ubd_dev->tag_set.queue_depth = 64;
	ubd_dev->tag_set.numa_node = NUMA_NO_NODE;
	ubd_dev->tag_set.driver_data = ubd_dev;
	ubd_dev->tag_set.nr_hw_queues = 1;

	err = blk_mq_alloc_tag_set(&ubd_dev->tag_set);
	if (err)
		goto out_close;

	disk = blk_mq_alloc_disk(&ubd_dev->tag_set, &lim, ubd_dev);
	if (IS_ERR(disk)) {
		err = PTR_ERR(disk);
		goto out_cleanup_tags;
	}

	disk->major = UBD_MAJOR;
	disk->first_minor = n << UBD_SHIFT;
	disk->minors = 1 << UBD_SHIFT;
	disk->fops = &ubd_blops;
	set_capacity(disk, ubd_dev->size / 512);
	sprintf(disk->disk_name, "ubd%c", 'a' + n);
	disk->private_data = ubd_dev;
	set_disk_ro(disk, !ubd_dev->openflags.w);

	ubd_dev->pdev.id = n;
	ubd_dev->pdev.name = DRIVER_NAME;
	ubd_dev->pdev.dev.release = ubd_device_release;
	dev_set_drvdata(&ubd_dev->pdev.dev, ubd_dev);
	platform_device_register(&ubd_dev->pdev);

	err = device_add_disk(&ubd_dev->pdev.dev, disk, ubd_attr_groups);
	if (err)
		goto out_cleanup_disk;

	ubd_dev->disk = disk;

	return 0;

out_cleanup_disk:
	put_disk(disk);
out_cleanup_tags:
	blk_mq_free_tag_set(&ubd_dev->tag_set);
out_close:
	ubd_close_dev(ubd_dev);
out:
	return err;
}

static int ubd_config(char *str, char **error_out)
{
	int n, ret;

	/* This string is possibly broken up and stored, so it's only
	 * freed if ubd_setup_common fails, or if only general options
	 * were set.
	 */
	str = kstrdup(str, GFP_KERNEL);
	if (str == NULL) {
		*error_out = "Failed to allocate memory";
		return -ENOMEM;
	}

	ret = ubd_setup_common(str, &n, error_out);
	if (ret)
		goto err_free;

	if (n == -1) {
		ret = 0;
		goto err_free;
	}

	mutex_lock(&ubd_lock);
	ret = ubd_add(n, error_out);
	if (ret)
		ubd_devs[n].file = NULL;
	mutex_unlock(&ubd_lock);

out:
	return ret;

err_free:
	kfree(str);
	goto out;
}

static int ubd_get_config(char *name, char *str, int size, char **error_out)
{
	struct ubd *ubd_dev;
	int n, len = 0;

	n = parse_unit(&name);
	if((n >= MAX_DEV) || (n < 0)){
		*error_out = "ubd_get_config : device number out of range";
		return -1;
	}

	ubd_dev = &ubd_devs[n];
	mutex_lock(&ubd_lock);

	if(ubd_dev->file == NULL){
		CONFIG_CHUNK(str, size, len, "", 1);
		goto out;
	}

	CONFIG_CHUNK(str, size, len, ubd_dev->file, 0);

	if(ubd_dev->cow.file != NULL){
		CONFIG_CHUNK(str, size, len, ",", 0);
		CONFIG_CHUNK(str, size, len, ubd_dev->cow.file, 1);
	}
	else CONFIG_CHUNK(str, size, len, "", 1);

 out:
	mutex_unlock(&ubd_lock);
	return len;
}

static int ubd_id(char **str, int *start_out, int *end_out)
{
	int n;

	n = parse_unit(str);
	*start_out = 0;
	*end_out = MAX_DEV - 1;
	return n;
}

static int ubd_remove(int n, char **error_out)
{
	struct ubd *ubd_dev;
	int err = -ENODEV;

	mutex_lock(&ubd_lock);

	ubd_dev = &ubd_devs[n];

	if(ubd_dev->file == NULL)
		goto out;

	if (ubd_dev->disk) {
		/* you cannot remove a open disk */
		err = -EBUSY;
		if (disk_openers(ubd_dev->disk))
			goto out;

		del_gendisk(ubd_dev->disk);
		ubd_close_dev(ubd_dev);
		put_disk(ubd_dev->disk);
	}

	err = 0;
	platform_device_unregister(&ubd_dev->pdev);
out:
	mutex_unlock(&ubd_lock);
	return err;
}

/* All these are called by mconsole in process context and without
 * ubd-specific locks.  The structure itself is const except for .list.
 */
static struct mc_device ubd_mc = {
	.list		= LIST_HEAD_INIT(ubd_mc.list),
	.name		= "ubd",
	.config		= ubd_config,
	.get_config	= ubd_get_config,
	.id		= ubd_id,
	.remove		= ubd_remove,
};

static int __init ubd_mc_init(void)
{
	mconsole_register_dev(&ubd_mc);
	return 0;
}

__initcall(ubd_mc_init);

static int __init ubd0_init(void)
{
	struct ubd *ubd_dev = &ubd_devs[0];

	mutex_lock(&ubd_lock);
	if(ubd_dev->file == NULL)
		ubd_dev->file = "root_fs";
	mutex_unlock(&ubd_lock);

	return 0;
}

__initcall(ubd0_init);

/* Used in ubd_init, which is an initcall */
static struct platform_driver ubd_driver = {
	.driver = {
		.name  = DRIVER_NAME,
	},
};

static int __init ubd_init(void)
{
	char *error;
	int i, err;

	if (register_blkdev(UBD_MAJOR, "ubd"))
		return -1;

	irq_req_buffer = kmalloc_objs(struct io_thread_req *,
				      UBD_REQ_BUFFER_SIZE);
	irq_remainder = 0;

	if (irq_req_buffer == NULL) {
		printk(KERN_ERR "Failed to initialize ubd buffering\n");
		return -ENOMEM;
	}
	io_req_buffer = kmalloc_objs(struct io_thread_req *,
				     UBD_REQ_BUFFER_SIZE);

	io_remainder = 0;

	if (io_req_buffer == NULL) {
		printk(KERN_ERR "Failed to initialize ubd buffering\n");
		return -ENOMEM;
	}
	platform_driver_register(&ubd_driver);
	mutex_lock(&ubd_lock);
	for (i = 0; i < MAX_DEV; i++){
		err = ubd_add(i, &error);
		if(err)
			printk(KERN_ERR "Failed to initialize ubd device %d :"
			       "%s\n", i, error);
	}
	mutex_unlock(&ubd_lock);
	return 0;
}

late_initcall(ubd_init);

static int __init ubd_driver_init(void)
{
	int err;

	/* Set by CONFIG_BLK_DEV_UBD_SYNC or ubd=sync.*/
	if(global_openflags.s){
		printk(KERN_INFO "ubd: Synchronous mode\n");
		/* Letting ubd=sync be like using ubd#s= instead of ubd#= is
		 * enough. So use anyway the io thread. */
	}
	err = start_io_thread(&io_td, &thread_fd);
	if (err < 0) {
		printk(KERN_ERR
		       "ubd : Failed to start I/O thread (errno = %d) - "
		       "falling back to synchronous I/O\n", -err);
		return 0;
	}
	/*
	 * Phase 2a (memo #2): try to bring up the io_uring substrate.
	 * If it succeeds, do_io_ring takes over for read/write ops with
	 * within-request parallelism.  If io_uring_setup is unavailable
	 * (older host, seccomp-blocked) ubd_ring stays NULL and the
	 * legacy synchronous do_io path runs unchanged.
	 *
	 * `ubd_no_io_uring=1` cmdline forces the legacy path for A/B
	 * perf comparison without changing the binary.
	 */
	if (ubd_no_io_uring) {
		printk(KERN_INFO "ubd: io_uring disabled by ubd_no_io_uring=1; using legacy sync I/O\n");
		ubd_ring = NULL;
	} else {
		ubd_ring = os_io_ring_create(UBD_RING_DEPTH);
		if (ubd_ring)
			printk(KERN_INFO "ubd: io_uring substrate active (depth=%d)\n",
			       UBD_RING_DEPTH);
		else
			printk(KERN_INFO "ubd: io_uring unavailable, using legacy sync I/O\n");
	}
	err = um_request_irq(UBD_IRQ, thread_fd, IRQ_READ, ubd_intr,
			     0, "ubd", ubd_devs);
	if(err < 0)
		printk(KERN_ERR "um_request_irq failed - errno = %d\n", -err);
	return 0;
}

device_initcall(ubd_driver_init);

static void cowify_bitmap(__u64 io_offset, int length, unsigned long *cow_mask,
			  __u64 *cow_offset, unsigned long *bitmap,
			  __u64 bitmap_offset, unsigned long *bitmap_words,
			  __u64 bitmap_len)
{
	__u64 sector = io_offset >> SECTOR_SHIFT;
	int i, update_bitmap = 0;

	for (i = 0; i < length >> SECTOR_SHIFT; i++) {
		if(cow_mask != NULL)
			ubd_set_bit(i, (unsigned char *) cow_mask);
		if(ubd_test_bit(sector + i, (unsigned char *) bitmap))
			continue;

		update_bitmap = 1;
		ubd_set_bit(sector + i, (unsigned char *) bitmap);
	}

	if(!update_bitmap)
		return;

	*cow_offset = sector / (sizeof(unsigned long) * 8);

	/* This takes care of the case where we're exactly at the end of the
	 * device, and *cow_offset + 1 is off the end.  So, just back it up
	 * by one word.  Thanks to Lynn Kerby for the fix and James McMechan
	 * for the original diagnosis.
	 */
	if (*cow_offset == (DIV_ROUND_UP(bitmap_len,
					 sizeof(unsigned long)) - 1))
		(*cow_offset)--;

	bitmap_words[0] = bitmap[*cow_offset];
	bitmap_words[1] = bitmap[*cow_offset + 1];

	*cow_offset *= sizeof(unsigned long);
	*cow_offset += bitmap_offset;
}

static void cowify_req(struct io_thread_req *req, struct io_desc *segment,
		       unsigned long offset, unsigned long *bitmap,
		       __u64 bitmap_offset, __u64 bitmap_len)
{
	__u64 sector = offset >> SECTOR_SHIFT;
	int i;

	if (segment->length > (sizeof(segment->sector_mask) * 8) << SECTOR_SHIFT)
		panic("Operation too long");

	if (req_op(req->req) == REQ_OP_READ) {
		for (i = 0; i < segment->length >> SECTOR_SHIFT; i++) {
			if(ubd_test_bit(sector + i, (unsigned char *) bitmap))
				ubd_set_bit(i, (unsigned char *)
					    &segment->sector_mask);
		}
	} else {
		cowify_bitmap(offset, segment->length, &segment->sector_mask,
			      &segment->cow_offset, bitmap, bitmap_offset,
			      segment->bitmap_words, bitmap_len);
	}
}

static void ubd_map_req(struct ubd *dev, struct io_thread_req *io_req,
			struct request *req)
{
	struct bio_vec bvec;
	struct req_iterator iter;
	int i = 0;
	unsigned long byte_offset = io_req->offset;
	enum req_op op = req_op(req);

	if (op == REQ_OP_WRITE_ZEROES || op == REQ_OP_DISCARD) {
		io_req->io_desc[0].buffer = NULL;
		io_req->io_desc[0].length = blk_rq_bytes(req);
	} else {
		rq_for_each_segment(bvec, req, iter) {
			BUG_ON(i >= io_req->desc_cnt);

			io_req->io_desc[i].buffer = bvec_virt(&bvec);
			io_req->io_desc[i].length = bvec.bv_len;
			i++;
		}
	}

	if (dev->cow.file) {
		for (i = 0; i < io_req->desc_cnt; i++) {
			cowify_req(io_req, &io_req->io_desc[i], byte_offset,
				   dev->cow.bitmap, dev->cow.bitmap_offset,
				   dev->cow.bitmap_len);
			byte_offset += io_req->io_desc[i].length;
		}

	}
}

static struct io_thread_req *ubd_alloc_req(struct ubd *dev, struct request *req,
					   int desc_cnt)
{
	struct io_thread_req *io_req;
	int i;

	io_req = kmalloc(sizeof(*io_req) +
			 (desc_cnt * sizeof(struct io_desc)),
			 GFP_ATOMIC);
	if (!io_req)
		return NULL;

	io_req->req = req;
	if (dev->cow.file)
		io_req->fds[0] = dev->cow.fd;
	else
		io_req->fds[0] = dev->fd;
	io_req->error = 0;
	io_req->sectorsize = SECTOR_SIZE;
	io_req->fds[1] = dev->fd;
	io_req->offset = (u64) blk_rq_pos(req) << SECTOR_SHIFT;
	io_req->offsets[0] = 0;
	io_req->offsets[1] = dev->cow.data_offset;

	for (i = 0 ; i < desc_cnt; i++) {
		io_req->io_desc[i].sector_mask = 0;
		io_req->io_desc[i].cow_offset = -1;
	}

	return io_req;
}

static int ubd_submit_request(struct ubd *dev, struct request *req)
{
	int segs = 0;
	struct io_thread_req *io_req;
	int ret;
	enum req_op op = req_op(req);

	if (op == REQ_OP_FLUSH)
		segs = 0;
	else if (op == REQ_OP_WRITE_ZEROES || op == REQ_OP_DISCARD)
		segs = 1;
	else
		segs = blk_rq_nr_phys_segments(req);

	io_req = ubd_alloc_req(dev, req, segs);
	if (!io_req)
		return -ENOMEM;

	io_req->desc_cnt = segs;
	if (segs)
		ubd_map_req(dev, io_req, req);

	ret = os_write_file(thread_fd, &io_req, sizeof(io_req));
	if (ret != sizeof(io_req)) {
		if (ret != -EAGAIN)
			pr_err("write to io thread failed: %d\n", -ret);
		kfree(io_req);
	}
	return ret;
}

static blk_status_t ubd_queue_rq(struct blk_mq_hw_ctx *hctx,
				 const struct blk_mq_queue_data *bd)
{
	struct ubd *ubd_dev = hctx->queue->queuedata;
	struct request *req = bd->rq;
	int ret = 0, res = BLK_STS_OK;

	blk_mq_start_request(req);

	spin_lock_irq(&ubd_dev->lock);

	switch (req_op(req)) {
	case REQ_OP_FLUSH:
	case REQ_OP_READ:
	case REQ_OP_WRITE:
	case REQ_OP_DISCARD:
	case REQ_OP_WRITE_ZEROES:
		ret = ubd_submit_request(ubd_dev, req);
		break;
	default:
		WARN_ON_ONCE(1);
		res = BLK_STS_NOTSUPP;
	}

	spin_unlock_irq(&ubd_dev->lock);

	if (ret < 0) {
		if (ret == -ENOMEM)
			res = BLK_STS_RESOURCE;
		else
			res = BLK_STS_DEV_RESOURCE;
	}

	return res;
}

static int ubd_getgeo(struct gendisk *disk, struct hd_geometry *geo)
{
	struct ubd *ubd_dev = disk->private_data;

	geo->heads = 128;
	geo->sectors = 32;
	geo->cylinders = ubd_dev->size / (128 * 32 * 512);
	return 0;
}

static int ubd_ioctl(struct block_device *bdev, blk_mode_t mode,
		     unsigned int cmd, unsigned long arg)
{
	struct ubd *ubd_dev = bdev->bd_disk->private_data;
	u16 ubd_id[ATA_ID_WORDS];

	switch (cmd) {
		struct cdrom_volctrl volume;
	case HDIO_GET_IDENTITY:
		memset(&ubd_id, 0, ATA_ID_WORDS * 2);
		ubd_id[ATA_ID_CYLS]	= ubd_dev->size / (128 * 32 * 512);
		ubd_id[ATA_ID_HEADS]	= 128;
		ubd_id[ATA_ID_SECTORS]	= 32;
		if(copy_to_user((char __user *) arg, (char *) &ubd_id,
				 sizeof(ubd_id)))
			return -EFAULT;
		return 0;

	case CDROMVOLREAD:
		if(copy_from_user(&volume, (char __user *) arg, sizeof(volume)))
			return -EFAULT;
		volume.channel0 = 255;
		volume.channel1 = 255;
		volume.channel2 = 255;
		volume.channel3 = 255;
		if(copy_to_user((char __user *) arg, &volume, sizeof(volume)))
			return -EFAULT;
		return 0;
	}
	return -EINVAL;
}

static int map_error(int error_code)
{
	switch (error_code) {
	case 0:
		return BLK_STS_OK;
	case ENOSYS:
	case EOPNOTSUPP:
		return BLK_STS_NOTSUPP;
	case ENOSPC:
		return BLK_STS_NOSPC;
	}
	return BLK_STS_IOERR;
}

/*
 * Everything from here onwards *IS NOT PART OF THE KERNEL*
 *
 * The following functions are part of UML hypervisor code.
 * All functions from here onwards are executed as a helper
 * thread and are not allowed to execute any kernel functions.
 *
 * Any communication must occur strictly via shared memory and IPC.
 *
 * Do not add printks, locks, kernel memory operations, etc - it
 * will result in unpredictable behaviour and/or crashes.
 */

static int update_bitmap(struct io_thread_req *req, struct io_desc *segment)
{
	int n;

	if (segment->cow_offset == -1)
		return map_error(0);

	n = os_pwrite_file(req->fds[1], &segment->bitmap_words,
			  sizeof(segment->bitmap_words), segment->cow_offset);
	if (n != sizeof(segment->bitmap_words))
		return map_error(-n);

	return map_error(0);
}

static void do_io(struct io_thread_req *req, struct io_desc *desc)
{
	char *buf = NULL;
	unsigned long len;
	int n, nsectors, start, end, bit;
	__u64 off;

	/* FLUSH is really a special case, we cannot "case" it with others */

	if (req_op(req->req) == REQ_OP_FLUSH) {
		/* fds[0] is always either the rw image or our cow file */
		req->error = map_error(-os_sync_file(req->fds[0]));
		return;
	}

	nsectors = desc->length / req->sectorsize;
	start = 0;
	do {
		bit = ubd_test_bit(start, (unsigned char *) &desc->sector_mask);
		end = start;
		while((end < nsectors) &&
		      (ubd_test_bit(end, (unsigned char *) &desc->sector_mask) == bit))
			end++;

		off = req->offset + req->offsets[bit] +
			start * req->sectorsize;
		len = (end - start) * req->sectorsize;
		if (desc->buffer != NULL)
			buf = &desc->buffer[start * req->sectorsize];

		switch (req_op(req->req)) {
		case REQ_OP_READ:
			n = 0;
			do {
				buf = &buf[n];
				len -= n;
				n = os_pread_file(req->fds[bit], buf, len, off);
				if (n < 0) {
					req->error = map_error(-n);
					return;
				}
			} while((n < len) && (n != 0));
			if (n < len) memset(&buf[n], 0, len - n);
			break;
		case REQ_OP_WRITE:
			n = os_pwrite_file(req->fds[bit], buf, len, off);
			if(n != len){
				req->error = map_error(-n);
				return;
			}
			break;
		case REQ_OP_DISCARD:
			n = os_falloc_punch(req->fds[bit], off, len);
			if (n) {
				req->error = map_error(-n);
				return;
			}
			break;
		case REQ_OP_WRITE_ZEROES:
			n = os_falloc_zeroes(req->fds[bit], off, len);
			if (n) {
				req->error = map_error(-n);
				return;
			}
			break;
		default:
			WARN_ON_ONCE(1);
			req->error = BLK_STS_NOTSUPP;
			return;
		}

		start = end;
	} while(start < nsectors);

	req->offset += len;
	req->error = update_bitmap(req, desc);
}

/* Changed in start_io_thread, which is serialized by being called only
 * from ubd_init, which is an initcall.
 */
int kernel_fd = -1;

/* Only changed by the io thread. XXX: currently unused. */
static int io_count;

/*
 * Per Documentation/virt/uml/redesign/06-sequencing/post-2026-05-19-
 * next-sprint/02-ubd-io-uring.md (memo #2, Phase 2a).
 *
 * Async per-request submission via the host io_uring substrate
 * (arch/um/os-Linux/io_uring.c).  Each request's segments are
 * submitted in parallel; completions are harvested in any order;
 * partial reads/writes are re-submitted for the remainder.
 *
 * The DISCARD / WRITE_ZEROES / FLUSH paths stay on the legacy
 * synchronous helpers (memo §Phase 5 future work).  Bitmap update
 * stays synchronous (memo §Phase 5 with IOSQE_IO_DRAIN).
 *
 * Cross-request parallelism (memo Phase 2b) requires restructuring
 * the io_thread loop to drain reqs into the ring without per-req
 * harvest barriers.  This commit lands within-request parallelism
 * only — already a measurable win on multi-segment requests (the
 * common case for MAX_SG > 1 workloads like Postgres / mmapped
 * pagewriteback).
 *
 * UBD_RING_DEPTH + ubd_ring forward-declared near the top of the
 * file so __init code paths see them without reorder pain.
 */

/*
 * MAX_SG = 64 is the block-layer cap on physical segments per request
 * (set in ubd_blk_mq_ops above).  A single vectored SQE can therefore
 * cover up to MAX_SG iovec entries.
 */
struct ubd_pending_slot {
	int		in_use;
	int		op;            /* REQ_OP_READ / REQ_OP_WRITE */
	int		fd;
	char		*buf;          /* may be NULL for short-read zerofill */
	unsigned long	total;         /* original requested length */
	unsigned long	done;          /* bytes completed so far */
	unsigned long long off;

	/*
	 * Phase 2b (memo #2): the owning request, so a CQE harvested
	 * from any pending slot can be routed back to its req's error
	 * field even when slots from many reqs are in flight at once.
	 */
	struct io_thread_req	*req;

	/*
	 * Phase 3 (memo #2): vectored submission.  When .vec_cnt > 0
	 * the slot represents a single IORING_OP_READV / WRITEV
	 * spanning io_desc[0..vec_cnt-1].  Short-completion fall-back
	 * routes the remainder through the per-segment path via
	 * ubd_ring_resubmit_vectored().
	 */
	int		vec_cnt;
	struct iovec	vec[MAX_SG];
};

static struct os_io_ring *ubd_ring;
static struct ubd_pending_slot ubd_slots[UBD_RING_DEPTH];

static int ubd_slot_alloc(void)
{
	int i;

	for (i = 0; i < UBD_RING_DEPTH; i++) {
		if (!ubd_slots[i].in_use) {
			ubd_slots[i].in_use = 1;
			return i;
		}
	}
	return -1;
}

static void ubd_slot_free(int i) { ubd_slots[i].in_use = 0; }

/*
 * Vectored-slot short-completion: trim consumed iovecs from the
 * front and resubmit the remainder as a fresh writev/readv.  Used
 * by ubd_ring_resubmit when s->vec_cnt > 0.
 */
static int ubd_ring_resubmit_vectored(int slot_idx)
{
	struct ubd_pending_slot *s = &ubd_slots[slot_idx];
	unsigned long long off = s->off + s->done;
	unsigned long pos = 0;
	int i, j;

	for (i = 0; i < s->vec_cnt; i++) {
		unsigned long ilen = s->vec[i].iov_len;

		if (pos + ilen > s->done)
			break;
		pos += ilen;
	}
	if (i >= s->vec_cnt)
		return -EINVAL;	/* over-completed — caller frees slot */

	if (s->done > pos) {
		unsigned long inner = s->done - pos;

		s->vec[i].iov_base = (char *)s->vec[i].iov_base + inner;
		s->vec[i].iov_len -= inner;
	}
	if (i > 0) {
		for (j = 0; j < s->vec_cnt - i; j++)
			s->vec[j] = s->vec[i + j];
		s->vec_cnt -= i;
	}
	s->done = 0;
	s->off  = off;
	s->total = 0;
	for (j = 0; j < s->vec_cnt; j++)
		s->total += s->vec[j].iov_len;

	if (s->op == REQ_OP_READ)
		return os_io_ring_submit_preadv(ubd_ring, s->fd, s->vec,
						s->vec_cnt, off, slot_idx);
	return os_io_ring_submit_pwritev(ubd_ring, s->fd, s->vec,
					 s->vec_cnt, off, slot_idx);
}

static int ubd_ring_resubmit(int slot_idx)
{
	struct ubd_pending_slot *s = &ubd_slots[slot_idx];
	unsigned long remaining;
	char *buf;
	unsigned long long off;

	if (s->vec_cnt > 0)
		return ubd_ring_resubmit_vectored(slot_idx);

	remaining = s->total - s->done;
	buf = s->buf ? s->buf + s->done : NULL;
	off = s->off + s->done;

	if (s->op == REQ_OP_READ)
		return os_io_ring_submit_pread(ubd_ring, s->fd, buf,
					       remaining, off, slot_idx);
	return os_io_ring_submit_pwrite(ubd_ring, s->fd, buf,
					remaining, off, slot_idx);
}

/*
 * Submit one contiguous (op, fd, buf, off, len) tuple to the ring.
 * Returns the allocated slot index on success or -errno.  On
 * ring-full (-EAGAIN) the caller is expected to harvest some CQEs
 * and retry.
 */
static int ubd_ring_submit_one(struct io_thread_req *req, int op, int fd,
			       char *buf, unsigned long len,
			       unsigned long long off)
{
	int slot, rc;
	struct ubd_pending_slot *s;

	slot = ubd_slot_alloc();
	if (slot < 0)
		return -EAGAIN;
	s = &ubd_slots[slot];
	s->op      = op;
	s->fd      = fd;
	s->buf     = buf;
	s->total   = len;
	s->done    = 0;
	s->off     = off;
	s->vec_cnt = 0;
	s->req     = req;

	rc = ubd_ring_resubmit(slot);
	if (rc < 0) {
		ubd_slot_free(slot);
		return rc;
	}
	return slot;
}

/*
 * Phase 3 (memo #2) — vectored submission.
 *
 * When a request's io_desc[] is entirely uniform (sector_mask == 0 +
 * cow_offset == -1 everywhere), all descriptors target the same fd
 * and run contiguously on disk.  Coalesce them into a single
 * IORING_OP_WRITEV / READV submission: one SQE instead of desc_cnt.
 *
 * Caller has already excluded the non-R/W ops; both branches here
 * are safe.  Returns slot index on success, -errno on failure
 * (-EAGAIN if the ring is full; the caller is expected to harvest
 * one CQE and retry, same shape as ubd_ring_submit_one).
 */
static int ubd_ring_submit_vectored(int op, struct io_thread_req *req,
				    unsigned long long base_off)
{
	int slot, rc, i;
	struct ubd_pending_slot *s;
	unsigned long total = 0;

	slot = ubd_slot_alloc();
	if (slot < 0)
		return -EAGAIN;
	s = &ubd_slots[slot];
	s->op      = op;
	s->fd      = req->fds[0];
	s->buf     = NULL;
	s->done    = 0;
	s->off     = base_off + req->offsets[0];
	s->vec_cnt = req->desc_cnt;
	s->req     = req;
	for (i = 0; i < req->desc_cnt; i++) {
		s->vec[i].iov_base = req->io_desc[i].buffer;
		s->vec[i].iov_len  = req->io_desc[i].length;
		total += req->io_desc[i].length;
	}
	s->total = total;

	if (op == REQ_OP_READ)
		rc = os_io_ring_submit_preadv(ubd_ring, s->fd, s->vec,
					      s->vec_cnt, s->off, slot);
	else
		rc = os_io_ring_submit_pwritev(ubd_ring, s->fd, s->vec,
					       s->vec_cnt, s->off, slot);
	if (rc < 0) {
		ubd_slot_free(slot);
		return rc;
	}
	return slot;
}

static bool ubd_req_vectored_eligible(struct io_thread_req *req)
{
	int i;

	if (req->desc_cnt < 2)
		return false;
	for (i = 0; i < req->desc_cnt; i++) {
		struct io_desc *d = &req->io_desc[i];

		if (d->sector_mask != 0)
			return false;
		if (d->cow_offset != -1)
			return false;
		if (!d->buffer)
			return false;
	}
	return true;
}

/*
 * Async path for a single req.  Submits every read/write
 * sub-segment (split by sector_mask bit, same as legacy do_io) and
 * harvests all completions before returning.  Falls back to legacy
 * sync do_io for FLUSH / DISCARD / WRITE_ZEROES which aren't yet on
 * the ring (memo §Phase 5).
 */
/*
 * Phase 2b (memo #2) — cross-request parallelism helpers.
 *
 * do_io_ring() handles a single request at a time: submit all its
 * SQEs, drain all its CQEs, do its bitmap update.  That leaves the
 * ring's depth largely unused when the block layer hands us a batch
 * of N requests (UBD_REQ_BUFFER_SIZE / sizeof(*req) reqs per
 * bulk_req_safe_read).
 *
 * do_io_ring_batch() submits the SQEs for every request in the
 * batch *before* draining, so multiple reqs are in flight at once.
 * CQEs are routed back to the owning req via slot->req->error;
 * bitmap updates run after the final drain.  Same correctness
 * properties as the per-req path, modulo CQE ordering (which
 * already wasn't guaranteed).
 */
static int ubd_handle_cqe(const struct os_io_cqe *cqe)
{
	struct ubd_pending_slot *s = &ubd_slots[cqe->user_data];

	if (cqe->res < 0) {
		if (s->req && !s->req->error)
			s->req->error = map_error(-cqe->res);
		ubd_slot_free(cqe->user_data);
		return 1;
	}
	if (cqe->res == 0) {
		if (s->op == REQ_OP_READ && s->buf)
			memset(s->buf + s->done, 0, s->total - s->done);
		ubd_slot_free(cqe->user_data);
		return 1;
	}
	s->done += cqe->res;
	if (s->done < s->total) {
		int rc = ubd_ring_resubmit(cqe->user_data);

		if (rc < 0) {
			if (s->req && !s->req->error)
				s->req->error = map_error(-rc);
			ubd_slot_free(cqe->user_data);
			return 1;
		}
		return 0;
	}
	ubd_slot_free(cqe->user_data);
	return 1;
}

/*
 * Drain CQEs until the ring is empty.  Used after do_io_ring_batch
 * has submitted SQEs for every request in the bulk batch.
 */
static void ubd_ring_drain_all(void)
{
	while (os_io_ring_in_flight(ubd_ring) > 0) {
		struct os_io_cqe cqe;
		int hrc = os_io_ring_wait_cqe(ubd_ring, &cqe, -1);

		if (hrc < 0) {
			pr_err_ratelimited("ubd: io_uring drain failed: %d\n",
					   hrc);
			break;
		}
		if (hrc == 1)
			ubd_handle_cqe(&cqe);
	}
}

/*
 * Submit one SQE, harvesting a CQE if the ring is full.  Returns
 * the slot index on success or a negative -errno.  Distinct from
 * ubd_ring_submit_one in that this *blocks* on EAGAIN until the
 * ring has space (vs. returning EAGAIN to the caller).
 */
static int ubd_submit_with_harvest(struct io_thread_req *req, int op,
				   int fd, char *buf, unsigned long len,
				   unsigned long long off)
{
	int slot;

	for (;;) {
		slot = ubd_ring_submit_one(req, op, fd, buf, len, off);
		if (slot >= 0)
			return slot;
		if (slot != -EAGAIN)
			return slot;
		{
			struct os_io_cqe cqe;
			int hrc = os_io_ring_wait_cqe(ubd_ring, &cqe, -1);

			if (hrc < 0)
				return hrc;
			if (hrc == 1)
				ubd_handle_cqe(&cqe);
		}
	}
}

static int ubd_submit_vectored_with_harvest(int op, struct io_thread_req *req,
					    unsigned long long base_off)
{
	int slot;

	for (;;) {
		slot = ubd_ring_submit_vectored(op, req, base_off);
		if (slot >= 0)
			return slot;
		if (slot != -EAGAIN)
			return slot;
		{
			struct os_io_cqe cqe;
			int hrc = os_io_ring_wait_cqe(ubd_ring, &cqe, -1);

			if (hrc < 0)
				return hrc;
			if (hrc == 1)
				ubd_handle_cqe(&cqe);
		}
	}
}

static void ubd_submit_req_async(struct io_thread_req *req)
{
	u64 base_offset;
	int i, op;

	op = req_op(req->req);
	if (op != REQ_OP_READ && op != REQ_OP_WRITE) {
		/* FLUSH / DISCARD / WRITE_ZEROES — sync fallback. */
		for (i = 0; !req->error && i < req->desc_cnt; i++)
			do_io(req, &req->io_desc[i]);
		return;
	}

	base_offset = req->offset;

	if (ubd_req_vectored_eligible(req)) {
		int slot = ubd_submit_vectored_with_harvest(op, req, base_offset);

		if (slot < 0 && !req->error)
			req->error = map_error(-slot);
		return;
	}

	for (i = 0; !req->error && i < req->desc_cnt; i++) {
		struct io_desc *d = &req->io_desc[i];
		int nsectors = d->length / req->sectorsize;
		int start = 0;
		unsigned long last_len = 0;

		do {
			int bit = ubd_test_bit(start,
				(unsigned char *)&d->sector_mask);
			int end = start;
			__u64 off;
			unsigned long len;
			char *buf;
			int slot;

			while (end < nsectors &&
			       ubd_test_bit(end,
				(unsigned char *)&d->sector_mask) == bit)
				end++;

			off = base_offset + req->offsets[bit] +
			      start * req->sectorsize;
			len = (end - start) * req->sectorsize;
			last_len = len;
			buf = d->buffer
			      ? &d->buffer[start * req->sectorsize]
			      : NULL;

			slot = ubd_submit_with_harvest(req, op, req->fds[bit],
						       buf, len, off);
			if (slot < 0) {
				if (!req->error)
					req->error = map_error(-slot);
				return;
			}
			start = end;
		} while (start < nsectors);

		base_offset += last_len;
	}
}

static void do_io_ring_batch(struct io_thread_req **reqs, int n_reqs)
{
	int i, j;

	/*
	 * Phase 2b fast-path (memo #2): when the bulk batch is one
	 * single-desc request, the io_uring path has no parallelism
	 * to win — just a fixed +2 syscall cycle (sqe_commit +
	 * io_uring_enter + cqe_poll) over the legacy depth-1 pwrite.
	 * Small-block O_DIRECT workloads (fio --bs=4k --numjobs=N
	 * --ioengine=psync) hit this case on every iteration because
	 * each fio thread serially waits for its own write to
	 * complete before queueing the next.  Bench shows the io_uring
	 * path is ~24 % slower than legacy here; the fast-path
	 * regression-proofs it.
	 */
	if (n_reqs == 1 && reqs[0]->desc_cnt <= 1) {
		struct io_thread_req *req = reqs[0];

		for (j = 0; !req->error && j < req->desc_cnt; j++)
			do_io(req, &req->io_desc[j]);
		return;
	}

	/*
	 * Phase 1: submit every request's SQEs.  The submission loop
	 * itself harvests CQEs from earlier reqs whenever the ring is
	 * full, so the maximum in-flight is bounded by UBD_RING_DEPTH.
	 */
	for (i = 0; i < n_reqs; i++)
		ubd_submit_req_async(reqs[i]);

	/* Phase 2: drain remaining in-flight SQEs from any req. */
	ubd_ring_drain_all();

	/*
	 * Phase 5 (memo #2): batched COW bitmap drain through the ring.
	 * Submit one IORING_OP_WRITE per cow_offset != -1 segment, drain
	 * together at the end.  For non-COW UBD images every cow_offset
	 * is -1 so the submission loop is a no-op and the drain returns
	 * immediately; for COW images this cuts N sync pwrite syscalls
	 * per batch down to one io_uring_enter + the per-CQE drain.
	 *
	 * The slot table is fully free at this point (the main drain
	 * above zeroed in_flight).  ubd_submit_with_harvest handles ring
	 * pressure by harvesting one CQE when the slot pool is full.
	 */
	for (i = 0; i < n_reqs; i++) {
		struct io_thread_req *req = reqs[i];
		int op = req_op(req->req);

		if (op != REQ_OP_READ && op != REQ_OP_WRITE)
			continue;
		if (req->error)
			continue;
		for (j = 0; j < req->desc_cnt; j++) {
			struct io_desc *d = &req->io_desc[j];
			int slot;

			if (d->cow_offset == -1)
				continue;
			slot = ubd_submit_with_harvest(
				req, REQ_OP_WRITE, req->fds[1],
				(char *)&d->bitmap_words,
				sizeof(d->bitmap_words),
				d->cow_offset);
			if (slot < 0 && !req->error)
				req->error = map_error(-slot);
		}
	}
	ubd_ring_drain_all();
}

static void do_io_ring(struct io_thread_req *req)
{
	int submitted = 0, harvested = 0;
	int i, op;
	u64 base_offset;

	op = req_op(req->req);
	if (op != REQ_OP_READ && op != REQ_OP_WRITE) {
		/* FLUSH / DISCARD / WRITE_ZEROES — sync fallback. */
		for (i = 0; !req->error && i < req->desc_cnt; i++)
			do_io(req, &req->io_desc[i]);
		return;
	}

	/*
	 * Mirror legacy do_io()'s req->offset += len advance across
	 * io_desc[] entries — otherwise multi-bvec requests would all
	 * target the same on-disk offset.  Local snapshot so we don't
	 * mutate req->offset in flight.
	 */
	base_offset = req->offset;

	/*
	 * Phase 3 fast-path: one writev/readv for the whole request
	 * when every desc is uniform (no COW overlay, no sector mask).
	 */
	if (ubd_req_vectored_eligible(req)) {
		int slot;

		for (;;) {
			slot = ubd_ring_submit_vectored(op, req, base_offset);
			if (slot >= 0) {
				submitted++;
				break;
			}
			if (slot != -EAGAIN) {
				req->error = map_error(-slot);
				goto harvest;
			}
			/* Ring full — wait for any CQE and retry. */
			{
				struct os_io_cqe cqe;
				int hrc = os_io_ring_wait_cqe(ubd_ring,
							      &cqe, -1);

				if (hrc < 0) {
					req->error = map_error(-hrc);
					goto harvest;
				}
				if (hrc == 1) {
					/* Spurious CQE before our submit;
					 * the harvest loop below is the
					 * only entity that touches slots,
					 * so just release this one.
					 */
					ubd_slot_free(cqe.user_data);
				}
			}
		}
		/* Mirror per-desc base_offset advance for the whole req. */
		for (i = 0; i < req->desc_cnt; i++)
			base_offset += req->io_desc[i].length;
		/* Skip per-desc loop — vectored path covers all descs. */
		goto harvest;
	}

	for (i = 0; !req->error && i < req->desc_cnt; i++) {
		struct io_desc *d = &req->io_desc[i];
		int nsectors = d->length / req->sectorsize;
		int start = 0;
		unsigned long last_len = 0;

		do {
			int bit = ubd_test_bit(start,
				(unsigned char *)&d->sector_mask);
			int end = start;
			__u64 off;
			unsigned long len;
			char *buf;
			int slot;

			while (end < nsectors &&
			       ubd_test_bit(end,
				(unsigned char *)&d->sector_mask) == bit)
				end++;

			off = base_offset + req->offsets[bit] +
			      start * req->sectorsize;
			len = (end - start) * req->sectorsize;
			last_len = len;
			buf = d->buffer
			      ? &d->buffer[start * req->sectorsize]
			      : NULL;

			for (;;) {
				slot = ubd_ring_submit_one(req, op,
					req->fds[bit], buf, len, off);
				if (slot >= 0) {
					submitted++;
					break;
				}
				if (slot != -EAGAIN) {
					req->error = map_error(-slot);
					goto harvest;
				}
				/* Ring full — harvest one CQE and retry. */
				{
					struct os_io_cqe cqe;
					int hrc = os_io_ring_wait_cqe(
						ubd_ring, &cqe, -1);
					if (hrc < 0) {
						req->error = map_error(-hrc);
						goto harvest;
					}
					if (hrc == 1) {
						struct ubd_pending_slot *s =
						   &ubd_slots[cqe.user_data];
						if (cqe.res < 0) {
							req->error =
							  map_error(-cqe.res);
							ubd_slot_free(
							  cqe.user_data);
							harvested++;
						} else if (cqe.res == 0) {
							if (s->op == REQ_OP_READ
							    && s->buf)
								memset(
								 s->buf + s->done,
								 0,
								 s->total - s->done);
							ubd_slot_free(
							  cqe.user_data);
							harvested++;
						} else {
							s->done += cqe.res;
							if (s->done < s->total) {
								int rc =
								 ubd_ring_resubmit(
								  cqe.user_data);
								if (rc < 0) {
									req->error =
									 map_error(-rc);
									ubd_slot_free(
									 cqe.user_data);
									harvested++;
								}
							} else {
								ubd_slot_free(
								  cqe.user_data);
								harvested++;
							}
						}
					}
				}
			}
			start = end;
		} while (start < nsectors);

		base_offset += last_len;
	}

harvest:
	while (harvested < submitted) {
		struct os_io_cqe cqe;
		int hrc = os_io_ring_wait_cqe(ubd_ring, &cqe, -1);
		struct ubd_pending_slot *s;

		if (hrc < 0) {
			req->error = map_error(-hrc);
			break;
		}
		if (hrc == 0)
			continue;
		s = &ubd_slots[cqe.user_data];
		if (cqe.res < 0) {
			if (!req->error)
				req->error = map_error(-cqe.res);
			ubd_slot_free(cqe.user_data);
			harvested++;
		} else if (cqe.res == 0) {
			if (s->op == REQ_OP_READ && s->buf)
				memset(s->buf + s->done, 0,
				       s->total - s->done);
			ubd_slot_free(cqe.user_data);
			harvested++;
		} else {
			s->done += cqe.res;
			if (s->done < s->total) {
				int rc = ubd_ring_resubmit(cqe.user_data);
				if (rc < 0) {
					req->error = map_error(-rc);
					ubd_slot_free(cqe.user_data);
					harvested++;
				}
			} else {
				ubd_slot_free(cqe.user_data);
				harvested++;
			}
		}
	}

	req->offset = base_offset;

	/* Bitmap update stays synchronous (memo §Phase 5). */
	for (i = 0; !req->error && i < req->desc_cnt; i++)
		req->error = update_bitmap(req, &req->io_desc[i]);
}

void *io_thread(void *arg)
{
	int n, count, written, res;

	os_fix_helper_thread_signals();

	while(1){
		n = bulk_req_safe_read(
			kernel_fd,
			io_req_buffer,
			&io_remainder,
			&io_remainder_size,
			UBD_REQ_BUFFER_SIZE
		);
		if (n <= 0) {
			if (n == -EAGAIN)
				ubd_read_poll(-1);

			continue;
		}

		if (ubd_ring) {
			/* Phase 2b (memo #2): cross-req parallelism — all
			 * reqs in the bulk batch overlap on the ring.
			 */
			io_count += n / sizeof(struct io_thread_req *);
			do_io_ring_batch(io_req_buffer,
					 n / sizeof(struct io_thread_req *));
		} else {
			for (count = 0;
			     count < n / sizeof(struct io_thread_req *);
			     count++) {
				struct io_thread_req *req = io_req_buffer[count];
				int i;

				io_count++;
				for (i = 0; !req->error && i < req->desc_cnt;
				     i++)
					do_io(req, &(req->io_desc[i]));
			}
		}

		written = 0;

		do {
			res = os_write_file(kernel_fd,
					    ((char *) io_req_buffer) + written,
					    n - written);
			if (res >= 0) {
				written += res;
			}
			if (written < n) {
				ubd_write_poll(-1);
			}
		} while (written < n);
	}

	return NULL;
}
