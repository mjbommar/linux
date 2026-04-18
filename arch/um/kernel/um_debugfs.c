// SPDX-License-Identifier: GPL-2.0
/*
 * UML debugfs controls for Layer 2 static-key gates.
 *
 * Mounts at /sys/kernel/debug/um/ when debugfs is available on the
 * running host kernel. Layout:
 *
 *   /sys/kernel/debug/um/
 *   ├── backend                 (ro) current backend name
 *   ├── hooks/<gate>            (rw) 0|1 — flip the static key
 *   └── stats                   (ro) per-gate hit counter (YAML)
 *
 * Writes to hooks/<gate> call static_branch_enable/disable on the
 * gate. Reads return the current static-key state (literal 0 or 1
 * plus a newline).
 *
 * Root-only (mode 0600 on each file) because KCOV coverage and
 * record/replay event streams can be sensitive. Matches the
 * kernel's general debugfs discipline.
 *
 * This entire TU compiles to nothing unless CONFIG_DEBUG_FS=y. The
 * sandbox profile (which sets CONFIG_DEBUG_FS=n) therefore gets the
 * gates themselves but no runtime-flip interface — exactly what B-03
 * §Q1 calls for.
 */

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include <asm/backend.h>
#include <asm/um-hooks.h>
#include <shared/backend.h>

static struct dentry *um_debugfs_root;

/* --- backend (read-only) ------------------------------------------- */

static int um_backend_show(struct seq_file *m, void *v)
{
	if (um_backend && um_backend->name)
		seq_printf(m, "%s\n", um_backend->name);
	else
		seq_puts(m, "(uninitialized)\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(um_backend);

/* --- stats (read-only) --------------------------------------------- */

static int um_stats_show(struct seq_file *m, void *v)
{
	enum um_hook_id id;

	for (id = 0; id < UM_HOOK__COUNT; id++) {
		const char *name = um_hook_name(id);
		struct static_key_false *key = um_hook_key(id);
		bool on = key && static_key_enabled(&key->key);

		seq_printf(m, "%-20s on=%d hits=%llu\n",
			   name ? name : "?",
			   on ? 1 : 0,
			   um_hook_stats_read(id));
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(um_stats);

/* --- per-gate rw files --------------------------------------------- */

static int um_hook_get(void *data, u64 *val)
{
	struct static_key_false *key = data;

	*val = static_key_enabled(&key->key) ? 1 : 0;
	return 0;
}

static int um_hook_set(void *data, u64 val)
{
	struct static_key_false *key = data;

	if (val > 1)
		return -EINVAL;

	if (val)
		static_branch_enable(key);
	else
		static_branch_disable(key);

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(um_hook_fops, um_hook_get, um_hook_set,
			 "%llu\n");

/* --- init / teardown ------------------------------------------------ */

static int __init um_debugfs_init(void)
{
	struct dentry *hooks_dir;
	enum um_hook_id id;

	if (!debugfs_initialized())
		return 0;

	um_debugfs_root = debugfs_create_dir("um", NULL);
	if (IS_ERR(um_debugfs_root)) {
		int err = PTR_ERR(um_debugfs_root);

		um_debugfs_root = NULL;
		return err;
	}

	debugfs_create_file("backend", 0400, um_debugfs_root, NULL,
			    &um_backend_fops);

	debugfs_create_file("stats", 0400, um_debugfs_root, NULL,
			    &um_stats_fops);

	hooks_dir = debugfs_create_dir("hooks", um_debugfs_root);
	if (IS_ERR(hooks_dir))
		hooks_dir = NULL;

	if (hooks_dir) {
		for (id = 0; id < UM_HOOK__COUNT; id++) {
			const char *name = um_hook_name(id);
			struct static_key_false *key = um_hook_key(id);

			if (!name || !key)
				continue;

			debugfs_create_file_unsafe(name, 0600, hooks_dir, key,
						   &um_hook_fops);
		}
	}

	return 0;
}
late_initcall(um_debugfs_init);
