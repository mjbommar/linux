/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_FS_HOSTFS
#define __UM_FS_HOSTFS

#include <os.h>
#include <generated/asm-offsets.h>

struct hostfs_timespec {
	long long tv_sec;
	long long tv_nsec;
};

struct hostfs_iattr {
	unsigned int		ia_valid;
	unsigned short		ia_mode;
	uid_t			ia_uid;
	gid_t			ia_gid;
	loff_t			ia_size;
	struct hostfs_timespec	ia_atime;
	struct hostfs_timespec	ia_mtime;
	struct hostfs_timespec	ia_ctime;
};

struct hostfs_stat {
	unsigned long long ino;
	unsigned int mode;
	unsigned int nlink;
	unsigned int uid;
	unsigned int gid;
	unsigned long long size;
	struct hostfs_timespec atime, mtime, ctime, btime;
	unsigned int blksize;
	unsigned long long blocks;
	struct {
		unsigned int maj;
		unsigned int min;
	} rdev, dev;
};

extern int stat_file(const char *path, struct hostfs_stat *p, int fd);
extern int access_file(char *path, int r, int w, int x);
extern int open_file(char *path, int r, int w, int append);

/*
 * Per Documentation/virt/uml/redesign/06-sequencing/post-2026-05-19-
 * next-sprint/03-hostfs-io-uring-openat2.md (memo #3, Phase 1):
 *
 * open_file_strict() resolves @rel via openat2(root_fd, rel, ...) with
 * RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS. Symlink-escape from the
 * hostfs mount root is rejected (-ELOOP / -EXDEV).
 *
 * Caller opens the hostfs mount root once at fill_super with O_PATH
 * | O_DIRECTORY and passes its fd as @root_fd here.  @rel must be
 * the path relative to that root (leading "/" tolerated; openat2
 * with RESOLVE_BENEATH treats the root_fd as the new "/" anyway).
 *
 * Returns the opened fd on success, -errno on failure.  In particular
 * returns -ENOSYS / -EINVAL if the host kernel doesn't expose
 * openat2 (< 5.6); callers fall back to open_file() in that case.
 */
extern int open_file_strict(int root_fd, const char *rel,
			    int r, int w, int append);

/*
 * Open @path with O_PATH | O_DIRECTORY | O_CLOEXEC for use as the
 * @root_fd anchor in open_file_strict().  Returns the fd or -errno.
 */
extern int open_root_path(const char *path);
extern void *open_dir(char *path, int *err_out);
extern void seek_dir(void *stream, unsigned long long pos);
extern char *read_dir(void *stream, unsigned long long *pos_out,
		      unsigned long long *ino_out, int *len_out,
		      unsigned int *type_out);
extern void close_file(void *stream);
extern int replace_file(int oldfd, int fd);
extern void close_dir(void *stream);
extern int read_file(int fd, unsigned long long *offset, char *buf, int len);
extern int write_file(int fd, unsigned long long *offset, const char *buf,
		      int len);
extern int lseek_file(int fd, long long offset, int whence);
extern int fsync_file(int fd, int datasync);
extern int file_create(char *name, int mode);
extern int set_attr(const char *file, struct hostfs_iattr *attrs, int fd);
extern int make_symlink(const char *from, const char *to);
extern int unlink_file(const char *file);
extern int do_mkdir(const char *file, int mode);
extern int hostfs_do_rmdir(const char *file);
extern int do_mknod(const char *file, int mode, unsigned int major,
		    unsigned int minor);
extern int link_file(const char *to, const char *from);
extern int hostfs_do_readlink(char *file, char *buf, int size);
extern int rename_file(char *from, char *to);
extern int rename2_file(char *from, char *to, unsigned int flags);
extern int do_statfs(char *root, long *bsize_out, long long *blocks_out,
		     long long *bfree_out, long long *bavail_out,
		     long long *files_out, long long *ffree_out,
		     void *fsid_out, int fsid_size, long *namelen_out);

#endif
