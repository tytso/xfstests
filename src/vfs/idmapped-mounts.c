// SPDX-License-Identifier: GPL-2.0
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../global.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <linux/limits.h>
#include <linux/types.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <stdbool.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "missing.h"
#include "utils.h"

static char t_buf[PATH_MAX];

int tcore_acls(const struct vfstest_info *info)
{
	int fret = -1;
	int dir1_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	if (mkdirat(info->t_dir1_fd, DIR1, 0777)) {
		log_stderr("failure: mkdirat");
		goto out;
	}
	if (fchmodat(info->t_dir1_fd, DIR1, 0777, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	if (mkdirat(info->t_dir1_fd, DIR2, 0777)) {
		log_stderr("failure: mkdirat");
		goto out;
	}
	if (fchmodat(info->t_dir1_fd, DIR2, 0777, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd = get_userns_fd(100010, 100020, 5);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, DIR1,
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	if (sys_move_mount(open_tree_fd, "", info->t_dir1_fd, DIR2, MOVE_MOUNT_F_EMPTY_PATH)) {
		log_stderr("failure: sys_move_mount");
		goto out;
	}

	dir1_fd = openat(info->t_dir1_fd, DIR1, O_DIRECTORY | O_CLOEXEC);
	if (dir1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	if (mkdirat(dir1_fd, DIR3, 0000)) {
		log_stderr("failure: mkdirat");
		goto out;
	}
	if (fchown(dir1_fd, 100010, 100010)) {
		log_stderr("failure: fchown");
		goto out;
	}
	if (fchmod(dir1_fd, 0777)) {
		log_stderr("failure: fchmod");
		goto out;
	}

	snprintf(t_buf, sizeof(t_buf), "setfacl -m u:100010:rwx %s/%s/%s/%s", info->t_mountpoint, T_DIR1, DIR1, DIR3);
	if (system(t_buf)) {
		log_stderr("failure: system");
		goto out;
	}

	snprintf(t_buf, sizeof(t_buf), "getfacl -p %s/%s/%s/%s | grep -q user:100010:rwx", info->t_mountpoint, T_DIR1, DIR1, DIR3);
	if (system(t_buf)) {
		log_stderr("failure: system");
		goto out;
	}

	snprintf(t_buf, sizeof(t_buf), "getfacl -p %s/%s/%s/%s | grep -q user:100020:rwx", info->t_mountpoint, T_DIR1, DIR2, DIR3);
	if (system(t_buf)) {
		log_stderr("failure: system");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 100010, 100010, true))
			die("failure: switch_userns");

		snprintf(t_buf, sizeof(t_buf), "getfacl -p %s/%s/%s/%s | grep -q user:%lu:rwx",
			 info->t_mountpoint, T_DIR1, DIR1, DIR3, 4294967295LU);
		if (system(t_buf))
			die("failure: system");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 100010, 100010, true))
			die("failure: switch_userns");

		snprintf(t_buf, sizeof(t_buf), "getfacl -p %s/%s/%s/%s | grep -q user:%lu:rwx",
			 info->t_mountpoint, T_DIR1, DIR2, DIR3, 100010LU);
		if (system(t_buf))
			die("failure: system");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	/* Now, dir is owned by someone else in the user namespace, but we can
	 * still read it because of acls.
	 */
	if (fchown(dir1_fd, 100012, 100012)) {
		log_stderr("failure: fchown");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		int fd;

		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 100010, 100010, true))
			die("failure: switch_userns");

		fd = openat(open_tree_fd, DIR3, O_CLOEXEC | O_DIRECTORY);
		if (fd < 0)
			die("failure: openat");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	/* if we delete the acls, the ls should fail because it's 700. */
	snprintf(t_buf, sizeof(t_buf), "%s/%s/%s/%s", info->t_mountpoint, T_DIR1, DIR1, DIR3);
	if (removexattr(t_buf, "system.posix_acl_access")) {
		log_stderr("failure: removexattr");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		int fd;

		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 100010, 100010, true))
			die("failure: switch_userns");

		fd = openat(open_tree_fd, DIR3, O_CLOEXEC | O_DIRECTORY);
		if (fd >= 0)
			die("failure: openat");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	snprintf(t_buf, sizeof(t_buf), "%s/" T_DIR1 "/" DIR2, info->t_mountpoint);
	sys_umount2(t_buf, MNT_DETACH);

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(dir1_fd);
	safe_close(open_tree_fd);

	return fret;
}

/* Validate that basic file operations on idmapped mounts from a user namespace. */
int tcore_create_in_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	/* change ownership of all files to uid 0 */
	if (chown_r(info->t_mnt_fd, T_DIR1, 0, 0)) {
		log_stderr("failure: chown_r");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		/* create regular file via open() */
		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
		if (file1_fd < 0)
			die("failure: open file");
		safe_close(file1_fd);

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 0))
			die("failure: check ownership");

		/* create regular file via mknod */
		if (mknodat(open_tree_fd, FILE2, S_IFREG | 0000, 0))
			die("failure: create");

		if (!expected_uid_gid(open_tree_fd, FILE2, 0, 0, 0))
			die("failure: check ownership");

		/* create symlink */
		if (symlinkat(FILE2, open_tree_fd, SYMLINK1))
			die("failure: create");

		if (!expected_uid_gid(open_tree_fd, SYMLINK1, AT_SYMLINK_NOFOLLOW, 0, 0))
			die("failure: check ownership");

		/* create directory */
		if (mkdirat(open_tree_fd, DIR1, 0700))
			die("failure: create");

		if (!expected_uid_gid(open_tree_fd, DIR1, 0, 0, 0))
			die("failure: check ownership");

		/* try to rename a file */
		if (renameat(open_tree_fd, FILE1, open_tree_fd, FILE1_RENAME))
			die("failure: create");

		if (!expected_uid_gid(open_tree_fd, FILE1_RENAME, 0, 0, 0))
			die("failure: check ownership");

		/* try to rename a file */
		if (renameat(open_tree_fd, DIR1, open_tree_fd, DIR1_RENAME))
			die("failure: create");

		if (!expected_uid_gid(open_tree_fd, DIR1_RENAME, 0, 0, 0))
			die("failure: check ownership");

		/* remove file */
		if (unlinkat(open_tree_fd, FILE1_RENAME, 0))
			die("failure: remove");

		/* remove directory */
		if (unlinkat(open_tree_fd, DIR1_RENAME, AT_REMOVEDIR))
			die("failure: remove");

		exit(EXIT_SUCCESS);
	}

	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

/* Validate that a caller whose fsids map into the idmapped mount within it's
 * user namespace cannot create any device nodes.
 */
int tcore_device_node_in_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		/* create character device */
		if (!mknodat(open_tree_fd, CHRDEV1, S_IFCHR | 0644, makedev(5, 1)))
			die("failure: create");

		exit(EXIT_SUCCESS);
	}

	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(open_tree_fd);

	return fret;
}

int tcore_fsids_mapped(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, hardlink_target_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	if (!caps_supported())
		return 0;

	/* create hardlink target */
	hardlink_target_fd = openat(info->t_dir1_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (hardlink_target_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	/* create directory for rename test */
	if (mkdirat(info->t_dir1_fd, DIR1, 0700)) {
		log_stderr("failure: mkdirat");
		goto out;
	}

	/* change ownership of all files to uid 0 */
	if (chown_r(info->t_mnt_fd, T_DIR1, 0, 0)) {
		log_stderr("failure: chown_r");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_fsids(10000, 10000))
			die("failure: switch fsids");

		if (!caps_up())
			die("failure: raise caps");

		/* The caller's fsids now have mappings in the idmapped mount so
		 * any file creation must fail.
		 */

		/* create hardlink */
		if (linkat(open_tree_fd, FILE1, open_tree_fd, HARDLINK1, 0))
			die("failure: create hardlink");

		/* try to rename a file */
		if (renameat(open_tree_fd, FILE1, open_tree_fd, FILE1_RENAME))
			die("failure: rename");

		/* try to rename a directory */
		if (renameat(open_tree_fd, DIR1, open_tree_fd, DIR1_RENAME))
			die("failure: rename");

		/* remove file */
		if (unlinkat(open_tree_fd, FILE1_RENAME, 0))
			die("failure: delete");

		/* remove directory */
		if (unlinkat(open_tree_fd, DIR1_RENAME, AT_REMOVEDIR))
			die("failure: delete");

		/* The caller's fsids have mappings in the idmapped mount so any
		 * file creation must fail.
		 */

		/* create regular file via open() */
		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
		if (file1_fd < 0)
			die("failure: create");

		/* create regular file via mknod */
		if (mknodat(open_tree_fd, FILE2, S_IFREG | 0000, 0))
			die("failure: create");

		/* create character device */
		if (mknodat(open_tree_fd, CHRDEV1, S_IFCHR | 0644, makedev(0, 0)))
			die("failure: create");

		/* create symlink */
		if (symlinkat(FILE2, open_tree_fd, SYMLINK1))
			die("failure: create");

		/* create directory */
		if (mkdirat(open_tree_fd, DIR1, 0700))
			die("failure: create");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(hardlink_target_fd);
	safe_close(open_tree_fd);

	return fret;
}

/* Validate that basic file operations on idmapped mounts. */
int tcore_fsids_unmapped(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, hardlink_target_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};

	/* create hardlink target */
	hardlink_target_fd = openat(info->t_dir1_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (hardlink_target_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	/* create directory for rename test */
	if (mkdirat(info->t_dir1_fd, DIR1, 0700)) {
		log_stderr("failure: mkdirat");
		goto out;
	}

	/* change ownership of all files to uid 0 */
	if (chown_r(info->t_mnt_fd, T_DIR1, 0, 0)) {
		log_stderr("failure: chown_r");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	if (!switch_fsids(0, 0)) {
		log_stderr("failure: switch_fsids");
		goto out;
	}

	/* The caller's fsids don't have a mappings in the idmapped mount so any
	 * file creation must fail.
	 */

	/* create hardlink */
	if (!linkat(open_tree_fd, FILE1, open_tree_fd, HARDLINK1, 0)) {
		log_stderr("failure: linkat");
		goto out;
	}
	if (errno != EOVERFLOW) {
		log_stderr("failure: errno");
		goto out;
	}

	/* try to rename a file */
	if (!renameat(open_tree_fd, FILE1, open_tree_fd, FILE1_RENAME)) {
		log_stderr("failure: renameat");
		goto out;
	}
	if (errno != EOVERFLOW) {
		log_stderr("failure: errno");
		goto out;
	}

	/* try to rename a directory */
	if (!renameat(open_tree_fd, DIR1, open_tree_fd, DIR1_RENAME)) {
		log_stderr("failure: renameat");
		goto out;
	}
	if (errno != EOVERFLOW) {
		log_stderr("failure: errno");
		goto out;
	}

	/* The caller is privileged over the inode so file deletion must work. */

	/* remove file */
	if (unlinkat(open_tree_fd, FILE1, 0)) {
		log_stderr("failure: unlinkat");
		goto out;
	}

	/* remove directory */
	if (unlinkat(open_tree_fd, DIR1, AT_REMOVEDIR)) {
		log_stderr("failure: unlinkat");
		goto out;
	}

	/* The caller's fsids don't have a mappings in the idmapped mount so
	 * any file creation must fail.
	 */

	/* create regular file via open() */
	file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (file1_fd >= 0) {
		log_stderr("failure: create");
		goto out;
	}
	if (errno != EOVERFLOW) {
		log_stderr("failure: errno");
		goto out;
	}

	/* create regular file via mknod */
	if (!mknodat(open_tree_fd, FILE2, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (errno != EOVERFLOW) {
		log_stderr("failure: errno");
		goto out;
	}

	/* create character device */
	if (!mknodat(open_tree_fd, CHRDEV1, S_IFCHR | 0644, makedev(5, 1))) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (errno != EOVERFLOW) {
		log_stderr("failure: errno");
		goto out;
	}

	/* create symlink */
	if (!symlinkat(FILE2, open_tree_fd, SYMLINK1)) {
		log_stderr("failure: symlinkat");
		goto out;
	}
	if (errno != EOVERFLOW) {
		log_stderr("failure: errno");
		goto out;
	}

	/* create directory */
	if (!mkdirat(open_tree_fd, DIR1, 0700)) {
		log_stderr("failure: mkdirat");
		goto out;
	}
	if (errno != EOVERFLOW) {
		log_stderr("failure: errno");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(hardlink_target_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

/* Validate that changing file ownership works correctly on idmapped mounts. */
int tcore_expected_uid_gid_idmapped_mounts(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd1 = -EBADF, open_tree_fd2 = -EBADF;
	struct mount_attr attr1 = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	struct mount_attr attr2 = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	if (!switch_fsids(0, 0)) {
		log_stderr("failure: switch_fsids");
		goto out;
	}

	/* create regular file via open() */
	file1_fd = openat(info->t_dir1_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(info->t_dir1_fd, FILE2, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}

	/* create character device */
	if (mknodat(info->t_dir1_fd, CHRDEV1, S_IFCHR | 0644, makedev(0, 0))) {
		log_stderr("failure: mknodat");
		goto out;
	}

	/* create hardlink */
	if (linkat(info->t_dir1_fd, FILE1, info->t_dir1_fd, HARDLINK1, 0)) {
		log_stderr("failure: linkat");
		goto out;
	}

	/* create symlink */
	if (symlinkat(FILE2, info->t_dir1_fd, SYMLINK1)) {
		log_stderr("failure: symlinkat");
		goto out;
	}

	/* create directory */
	if (mkdirat(info->t_dir1_fd, DIR1, 0700)) {
		log_stderr("failure: mkdirat");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr1.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr1.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd1 = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd1 < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd1, "", AT_EMPTY_PATH, &attr1, sizeof(attr1))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	/* Validate that all files created through the image mountpoint are
	 * owned by the callers fsuid and fsgid.
	 */
	if (!expected_uid_gid(info->t_dir1_fd, FILE1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, FILE2, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, HARDLINK1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, CHRDEV1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, SYMLINK1, AT_SYMLINK_NOFOLLOW, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, SYMLINK1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, DIR1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	/* Validate that all files are owned by the uid and gid specified in
	 * the idmapping of the mount they are accessed from.
	 */
	if (!expected_uid_gid(open_tree_fd1, FILE1, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, FILE2, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, HARDLINK1, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, CHRDEV1, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, SYMLINK1, AT_SYMLINK_NOFOLLOW, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, SYMLINK1, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, DIR1, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr2.userns_fd	= get_userns_fd(0, 30000, 2001);
	if (attr2.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd2 = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd2 < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd2, "", AT_EMPTY_PATH, &attr2, sizeof(attr2))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	/* Validate that all files are owned by the uid and gid specified in
	 * the idmapping of the mount they are accessed from.
	 */
	if (!expected_uid_gid(open_tree_fd2, FILE1, 0, 30000, 30000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, FILE2, 0, 30000, 30000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, HARDLINK1, 0, 30000, 30000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, CHRDEV1, 0, 30000, 30000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, SYMLINK1, AT_SYMLINK_NOFOLLOW, 30000, 30000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, SYMLINK1, 0, 30000, 30000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, DIR1, 0, 30000, 30000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	/* Change ownership throught original image mountpoint. */
	if (fchownat(info->t_dir1_fd, FILE1, 2000, 2000, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchownat(info->t_dir1_fd, FILE2, 2000, 2000, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchownat(info->t_dir1_fd, HARDLINK1, 2000, 2000, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchownat(info->t_dir1_fd, CHRDEV1, 2000, 2000, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchownat(info->t_dir1_fd, SYMLINK1, 3000, 3000, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchownat(info->t_dir1_fd, SYMLINK1, 2000, 2000, AT_EMPTY_PATH)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchownat(info->t_dir1_fd, DIR1, 2000, 2000, AT_EMPTY_PATH)) {
		log_stderr("failure: fchownat");
		goto out;
	}

	/* Check ownership through original mount. */
	if (!expected_uid_gid(info->t_dir1_fd, FILE1, 0, 2000, 2000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, FILE2, 0, 2000, 2000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, HARDLINK1, 0, 2000, 2000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, CHRDEV1, 0, 2000, 2000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, SYMLINK1, AT_SYMLINK_NOFOLLOW, 3000, 3000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, SYMLINK1, 0, 2000, 2000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, DIR1, 0, 2000, 2000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	/* Check ownership through first idmapped mount. */
	if (!expected_uid_gid(open_tree_fd1, FILE1, 0, 12000, 12000)) {
		log_stderr("failure:expected_uid_gid ");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, FILE2, 0, 12000, 12000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, HARDLINK1, 0, 12000, 12000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, CHRDEV1, 0, 12000, 12000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, SYMLINK1, AT_SYMLINK_NOFOLLOW, 13000, 13000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, SYMLINK1, 0, 12000, 12000)) {
		log_stderr("failure:expected_uid_gid ");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, DIR1, 0, 12000, 12000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	/* Check ownership through second idmapped mount. */
	if (!expected_uid_gid(open_tree_fd2, FILE1, 0, 32000, 32000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, FILE2, 0, 32000, 32000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, HARDLINK1, 0, 32000, 32000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, CHRDEV1, 0, 32000, 32000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, SYMLINK1, AT_SYMLINK_NOFOLLOW, info->t_overflowuid, info->t_overflowgid)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, SYMLINK1, 0, 32000, 32000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, DIR1, 0, 32000, 32000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr1.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		if (!fchownat(info->t_dir1_fd, FILE1, 1000, 1000, 0))
			die("failure: fchownat");
		if (!fchownat(info->t_dir1_fd, FILE2, 1000, 1000, 0))
			die("failure: fchownat");
		if (!fchownat(info->t_dir1_fd, HARDLINK1, 1000, 1000, 0))
			die("failure: fchownat");
		if (!fchownat(info->t_dir1_fd, CHRDEV1, 1000, 1000, 0))
			die("failure: fchownat");
		if (!fchownat(info->t_dir1_fd, SYMLINK1, 2000, 2000, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
			die("failure: fchownat");
		if (!fchownat(info->t_dir1_fd, SYMLINK1, 1000, 1000, AT_EMPTY_PATH))
			die("failure: fchownat");
		if (!fchownat(info->t_dir1_fd, DIR1, 1000, 1000, AT_EMPTY_PATH))
			die("failure: fchownat");

		if (!fchownat(open_tree_fd2, FILE1, 1000, 1000, 0))
			die("failure: fchownat");
		if (!fchownat(open_tree_fd2, FILE2, 1000, 1000, 0))
			die("failure: fchownat");
		if (!fchownat(open_tree_fd2, HARDLINK1, 1000, 1000, 0))
			die("failure: fchownat");
		if (!fchownat(open_tree_fd2, CHRDEV1, 1000, 1000, 0))
			die("failure: fchownat");
		if (!fchownat(open_tree_fd2, SYMLINK1, 2000, 2000, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
			die("failure: fchownat");
		if (!fchownat(open_tree_fd2, SYMLINK1, 1000, 1000, AT_EMPTY_PATH))
			die("failure: fchownat");
		if (!fchownat(open_tree_fd2, DIR1, 1000, 1000, AT_EMPTY_PATH))
			die("failure: fchownat");

		if (fchownat(open_tree_fd1, FILE1, 1000, 1000, 0))
			die("failure: fchownat");
		if (fchownat(open_tree_fd1, FILE2, 1000, 1000, 0))
			die("failure: fchownat");
		if (fchownat(open_tree_fd1, HARDLINK1, 1000, 1000, 0))
			die("failure: fchownat");
		if (fchownat(open_tree_fd1, CHRDEV1, 1000, 1000, 0))
			die("failure: fchownat");
		if (fchownat(open_tree_fd1, SYMLINK1, 2000, 2000, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
			die("failure: fchownat");
		if (fchownat(open_tree_fd1, SYMLINK1, 1000, 1000, AT_EMPTY_PATH))
			die("failure: fchownat");
		if (fchownat(open_tree_fd1, DIR1, 1000, 1000, AT_EMPTY_PATH))
			die("failure: fchownat");

		if (!expected_uid_gid(info->t_dir1_fd, FILE1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(info->t_dir1_fd, FILE2, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(info->t_dir1_fd, HARDLINK1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(info->t_dir1_fd, CHRDEV1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(info->t_dir1_fd, SYMLINK1, AT_SYMLINK_NOFOLLOW, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(info->t_dir1_fd, SYMLINK1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(info->t_dir1_fd, DIR1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");

		if (!expected_uid_gid(open_tree_fd2, FILE1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd2, FILE2, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd2, HARDLINK1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd2, CHRDEV1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd2, SYMLINK1, AT_SYMLINK_NOFOLLOW, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd2, SYMLINK1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd2, DIR1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");

		if (!expected_uid_gid(open_tree_fd1, FILE1, 0, 1000, 1000))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd1, FILE2, 0, 1000, 1000))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd1, HARDLINK1, 0, 1000, 1000))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd1, CHRDEV1, 0, 1000, 1000))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd1, SYMLINK1, AT_SYMLINK_NOFOLLOW, 2000, 2000))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd1, SYMLINK1, 0, 1000, 1000))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd1, DIR1, 0, 1000, 1000))
			die("failure: expected_uid_gid");

		exit(EXIT_SUCCESS);
	}

	if (wait_for_pid(pid))
		goto out;

	/* Check ownership through original mount. */
	if (!expected_uid_gid(info->t_dir1_fd, FILE1, 0, 1000, 1000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, FILE2, 0, 1000, 1000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, HARDLINK1, 0, 1000, 1000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, CHRDEV1, 0, 1000, 1000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, SYMLINK1, AT_SYMLINK_NOFOLLOW, 2000, 2000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, SYMLINK1, 0, 1000, 1000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, DIR1, 0, 1000, 1000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	/* Check ownership through first idmapped mount. */
	if (!expected_uid_gid(open_tree_fd1, FILE1, 0, 11000, 11000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, FILE2, 0, 11000, 11000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, HARDLINK1, 0, 11000, 11000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, CHRDEV1, 0, 11000, 11000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, SYMLINK1, AT_SYMLINK_NOFOLLOW, 12000, 12000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, SYMLINK1, 0, 11000, 11000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, DIR1, 0, 11000, 11000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	/* Check ownership through second idmapped mount. */
	if (!expected_uid_gid(open_tree_fd2, FILE1, 0, 31000, 31000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, FILE2, 0, 31000, 31000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, HARDLINK1, 0, 31000, 31000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, CHRDEV1, 0, 31000, 31000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, SYMLINK1, AT_SYMLINK_NOFOLLOW, 32000, 32000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, SYMLINK1, 0, 31000, 31000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, DIR1, 0, 31000, 31000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr2.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		if (!fchownat(info->t_dir1_fd, FILE1, 0, 0, 0))
			die("failure: fchownat");
		if (!fchownat(info->t_dir1_fd, FILE2, 0, 0, 0))
			die("failure: fchownat");
		if (!fchownat(info->t_dir1_fd, HARDLINK1, 0, 0, 0))
			die("failure: fchownat");
		if (!fchownat(info->t_dir1_fd, CHRDEV1, 0, 0, 0))
			die("failure: fchownat");
		if (!fchownat(info->t_dir1_fd, SYMLINK1, 3000, 3000, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
			die("failure: fchownat");
		if (!fchownat(info->t_dir1_fd, SYMLINK1, 0, 0, AT_EMPTY_PATH))
			die("failure: fchownat");
		if (!fchownat(info->t_dir1_fd, DIR1, 0, 0, AT_EMPTY_PATH))
			die("failure: fchownat");

		if (!fchownat(open_tree_fd1, FILE1, 0, 0, 0))
			die("failure: fchownat");
		if (!fchownat(open_tree_fd1, FILE2, 0, 0, 0))
			die("failure: fchownat");
		if (!fchownat(open_tree_fd1, HARDLINK1, 0, 0, 0))
			die("failure: fchownat");
		if (!fchownat(open_tree_fd1, CHRDEV1, 0, 0, 0))
			die("failure: fchownat");
		if (!fchownat(open_tree_fd1, SYMLINK1, 3000, 3000, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
			die("failure: fchownat");
		if (!fchownat(open_tree_fd1, SYMLINK1, 0, 0, AT_EMPTY_PATH))
			die("failure: fchownat");
		if (!fchownat(open_tree_fd1, DIR1, 0, 0, AT_EMPTY_PATH))
			die("failure: fchownat");

		if (fchownat(open_tree_fd2, FILE1, 0, 0, 0))
			die("failure: fchownat");
		if (fchownat(open_tree_fd2, FILE2, 0, 0, 0))
			die("failure: fchownat");
		if (fchownat(open_tree_fd2, HARDLINK1, 0, 0, 0))
			die("failure: fchownat");
		if (fchownat(open_tree_fd2, CHRDEV1, 0, 0, 0))
			die("failure: fchownat");
		if (!fchownat(open_tree_fd2, SYMLINK1, 3000, 3000, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
			die("failure: fchownat");
		if (fchownat(open_tree_fd2, SYMLINK1, 0, 0, AT_EMPTY_PATH))
			die("failure: fchownat");
		if (fchownat(open_tree_fd2, DIR1, 0, 0, AT_EMPTY_PATH))
			die("failure: fchownat");

		if (!expected_uid_gid(info->t_dir1_fd, FILE1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(info->t_dir1_fd, FILE2, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(info->t_dir1_fd, HARDLINK1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(info->t_dir1_fd, CHRDEV1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(info->t_dir1_fd, SYMLINK1, AT_SYMLINK_NOFOLLOW, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(info->t_dir1_fd, SYMLINK1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(info->t_dir1_fd, DIR1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");

		if (!expected_uid_gid(open_tree_fd1, FILE1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd1, FILE2, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd1, HARDLINK1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd1, CHRDEV1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd1, SYMLINK1, AT_SYMLINK_NOFOLLOW, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd1, SYMLINK1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd1, DIR1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");

		if (!expected_uid_gid(open_tree_fd2, FILE1, 0, 0, 0))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd2, FILE2, 0, 0, 0))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd2, HARDLINK1, 0, 0, 0))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd2, CHRDEV1, 0, 0, 0))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd2, SYMLINK1, AT_SYMLINK_NOFOLLOW, 2000, 2000))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd2, SYMLINK1, 0, 0, 0))
			die("failure: expected_uid_gid");
		if (!expected_uid_gid(open_tree_fd2, DIR1, 0, 0, 0))
			die("failure: expected_uid_gid");

		exit(EXIT_SUCCESS);
	}

	if (wait_for_pid(pid))
		goto out;

	/* Check ownership through original mount. */
	if (!expected_uid_gid(info->t_dir1_fd, FILE1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, FILE2, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, HARDLINK1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, CHRDEV1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, SYMLINK1, AT_SYMLINK_NOFOLLOW, 2000, 2000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, SYMLINK1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(info->t_dir1_fd, DIR1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	/* Check ownership through first idmapped mount. */
	if (!expected_uid_gid(open_tree_fd1, FILE1, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, FILE2, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, HARDLINK1, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, CHRDEV1, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, SYMLINK1, AT_SYMLINK_NOFOLLOW, 12000, 12000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, SYMLINK1, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd1, DIR1, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	/* Check ownership through second idmapped mount. */
	if (!expected_uid_gid(open_tree_fd2, FILE1, 0, 30000, 30000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, FILE2, 0, 30000, 30000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, HARDLINK1, 0, 30000, 30000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, CHRDEV1, 0, 30000, 30000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, SYMLINK1, AT_SYMLINK_NOFOLLOW, 32000, 32000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, SYMLINK1, 0, 30000, 30000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(open_tree_fd2, DIR1, 0, 30000, 30000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr1.userns_fd);
	safe_close(attr2.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd1);
	safe_close(open_tree_fd2);

	return fret;
}

int tcore_fscaps_idmapped_mounts(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, file1_fd2 = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	file1_fd = openat(info->t_dir1_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	/* Skip if vfs caps are unsupported. */
	if (set_dummy_vfs_caps(file1_fd, 0, 1000))
		return 0;

	if (fremovexattr(file1_fd, "security.capability")) {
		log_stderr("failure: fremovexattr");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	file1_fd2 = openat(open_tree_fd, FILE1, O_RDWR | O_CLOEXEC, 0);
	if (file1_fd2 < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	if (!set_dummy_vfs_caps(file1_fd2, 0, 1000)) {
		log_stderr("failure: set_dummy_vfs_caps");
		goto out;
	}

	if (set_dummy_vfs_caps(file1_fd2, 0, 10000)) {
		log_stderr("failure: set_dummy_vfs_caps");
		goto out;
	}

	if (!expected_dummy_vfs_caps_uid(file1_fd2, 10000)) {
		log_stderr("failure: expected_dummy_vfs_caps_uid");
		goto out;
	}

	if (!expected_dummy_vfs_caps_uid(file1_fd, 0)) {
		log_stderr("failure: expected_dummy_vfs_caps_uid");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		if (!expected_dummy_vfs_caps_uid(file1_fd2, 0))
			die("failure: expected_dummy_vfs_caps_uid");

		exit(EXIT_SUCCESS);
	}

	if (wait_for_pid(pid))
		goto out;

	if (fremovexattr(file1_fd2, "security.capability")) {
		log_stderr("failure: fremovexattr");
		goto out;
	}
	if (expected_dummy_vfs_caps_uid(file1_fd2, -1)) {
		log_stderr("failure: expected_dummy_vfs_caps_uid");
		goto out;
	}
	if (errno != ENODATA) {
		log_stderr("failure: errno");
		goto out;
	}

	if (set_dummy_vfs_caps(file1_fd2, 0, 12000)) {
		log_stderr("failure: set_dummy_vfs_caps");
		goto out;
	}

	if (!expected_dummy_vfs_caps_uid(file1_fd2, 12000)) {
		log_stderr("failure: expected_dummy_vfs_caps_uid");
		goto out;
	}

	if (!expected_dummy_vfs_caps_uid(file1_fd, 2000)) {
		log_stderr("failure: expected_dummy_vfs_caps_uid");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		if (!expected_dummy_vfs_caps_uid(file1_fd2, 2000))
			die("failure: expected_dummy_vfs_caps_uid");

		exit(EXIT_SUCCESS);
	}

	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(file1_fd2);
	safe_close(open_tree_fd);

	return fret;
}

int tcore_fscaps_idmapped_mounts_in_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, file1_fd2 = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	file1_fd = openat(info->t_dir1_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	/* Skip if vfs caps are unsupported. */
	if (set_dummy_vfs_caps(file1_fd, 0, 1000))
		return 0;

	if (fremovexattr(file1_fd, "security.capability")) {
		log_stderr("failure: fremovexattr");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	file1_fd2 = openat(open_tree_fd, FILE1, O_RDWR | O_CLOEXEC, 0);
	if (file1_fd2 < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		if (expected_dummy_vfs_caps_uid(file1_fd2, -1))
			die("failure: expected_dummy_vfs_caps_uid");
		if (errno != ENODATA)
			die("failure: errno");

		if (set_dummy_vfs_caps(file1_fd2, 0, 1000))
			die("failure: set_dummy_vfs_caps");

		if (!expected_dummy_vfs_caps_uid(file1_fd2, 1000))
			die("failure: expected_dummy_vfs_caps_uid");

		if (!expected_dummy_vfs_caps_uid(file1_fd, 1000) && errno != EOVERFLOW)
			die("failure: expected_dummy_vfs_caps_uid");

		exit(EXIT_SUCCESS);
	}

	if (wait_for_pid(pid))
		goto out;

	if (!expected_dummy_vfs_caps_uid(file1_fd, 1000)) {
		log_stderr("failure: expected_dummy_vfs_caps_uid");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(file1_fd2);
	safe_close(open_tree_fd);

	return fret;
}

static int fscaps_idmapped_mounts_in_userns_valid_in_ancestor_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, file1_fd2 = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	file1_fd = openat(info->t_dir1_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	/* Skip if vfs caps are unsupported. */
	if (set_dummy_vfs_caps(file1_fd, 0, 1000))
		return 0;

	if (fremovexattr(file1_fd, "security.capability")) {
		log_stderr("failure: fremovexattr");
		goto out;
	}
	if (expected_dummy_vfs_caps_uid(file1_fd, -1)) {
		log_stderr("failure: expected_dummy_vfs_caps_uid");
		goto out;
	}
	if (errno != ENODATA) {
		log_stderr("failure: errno");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	file1_fd2 = openat(open_tree_fd, FILE1, O_RDWR | O_CLOEXEC, 0);
	if (file1_fd2 < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	/*
	 * Verify we can set an v3 fscap for real root this was regressed at
	 * some point. Make sure this doesn't happen again!
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		if (expected_dummy_vfs_caps_uid(file1_fd2, -1))
			die("failure: expected_dummy_vfs_caps_uid");
		if (errno != ENODATA)
			die("failure: errno");

		if (set_dummy_vfs_caps(file1_fd2, 0, 0))
			die("failure: set_dummy_vfs_caps");

		if (!expected_dummy_vfs_caps_uid(file1_fd2, 0))
			die("failure: expected_dummy_vfs_caps_uid");

		if (!expected_dummy_vfs_caps_uid(file1_fd, 0) && errno != EOVERFLOW)
			die("failure: expected_dummy_vfs_caps_uid");

		exit(EXIT_SUCCESS);
	}

	if (wait_for_pid(pid))
		goto out;

	if (!expected_dummy_vfs_caps_uid(file1_fd2, 10000)) {
		log_stderr("failure: expected_dummy_vfs_caps_uid");
		goto out;
	}

	if (!expected_dummy_vfs_caps_uid(file1_fd, 0)) {
		log_stderr("failure: expected_dummy_vfs_caps_uid");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(file1_fd2);
	safe_close(open_tree_fd);

	return fret;
}

int tcore_fscaps_idmapped_mounts_in_userns_separate_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, file1_fd2 = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	file1_fd = openat(info->t_dir1_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	/* Skip if vfs caps are unsupported. */
	if (set_dummy_vfs_caps(file1_fd, 0, 1000)) {
		log_stderr("failure: set_dummy_vfs_caps");
		goto out;
	}

	if (fremovexattr(file1_fd, "security.capability")) {
		log_stderr("failure: fremovexattr");
		goto out;
	}

	/* change ownership of all files to uid 0 */
	if (chown_r(info->t_mnt_fd, T_DIR1, 20000, 20000)) {
		log_stderr("failure: chown_r");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(20000, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	file1_fd2 = openat(open_tree_fd, FILE1, O_RDWR | O_CLOEXEC, 0);
	if (file1_fd2 < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		int userns_fd;

		userns_fd = get_userns_fd(0, 10000, 10000);
		if (userns_fd < 0)
			die("failure: get_userns_fd");

		if (!switch_userns(userns_fd, 0, 0, false))
			die("failure: switch_userns");

		if (set_dummy_vfs_caps(file1_fd2, 0, 0))
			die("failure: set fscaps");

		if (!expected_dummy_vfs_caps_uid(file1_fd2, 0))
			die("failure: expected_dummy_vfs_caps_uid");

		if (!expected_dummy_vfs_caps_uid(file1_fd, 20000) && errno != EOVERFLOW)
			die("failure: expected_dummy_vfs_caps_uid");

		exit(EXIT_SUCCESS);
	}

	if (wait_for_pid(pid))
		goto out;

	if (!expected_dummy_vfs_caps_uid(file1_fd, 20000)) {
		log_stderr("failure: expected_dummy_vfs_caps_uid");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		int userns_fd;

		userns_fd = get_userns_fd(0, 10000, 10000);
		if (userns_fd < 0)
			die("failure: get_userns_fd");

		if (!switch_userns(userns_fd, 0, 0, false))
			die("failure: switch_userns");

		if (fremovexattr(file1_fd2, "security.capability"))
			die("failure: fremovexattr");
		if (expected_dummy_vfs_caps_uid(file1_fd2, -1))
			die("failure: expected_dummy_vfs_caps_uid");
		if (errno != ENODATA)
			die("failure: errno");

		if (set_dummy_vfs_caps(file1_fd2, 0, 1000))
			die("failure: set_dummy_vfs_caps");

		if (!expected_dummy_vfs_caps_uid(file1_fd2, 1000))
			die("failure: expected_dummy_vfs_caps_uid");

		if (!expected_dummy_vfs_caps_uid(file1_fd, 21000) && errno != EOVERFLOW)
			die("failure: expected_dummy_vfs_caps_uid");

		exit(EXIT_SUCCESS);
	}

	if (wait_for_pid(pid))
		goto out;

	if (!expected_dummy_vfs_caps_uid(file1_fd, 21000)) {
		log_stderr("failure: expected_dummy_vfs_caps_uid");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(file1_fd2);
	safe_close(open_tree_fd);

	return fret;
}

int tcore_hardlink_crossing_idmapped_mounts(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd1 = -EBADF, open_tree_fd2 = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};

	if (chown_r(info->t_mnt_fd, T_DIR1, 10000, 10000)) {
		log_stderr("failure: chown_r");
		goto out;
	}

	attr.userns_fd	= get_userns_fd(10000, 0, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd1 = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd1 < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd1, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	file1_fd = openat(open_tree_fd1, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	if (!expected_uid_gid(open_tree_fd1, FILE1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	if (!expected_uid_gid(info->t_dir1_fd, FILE1, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	safe_close(file1_fd);

	if (mkdirat(open_tree_fd1, DIR1, 0777)) {
		log_stderr("failure: mkdirat");
		goto out;
	}

	open_tree_fd2 = sys_open_tree(info->t_dir1_fd, DIR1,
				      AT_NO_AUTOMOUNT |
				      AT_SYMLINK_NOFOLLOW |
				      OPEN_TREE_CLOEXEC |
				      OPEN_TREE_CLONE |
				      AT_RECURSIVE);
	if (open_tree_fd2 < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd2, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	/* We're crossing a mountpoint so this must fail.
	 *
	 * Note that this must also fail for non-idmapped mounts but here we're
	 * interested in making sure we're not introducing an accidental way to
	 * violate that restriction or that suddenly this becomes possible.
	 */
	if (!linkat(open_tree_fd1, FILE1, open_tree_fd2, HARDLINK1, 0)) {
		log_stderr("failure: linkat");
		goto out;
	}
	if (errno != EXDEV) {
		log_stderr("failure: errno");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd1);
	safe_close(open_tree_fd2);

	return fret;
}

int tcore_hardlink_from_idmapped_mount(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};

	if (chown_r(info->t_mnt_fd, T_DIR1, 10000, 10000)) {
		log_stderr("failure: chown_r");
		goto out;
	}

	attr.userns_fd	= get_userns_fd(10000, 0, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}
	safe_close(file1_fd);

	if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	if (!expected_uid_gid(info->t_dir1_fd, FILE1, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	/* We're not crossing a mountpoint so this must succeed. */
	if (linkat(open_tree_fd, FILE1, open_tree_fd, HARDLINK1, 0)) {
		log_stderr("failure: linkat");
		goto out;
	}


	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

int tcore_hardlink_from_idmapped_mount_in_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	if (chown_r(info->t_mnt_fd, T_DIR1, 0, 0)) {
		log_stderr("failure: chown_r");
		goto out;
	}

	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
		if (file1_fd < 0)
			die("failure: create");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 0))
			die("failure: check ownership");

		/* We're not crossing a mountpoint so this must succeed. */
		if (linkat(open_tree_fd, FILE1, open_tree_fd, HARDLINK1, 0))
			die("failure: create");

		if (!expected_uid_gid(open_tree_fd, HARDLINK1, 0, 0, 0))
			die("failure: check ownership");

		exit(EXIT_SUCCESS);
	}

	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}


#ifdef HAVE_LIBURING_H
int tcore_io_uring_idmapped(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	struct io_uring *ring;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	int cred_id, ret;
	pid_t pid;

	ring = mmap(0, sizeof(struct io_uring), PROT_READ|PROT_WRITE,
		   MAP_SHARED | MAP_ANONYMOUS, 0, 0);
	if (!ring)
		return log_errno(-1, "failure: io_uring_queue_init");

	ret = io_uring_queue_init(8, ring, 0);
	if (ret) {
		log_stderr("failure: io_uring_queue_init");
		goto out_unmap;
	}

	ret = io_uring_register_personality(ring);
	if (ret < 0) {
		fret = 0;
		goto out_unmap; /* personalities not supported */
	}
	cred_id = ret;

	/* create file only owner can open */
	file1_fd = openat(info->t_dir1_fd, FILE1, O_RDONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0000);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}
	if (fchown(file1_fd, 0, 0)) {
		log_stderr("failure: fchown");
		goto out;
	}
	if (fchmod(file1_fd, 0600)) {
		log_stderr("failure: fchmod");
		goto out;
	}
	safe_close(file1_fd);

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0)
		return log_errno(-1, "failure: create user namespace");

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0)
		return log_errno(-1, "failure: create detached mount");

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr)))
		return log_errno(-1, "failure: set mount attributes");

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_ids(10000, 10000))
			die("failure: switch_ids");

		file1_fd = io_uring_openat_with_creds(ring, open_tree_fd, FILE1,
						      -1, false, NULL);
		if (file1_fd < 0)
			die("failure: io_uring_open_file");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_ids(10001, 10001))
			die("failure: switch_ids");

		file1_fd = io_uring_openat_with_creds(ring, open_tree_fd, FILE1,
						      cred_id, false, NULL);
		if (file1_fd < 0)
			die("failure: io_uring_open_file");

		file1_fd = io_uring_openat_with_creds(ring, open_tree_fd, FILE1,
						      cred_id, true, NULL);
		if (file1_fd < 0)
			die("failure: io_uring_open_file");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	ret = io_uring_unregister_personality(ring, cred_id);
	if (ret)
		log_stderr("failure: io_uring_unregister_personality");

out_unmap:
	munmap(ring, sizeof(struct io_uring));

	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

/*
 * Create an idmapped mount where the we leave the owner of the file unmapped.
 * In no circumstances, even with recorded credentials can it be allowed to
 * open the file.
 */
int tcore_io_uring_idmapped_unmapped(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	struct io_uring *ring;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	int cred_id, ret, ret_cqe;
	pid_t pid;

	ring = mmap(0, sizeof(struct io_uring), PROT_READ|PROT_WRITE,
		   MAP_SHARED | MAP_ANONYMOUS, 0, 0);
	if (!ring)
		return log_errno(-1, "failure: io_uring_queue_init");

	ret = io_uring_queue_init(8, ring, 0);
	if (ret) {
		log_stderr("failure: io_uring_queue_init");
		goto out_unmap;
	}

	ret = io_uring_register_personality(ring);
	if (ret < 0) {
		fret = 0;
		goto out_unmap; /* personalities not supported */
	}
	cred_id = ret;

	/* create file only owner can open */
	file1_fd = openat(info->t_dir1_fd, FILE1, O_RDONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0000);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}
	if (fchown(file1_fd, 0, 0)) {
		log_stderr("failure: fchown");
		goto out;
	}
	if (fchmod(file1_fd, 0600)) {
		log_stderr("failure: fchmod");
		goto out;
	}
	safe_close(file1_fd);

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(1, 10000, 10000);
	if (attr.userns_fd < 0)
		return log_errno(-1, "failure: create user namespace");

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0)
		return log_errno(-1, "failure: create detached mount");

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr)))
		return log_errno(-1, "failure: set mount attributes");

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_ids(10000, 10000))
			die("failure: switch_ids");

		ret_cqe = 0;
		file1_fd = io_uring_openat_with_creds(ring, open_tree_fd, FILE1,
						      cred_id, false, &ret_cqe);
		if (file1_fd >= 0)
			die("failure: io_uring_open_file");
		if (ret_cqe == 0)
			die("failure: non-open() related io_uring_open_file failure");
		if (ret_cqe != -EACCES)
			die("failure: errno(%d)", abs(ret_cqe));

		ret_cqe = 0;
		file1_fd = io_uring_openat_with_creds(ring, open_tree_fd, FILE1,
						      cred_id, true, &ret_cqe);
		if (file1_fd >= 0)
			die("failure: io_uring_open_file");
		if (ret_cqe == 0)
			die("failure: non-open() related io_uring_open_file failure");
		if (ret_cqe != -EACCES)
			die("failure: errno(%d)", abs(ret_cqe));

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	ret = io_uring_unregister_personality(ring, cred_id);
	if (ret)
		log_stderr("failure: io_uring_unregister_personality");

out_unmap:
	munmap(ring, sizeof(struct io_uring));

	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

int tcore_io_uring_idmapped_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	struct io_uring *ring;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	int cred_id, ret, ret_cqe;
	pid_t pid;

	ring = mmap(0, sizeof(struct io_uring), PROT_READ|PROT_WRITE,
		   MAP_SHARED | MAP_ANONYMOUS, 0, 0);
	if (!ring)
		return log_errno(-1, "failure: io_uring_queue_init");

	ret = io_uring_queue_init(8, ring, 0);
	if (ret) {
		log_stderr("failure: io_uring_queue_init");
		goto out_unmap;
	}

	ret = io_uring_register_personality(ring);
	if (ret < 0) {
		fret = 0;
		goto out_unmap; /* personalities not supported */
	}
	cred_id = ret;

	/* create file only owner can open */
	file1_fd = openat(info->t_dir1_fd, FILE1, O_RDONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0000);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}
	if (fchown(file1_fd, 0, 0)) {
		log_stderr("failure: fchown");
		goto out;
	}
	if (fchmod(file1_fd, 0600)) {
		log_stderr("failure: fchmod");
		goto out;
	}
	safe_close(file1_fd);

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0)
		return log_errno(-1, "failure: create user namespace");

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0)
		return log_errno(-1, "failure: create detached mount");

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr)))
		return log_errno(-1, "failure: set mount attributes");

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		file1_fd = io_uring_openat_with_creds(ring, open_tree_fd, FILE1,
						      -1, false, NULL);
		if (file1_fd < 0)
			die("failure: io_uring_open_file");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 1000, 1000, true))
			die("failure: switch_userns");

		ret_cqe = 0;
		file1_fd = io_uring_openat_with_creds(ring, info->t_dir1_fd, FILE1,
						      -1, false, &ret_cqe);
		if (file1_fd >= 0)
			die("failure: io_uring_open_file");
		if (ret_cqe == 0)
			die("failure: non-open() related io_uring_open_file failure");
		if (ret_cqe != -EACCES)
			die("failure: errno(%d)", abs(ret_cqe));

		ret_cqe = 0;
		file1_fd = io_uring_openat_with_creds(ring, info->t_dir1_fd, FILE1,
						      -1, true, &ret_cqe);
		if (file1_fd >= 0)
			die("failure: io_uring_open_file");
		if (ret_cqe == 0)
			die("failure: non-open() related io_uring_open_file failure");
		if (ret_cqe != -EACCES)
			die("failure: errno(%d)", abs(ret_cqe));

		ret_cqe = 0;
		file1_fd = io_uring_openat_with_creds(ring, open_tree_fd, FILE1,
						      -1, false, &ret_cqe);
		if (file1_fd >= 0)
			die("failure: io_uring_open_file");
		if (ret_cqe == 0)
			die("failure: non-open() related io_uring_open_file failure");
		if (ret_cqe != -EACCES)
			die("failure: errno(%d)", abs(ret_cqe));

		ret_cqe = 0;
		file1_fd = io_uring_openat_with_creds(ring, open_tree_fd, FILE1,
						      -1, true, &ret_cqe);
		if (file1_fd >= 0)
			die("failure: io_uring_open_file");
		if (ret_cqe == 0)
			die("failure: non-open() related io_uring_open_file failure");
		if (ret_cqe != -EACCES)
			die("failure: errno(%d)", abs(ret_cqe));

		file1_fd = io_uring_openat_with_creds(ring, open_tree_fd, FILE1,
						      cred_id, false, NULL);
		if (file1_fd < 0)
			die("failure: io_uring_open_file");

		file1_fd = io_uring_openat_with_creds(ring, open_tree_fd, FILE1,
						      cred_id, true, NULL);
		if (file1_fd < 0)
			die("failure: io_uring_open_file");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	ret = io_uring_unregister_personality(ring, cred_id);
	if (ret)
		log_stderr("failure: io_uring_unregister_personality");

out_unmap:
	munmap(ring, sizeof(struct io_uring));

	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

int tcore_io_uring_idmapped_unmapped_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	struct io_uring *ring;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	int cred_id, ret, ret_cqe;
	pid_t pid;

	ring = mmap(0, sizeof(struct io_uring), PROT_READ|PROT_WRITE,
		   MAP_SHARED | MAP_ANONYMOUS, 0, 0);
	if (!ring)
		return log_errno(-1, "failure: io_uring_queue_init");

	ret = io_uring_queue_init(8, ring, 0);
	if (ret) {
		log_stderr("failure: io_uring_queue_init");
		goto out_unmap;
	}

	ret = io_uring_register_personality(ring);
	if (ret < 0) {
		fret = 0;
		goto out_unmap; /* personalities not supported */
	}
	cred_id = ret;

	/* create file only owner can open */
	file1_fd = openat(info->t_dir1_fd, FILE1, O_RDONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0000);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}
	if (fchown(file1_fd, 0, 0)) {
		log_stderr("failure: fchown");
		goto out;
	}
	if (fchmod(file1_fd, 0600)) {
		log_stderr("failure: fchmod");
		goto out;
	}
	safe_close(file1_fd);

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(1, 10000, 10000);
	if (attr.userns_fd < 0)
		return log_errno(-1, "failure: create user namespace");

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0)
		return log_errno(-1, "failure: create detached mount");

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr)))
		return log_errno(-1, "failure: set mount attributes");

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 10000, 10000, true))
			die("failure: switch_ids");

		ret_cqe = 0;
		file1_fd = io_uring_openat_with_creds(ring, open_tree_fd, FILE1,
						      cred_id, false, &ret_cqe);
		if (file1_fd >= 0)
			die("failure: io_uring_open_file");
		if (ret_cqe == 0)
			die("failure: non-open() related io_uring_open_file failure");
		if (ret_cqe != -EACCES)
			die("failure: errno(%d)", abs(ret_cqe));

		ret_cqe = 0;
		file1_fd = io_uring_openat_with_creds(ring, open_tree_fd, FILE1,
						      cred_id, true, &ret_cqe);
		if (file1_fd >= 0)
			die("failure: io_uring_open_file");
		if (ret_cqe == 0)
			die("failure: non-open() related io_uring_open_file failure");
		if (ret_cqe != -EACCES)
			die("failure: errno(%d)", abs(ret_cqe));

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	ret = io_uring_unregister_personality(ring, cred_id);
	if (ret)
		log_stderr("failure: io_uring_unregister_personality");

out_unmap:
	munmap(ring, sizeof(struct io_uring));

	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}
#endif /* HAVE_LIBURING_H */

/* Validate that protected symlinks work correctly on idmapped mounts. */
int tcore_protected_symlinks_idmapped_mounts(const struct vfstest_info *info)
{
	int fret = -1;
	int dir_fd = -EBADF, fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	if (!protected_symlinks_enabled())
		return 0;

	if (!caps_supported())
		return 0;

	/* create directory */
	if (mkdirat(info->t_dir1_fd, DIR1, 0000)) {
		log_stderr("failure: mkdirat");
		goto out;
	}

	dir_fd = openat(info->t_dir1_fd, DIR1, O_DIRECTORY | O_CLOEXEC);
	if (dir_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}
	if (fchown(dir_fd, 10000, 10000)) {
		log_stderr("failure: fchown");
		goto out;
	}
	if (fchmod(dir_fd, 0777 | S_ISVTX)) {
		log_stderr("failure: fchmod");
		goto out;
	}
	/* validate sticky bit is set */
	if (!is_sticky(info->t_dir1_fd, DIR1, 0)) {
		log_stderr("failure: is_sticky");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE1, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE1, 10000, 10000, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE1, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* create symlinks */
	if (symlinkat(FILE1, dir_fd, SYMLINK_USER1)) {
		log_stderr("failure: symlinkat");
		goto out;
	}
	if (fchownat(dir_fd, SYMLINK_USER1, 10000, 10000, AT_SYMLINK_NOFOLLOW)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (!expected_uid_gid(dir_fd, SYMLINK_USER1, AT_SYMLINK_NOFOLLOW, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(dir_fd, FILE1, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	if (symlinkat(FILE1, dir_fd, SYMLINK_USER2)) {
		log_stderr("failure: symlinkat");
		goto out;
	}
	if (fchownat(dir_fd, SYMLINK_USER2, 11000, 11000, AT_SYMLINK_NOFOLLOW)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (!expected_uid_gid(dir_fd, SYMLINK_USER2, AT_SYMLINK_NOFOLLOW, 11000, 11000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(dir_fd, FILE1, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	if (symlinkat(FILE1, dir_fd, SYMLINK_USER3)) {
		log_stderr("failure: symlinkat");
		goto out;
	}
	if (fchownat(dir_fd, SYMLINK_USER3, 12000, 12000, AT_SYMLINK_NOFOLLOW)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (!expected_uid_gid(dir_fd, SYMLINK_USER3, AT_SYMLINK_NOFOLLOW, 12000, 12000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(dir_fd, FILE1, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(10000, 0, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: open_tree_fd");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	/* validate file can be directly read */
	fd = openat(open_tree_fd, DIR1 "/"  FILE1, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}
	safe_close(fd);

	/* validate file can be read through own symlink */
	fd = openat(open_tree_fd, DIR1 "/" SYMLINK_USER1, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}
	safe_close(fd);

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_ids(1000, 1000))
			die("failure: switch_ids");

		/* validate file can be directly read */
		fd = openat(open_tree_fd, DIR1 "/" FILE1, O_RDONLY | O_CLOEXEC, 0);
		if (fd < 0)
			die("failure: openat");
		safe_close(fd);

		/* validate file can be read through own symlink */
		fd = openat(open_tree_fd, DIR1 "/" SYMLINK_USER2, O_RDONLY | O_CLOEXEC, 0);
		if (fd < 0)
			die("failure: openat");
		safe_close(fd);

		/* validate file can be read through root symlink */
		fd = openat(open_tree_fd, DIR1 "/" SYMLINK_USER1, O_RDONLY | O_CLOEXEC, 0);
		if (fd < 0)
			die("failure: openat");
		safe_close(fd);

		/* validate file can't be read through other users symlink */
		fd = openat(open_tree_fd, DIR1 "/" SYMLINK_USER3, O_RDONLY | O_CLOEXEC, 0);
		if (fd >= 0)
			die("failure: openat");
		if (errno != EACCES)
			die("failure: errno");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_ids(2000, 2000))
			die("failure: switch_ids");

		/* validate file can be directly read */
		fd = openat(open_tree_fd, DIR1 "/" FILE1, O_RDONLY | O_CLOEXEC, 0);
		if (fd < 0)
			die("failure: openat");
		safe_close(fd);

		/* validate file can be read through own symlink */
		fd = openat(open_tree_fd, DIR1 "/" SYMLINK_USER3, O_RDONLY | O_CLOEXEC, 0);
		if (fd < 0)
			die("failure: openat");
		safe_close(fd);

		/* validate file can be read through root symlink */
		fd = openat(open_tree_fd, DIR1 "/" SYMLINK_USER1, O_RDONLY | O_CLOEXEC, 0);
		if (fd < 0)
			die("failure: openat");
		safe_close(fd);

		/* validate file can't be read through other users symlink */
		fd = openat(open_tree_fd, DIR1 "/" SYMLINK_USER2, O_RDONLY | O_CLOEXEC, 0);
		if (fd >= 0)
			die("failure: openat");
		if (errno != EACCES)
			die("failure: errno");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(fd);
	safe_close(dir_fd);
	safe_close(open_tree_fd);

	return fret;
}

/* Validate that protected symlinks work correctly on idmapped mounts inside a
 * user namespace.
 */
int tcore_protected_symlinks_idmapped_mounts_in_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int dir_fd = -EBADF, fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	if (!protected_symlinks_enabled())
		return 0;

	if (!caps_supported())
		return 0;

	/* create directory */
	if (mkdirat(info->t_dir1_fd, DIR1, 0000)) {
		log_stderr("failure: mkdirat");
		goto out;
	}

	dir_fd = openat(info->t_dir1_fd, DIR1, O_DIRECTORY | O_CLOEXEC);
	if (dir_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}
	if (fchown(dir_fd, 0, 0)) {
		log_stderr("failure: fchown");
		goto out;
	}
	if (fchmod(dir_fd, 0777 | S_ISVTX)) {
		log_stderr("failure: fchmod");
		goto out;
	}
	/* validate sticky bit is set */
	if (!is_sticky(info->t_dir1_fd, DIR1, 0)) {
		log_stderr("failure: is_sticky");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE1, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE1, 0, 0, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE1, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* create symlinks */
	if (symlinkat(FILE1, dir_fd, SYMLINK_USER1)) {
		log_stderr("failure: symlinkat");
		goto out;
	}
	if (fchownat(dir_fd, SYMLINK_USER1, 0, 0, AT_SYMLINK_NOFOLLOW)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (!expected_uid_gid(dir_fd, SYMLINK_USER1, AT_SYMLINK_NOFOLLOW, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(dir_fd, FILE1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	if (symlinkat(FILE1, dir_fd, SYMLINK_USER2)) {
		log_stderr("failure: symlinkat");
		goto out;
	}
	if (fchownat(dir_fd, SYMLINK_USER2, 1000, 1000, AT_SYMLINK_NOFOLLOW)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (!expected_uid_gid(dir_fd, SYMLINK_USER2, AT_SYMLINK_NOFOLLOW, 1000, 1000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(dir_fd, FILE1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	if (symlinkat(FILE1, dir_fd, SYMLINK_USER3)) {
		log_stderr("failure: symlinkat");
		goto out;
	}
	if (fchownat(dir_fd, SYMLINK_USER3, 2000, 2000, AT_SYMLINK_NOFOLLOW)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (!expected_uid_gid(dir_fd, SYMLINK_USER3, AT_SYMLINK_NOFOLLOW, 2000, 2000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}
	if (!expected_uid_gid(dir_fd, FILE1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	/* validate file can be directly read */
	fd = openat(open_tree_fd, DIR1 "/" FILE1, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}
	safe_close(fd);

	/* validate file can be read through own symlink */
	fd = openat(open_tree_fd, DIR1 "/" SYMLINK_USER1, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}
	safe_close(fd);

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 1000, 1000, true))
			die("failure: switch_userns");

		/* validate file can be directly read */
		fd = openat(open_tree_fd, DIR1 "/" FILE1, O_RDONLY | O_CLOEXEC, 0);
		if (fd < 0)
			die("failure: openat");
		safe_close(fd);

		/* validate file can be read through own symlink */
		fd = openat(open_tree_fd, DIR1 "/" SYMLINK_USER2, O_RDONLY | O_CLOEXEC, 0);
		if (fd < 0)
			die("failure: openat");
		safe_close(fd);

		/* validate file can be read through root symlink */
		fd = openat(open_tree_fd, DIR1 "/" SYMLINK_USER1, O_RDONLY | O_CLOEXEC, 0);
		if (fd < 0)
			die("failure: openat");
		safe_close(fd);

		/* validate file can't be read through other users symlink */
		fd = openat(open_tree_fd, DIR1 "/" SYMLINK_USER3, O_RDONLY | O_CLOEXEC, 0);
		if (fd >= 0)
			die("failure: openat");
		if (errno != EACCES)
			die("failure: errno");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 2000, 2000, true))
			die("failure: switch_userns");

		/* validate file can be directly read */
		fd = openat(open_tree_fd, DIR1 "/" FILE1, O_RDONLY | O_CLOEXEC, 0);
		if (fd < 0)
			die("failure: openat");
		safe_close(fd);

		/* validate file can be read through own symlink */
		fd = openat(open_tree_fd, DIR1 "/" SYMLINK_USER3, O_RDONLY | O_CLOEXEC, 0);
		if (fd < 0)
			die("failure: openat");
		safe_close(fd);

		/* validate file can be read through root symlink */
		fd = openat(open_tree_fd, DIR1 "/" SYMLINK_USER1, O_RDONLY | O_CLOEXEC, 0);
		if (fd < 0)
			die("failure: openat");
		safe_close(fd);

		/* validate file can't be read through other users symlink */
		fd = openat(open_tree_fd, DIR1 "/" SYMLINK_USER2, O_RDONLY | O_CLOEXEC, 0);
		if (fd >= 0)
			die("failure: openat");
		if (errno != EACCES)
			die("failure: errno");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(dir_fd);
	safe_close(open_tree_fd);
	safe_close(attr.userns_fd);

	return fret;
}

int tcore_rename_crossing_idmapped_mounts(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd1 = -EBADF, open_tree_fd2 = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};

	if (chown_r(info->t_mnt_fd, T_DIR1, 10000, 10000)) {
		log_stderr("failure: chown_r");
		goto out;
	}

	attr.userns_fd	= get_userns_fd(10000, 0, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd1 = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd1 < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd1, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	file1_fd = openat(open_tree_fd1, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	if (!expected_uid_gid(open_tree_fd1, FILE1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	if (!expected_uid_gid(info->t_dir1_fd, FILE1, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	if (mkdirat(open_tree_fd1, DIR1, 0777)) {
		log_stderr("failure: mkdirat");
		goto out;
	}

	open_tree_fd2 = sys_open_tree(info->t_dir1_fd, DIR1,
				      AT_NO_AUTOMOUNT |
				      AT_SYMLINK_NOFOLLOW |
				      OPEN_TREE_CLOEXEC |
				      OPEN_TREE_CLONE |
				      AT_RECURSIVE);
	if (open_tree_fd2 < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd2, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	/* We're crossing a mountpoint so this must fail.
	 *
	 * Note that this must also fail for non-idmapped mounts but here we're
	 * interested in making sure we're not introducing an accidental way to
	 * violate that restriction or that suddenly this becomes possible.
	 */
	if (!renameat(open_tree_fd1, FILE1, open_tree_fd2, FILE1_RENAME)) {
		log_stderr("failure: renameat");
		goto out;
	}
	if (errno != EXDEV) {
		log_stderr("failure: errno");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd1);
	safe_close(open_tree_fd2);

	return fret;
}

int tcore_rename_from_idmapped_mount(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};

	if (chown_r(info->t_mnt_fd, T_DIR1, 10000, 10000)) {
		log_stderr("failure: chown_r");
		goto out;
	}

	attr.userns_fd	= get_userns_fd(10000, 0, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	if (!expected_uid_gid(info->t_dir1_fd, FILE1, 0, 10000, 10000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	/* We're not crossing a mountpoint so this must succeed. */
	if (renameat(open_tree_fd, FILE1, open_tree_fd, FILE1_RENAME)) {
		log_stderr("failure: renameat");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

int tcore_rename_from_idmapped_mount_in_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	pid_t pid;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};

	if (chown_r(info->t_mnt_fd, T_DIR1, 0, 0)) {
		log_stderr("failure: chown_r");
		goto out;
	}

	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
		if (file1_fd < 0)
			die("failure: create");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 0))
			die("failure: check ownership");

		/* We're not crossing a mountpoint so this must succeed. */
		if (renameat(open_tree_fd, FILE1, open_tree_fd, FILE1_RENAME))
			die("failure: create");

		if (!expected_uid_gid(open_tree_fd, FILE1_RENAME, 0, 0, 0))
			die("failure: check ownership");

		exit(EXIT_SUCCESS);
	}

	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

int tcore_setattr_truncate_idmapped(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	pid_t pid;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_ids(10000, 10000))
			die("failure: switch_ids");

		/* create regular file via open() */
		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, S_IXGRP | S_ISGID);
		if (file1_fd < 0)
			die("failure: create");

		if (ftruncate(file1_fd, 10000))
			die("failure: ftruncate");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 10000, 10000))
			die("failure: check ownership");

		if (!expected_file_size(open_tree_fd, FILE1, 0, 10000))
			die("failure: expected_file_size");

		if (ftruncate(file1_fd, 0))
			die("failure: ftruncate");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 10000, 10000))
			die("failure: check ownership");

		if (!expected_file_size(open_tree_fd, FILE1, 0, 0))
			die("failure: expected_file_size");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		int file1_fd2 = -EBADF;

		/* create regular file via open() */
		file1_fd2 = openat(open_tree_fd, FILE1, O_RDWR | O_CLOEXEC, S_IXGRP | S_ISGID);
		if (file1_fd2 < 0)
			die("failure: create");

		if (ftruncate(file1_fd2, 10000))
			die("failure: ftruncate");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 10000, 10000))
			die("failure: check ownership");

		if (!expected_file_size(open_tree_fd, FILE1, 0, 10000))
			die("failure: expected_file_size");

		if (ftruncate(file1_fd2, 0))
			die("failure: ftruncate");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 10000, 10000))
			die("failure: check ownership");

		if (!expected_file_size(open_tree_fd, FILE1, 0, 0))
			die("failure: expected_file_size");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

int tcore_setattr_truncate_idmapped_in_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		/* create regular file via open() */
		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, S_IXGRP | S_ISGID);
		if (file1_fd < 0)
			die("failure: create");

		if (ftruncate(file1_fd, 10000))
			die("failure: ftruncate");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 0))
			die("failure: check ownership");

		if (!expected_file_size(open_tree_fd, FILE1, 0, 10000))
			die("failure: expected_file_size");

		if (ftruncate(file1_fd, 0))
			die("failure: ftruncate");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 0))
			die("failure: check ownership");

		if (!expected_file_size(open_tree_fd, FILE1, 0, 0))
			die("failure: expected_file_size");

		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: delete");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	if (fchownat(info->t_dir1_fd, "", -1, 1000, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
		log_stderr("failure: fchownat");
		goto out;
	}

	if (fchownat(info->t_dir1_fd, "", -1, 1000, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
		log_stderr("failure: fchownat");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 0, 0, true))
			die("failure: switch_userns");

		/* create regular file via open() */
		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, S_IXGRP | S_ISGID);
		if (file1_fd < 0)
			die("failure: create");

		if (ftruncate(file1_fd, 10000))
			die("failure: ftruncate");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 0))
			die("failure: check ownership");

		if (!expected_file_size(open_tree_fd, FILE1, 0, 10000))
			die("failure: expected_file_size");

		if (ftruncate(file1_fd, 0))
			die("failure: ftruncate");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 0))
			die("failure: check ownership");

		if (!expected_file_size(open_tree_fd, FILE1, 0, 0))
			die("failure: expected_file_size");

		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: delete");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	if (fchownat(info->t_dir1_fd, "", -1, 0, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
		log_stderr("failure: fchownat");
		goto out;
	}

	if (fchownat(info->t_dir1_fd, "", -1, 0, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
		log_stderr("failure: fchownat");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 0, 1000, true))
			die("failure: switch_userns");

		/* create regular file via open() */
		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, S_IXGRP | S_ISGID);
		if (file1_fd < 0)
			die("failure: create");

		if (ftruncate(file1_fd, 10000))
			die("failure: ftruncate");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 1000))
			die("failure: check ownership");

		if (!expected_file_size(open_tree_fd, FILE1, 0, 10000))
			die("failure: expected_file_size");

		if (ftruncate(file1_fd, 0))
			die("failure: ftruncate");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 1000))
			die("failure: check ownership");

		if (!expected_file_size(open_tree_fd, FILE1, 0, 0))
			die("failure: expected_file_size");

		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: delete");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

int tcore_setgid_create_idmapped(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;
	int tmpfile_fd = -EBADF;
	bool supported = false;
	char path[PATH_MAX];

	if (!caps_supported())
		return 0;

	if (fchmod(info->t_dir1_fd, S_IRUSR |
			      S_IWUSR |
			      S_IRGRP |
			      S_IWGRP |
			      S_IROTH |
			      S_IWOTH |
			      S_IXUSR |
			      S_IXGRP |
			      S_IXOTH |
			      S_ISGID), 0) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* Verify that the sid bits got raised. */
	if (!is_setgid(info->t_dir1_fd, "", AT_EMPTY_PATH)) {
		log_stderr("failure: is_setgid");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	supported = openat_tmpfile_supported(open_tree_fd);

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_ids(10000, 11000))
			die("failure: switch fsids");

		/* create regular file via open() */
		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, S_IXGRP | S_ISGID);
		if (file1_fd < 0)
			die("failure: create");

		/* Neither in_group_p() nor capable_wrt_inode_uidgid() so setgid
		 * bit needs to be stripped.
		 */
		if (is_setgid(open_tree_fd, FILE1, 0))
			die("failure: is_setgid");

		/* create directory */
		if (mkdirat(open_tree_fd, DIR1, 0000))
			die("failure: create");

		if (xfs_irix_sgid_inherit_enabled(info->t_fstype)) {
			/* We're not in_group_p(). */
			if (is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		} else {
			/* Directories always inherit the setgid bit. */
			if (!is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		}

		/* create a special file via mknodat() vfs_create */
		if (mknodat(open_tree_fd, FILE2, S_IFREG | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, FILE2, 0))
			die("failure: is_setgid");

		/* create a whiteout device via mknodat() vfs_mknod */
		if (mknodat(open_tree_fd, CHRDEV1, S_IFCHR | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, CHRDEV1, 0))
			die("failure: is_setgid");

		/*
		 * In setgid directories newly created files always inherit the
		 * gid from the parent directory. Verify that the file is owned
		 * by gid 10000, not by gid 11000.
		 */
		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 10000, 10000))
			die("failure: check ownership");

		/*
		 * In setgid directories newly created directories always
		 * inherit the gid from the parent directory. Verify that the
		 * directory is owned by gid 10000, not by gid 11000.
		 */
		if (!expected_uid_gid(open_tree_fd, DIR1, 0, 10000, 10000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, FILE2, 0, 10000, 10000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, CHRDEV1, 0, 10000, 10000))
			die("failure: check ownership");

		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, DIR1, AT_REMOVEDIR))
			die("failure: delete");

		if (unlinkat(open_tree_fd, FILE2, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, CHRDEV1, 0))
			die("failure: delete");

		/* create tmpfile via filesystem tmpfile api */
		if (supported) {
			tmpfile_fd = openat(open_tree_fd, ".", O_TMPFILE | O_RDWR, S_IXGRP | S_ISGID);
			if (tmpfile_fd < 0)
				die("failure: create");
			/* link the temporary file into the filesystem, making it permanent */
			snprintf(path, PATH_MAX,  "/proc/self/fd/%d", tmpfile_fd);
			if (linkat(AT_FDCWD, path, open_tree_fd, FILE3, AT_SYMLINK_FOLLOW))
				die("failure: linkat");
			if (close(tmpfile_fd))
				die("failure: close");
			if (is_setgid(open_tree_fd, FILE3, 0))
				die("failure: is_setgid");
			if (!expected_uid_gid(open_tree_fd, FILE3, 0, 10000, 10000))
				die("failure: check ownership");
			if (unlinkat(open_tree_fd, FILE3, 0))
				die("failure: delete");
		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

int tcore_setgid_create_idmapped_in_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;
	int tmpfile_fd = -EBADF;
	bool supported = false;
	char path[PATH_MAX];

	if (!caps_supported())
		return 0;

	if (fchmod(info->t_dir1_fd, S_IRUSR |
			      S_IWUSR |
			      S_IRGRP |
			      S_IWGRP |
			      S_IROTH |
			      S_IWOTH |
			      S_IXUSR |
			      S_IXGRP |
			      S_IXOTH |
			      S_ISGID), 0) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* Verify that the sid bits got raised. */
	if (!is_setgid(info->t_dir1_fd, "", AT_EMPTY_PATH)) {
		log_stderr("failure: is_setgid");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	supported = openat_tmpfile_supported(open_tree_fd);

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		/* create regular file via open() */
		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, S_IXGRP | S_ISGID);
		if (file1_fd < 0)
			die("failure: create");

		/* We're in_group_p() and capable_wrt_inode_uidgid() so setgid
		 * bit needs to be set.
		 */
		if (!is_setgid(open_tree_fd, FILE1, 0))
			die("failure: is_setgid");

		/* create directory */
		if (mkdirat(open_tree_fd, DIR1, 0000))
			die("failure: create");

		/* Directories always inherit the setgid bit. */
		if (!is_setgid(open_tree_fd, DIR1, 0))
			die("failure: is_setgid");

		/* create a special file via mknodat() vfs_create */
		if (mknodat(open_tree_fd, FILE2, S_IFREG | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (!is_setgid(open_tree_fd, FILE2, 0))
			die("failure: is_setgid");

		/* create a whiteout device via mknodat() vfs_mknod */
		if (mknodat(open_tree_fd, CHRDEV1, S_IFCHR | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (!is_setgid(open_tree_fd, CHRDEV1, 0))
			die("failure: is_setgid");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 0))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, DIR1, 0, 0, 0))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, FILE2, 0, 0, 0))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, CHRDEV1, 0, 0, 0))
			die("failure: check ownership");

		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, DIR1, AT_REMOVEDIR))
			die("failure: delete");

		if (unlinkat(open_tree_fd, FILE2, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, CHRDEV1, 0))
			die("failure: delete");

		/* create tmpfile via filesystem tmpfile api */
		if (supported) {
			tmpfile_fd = openat(open_tree_fd, ".", O_TMPFILE | O_RDWR, S_IXGRP | S_ISGID);
			if (tmpfile_fd < 0)
				die("failure: create");
			/* link the temporary file into the filesystem, making it permanent */
			snprintf(path, PATH_MAX,  "/proc/self/fd/%d", tmpfile_fd);
			if (linkat(AT_FDCWD, path, open_tree_fd, FILE3, AT_SYMLINK_FOLLOW))
				die("failure: linkat");
			if (close(tmpfile_fd))
				die("failure: close");
			if (!is_setgid(open_tree_fd, FILE3, 0))
				die("failure: is_setgid");
			if (!expected_uid_gid(open_tree_fd, FILE3, 0, 0, 0))
				die("failure: check ownership");
			if (unlinkat(open_tree_fd, FILE3, 0))
				die("failure: delete");
		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	/*
	 * Below we verify that setgid inheritance for a newly created file or
	 * directory works correctly. As part of this we need to verify that
	 * newly created files or directories inherit their gid from their
	 * parent directory. So we change the parent directorie's gid to 1000
	 * and create a file with fs{g,u}id 0 and verify that the newly created
	 * file and directory inherit gid 1000, not 0.
	 */
	if (fchownat(info->t_dir1_fd, "", -1, 1000, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
		log_stderr("failure: fchownat");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		if (!caps_down_fsetid())
			die("failure: caps_down_fsetid");

		/* create regular file via open() */
		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, S_IXGRP | S_ISGID);
		if (file1_fd < 0)
			die("failure: create");

		/* Neither in_group_p() nor capable_wrt_inode_uidgid() so setgid
		 * bit needs to be stripped.
		 */
		if (is_setgid(open_tree_fd, FILE1, 0))
			die("failure: is_setgid");

		/* create directory */
		if (mkdirat(open_tree_fd, DIR1, 0000))
			die("failure: create");

		if (xfs_irix_sgid_inherit_enabled(info->t_fstype)) {
			/* We're not in_group_p(). */
			if (is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		} else {
			/* Directories always inherit the setgid bit. */
			if (!is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		}

		/* create a special file via mknodat() vfs_create */
		if (mknodat(open_tree_fd, FILE2, S_IFREG | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, FILE2, 0))
			die("failure: is_setgid");

		/* create a whiteout device via mknodat() vfs_mknod */
		if (mknodat(open_tree_fd, CHRDEV1, S_IFCHR | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, CHRDEV1, 0))
			die("failure: is_setgid");

		/*
		 * In setgid directories newly created files always inherit the
		 * gid from the parent directory. Verify that the file is owned
		 * by gid 1000, not by gid 0.
		 */
		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 1000))
			die("failure: check ownership");

		/*
		 * In setgid directories newly created directories always
		 * inherit the gid from the parent directory. Verify that the
		 * directory is owned by gid 1000, not by gid 0.
		 */
		if (!expected_uid_gid(open_tree_fd, DIR1, 0, 0, 1000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, FILE2, 0, 0, 1000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, CHRDEV1, 0, 0, 1000))
			die("failure: check ownership");

		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, DIR1, AT_REMOVEDIR))
			die("failure: delete");

		if (unlinkat(open_tree_fd, FILE2, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, CHRDEV1, 0))
			die("failure: delete");

		/* create tmpfile via filesystem tmpfile api */
		if (supported) {
			tmpfile_fd = openat(open_tree_fd, ".", O_TMPFILE | O_RDWR, S_IXGRP | S_ISGID);
			if (tmpfile_fd < 0)
				die("failure: create");
			/* link the temporary file into the filesystem, making it permanent */
			snprintf(path, PATH_MAX,  "/proc/self/fd/%d", tmpfile_fd);
			if (linkat(AT_FDCWD, path, open_tree_fd, FILE3, AT_SYMLINK_FOLLOW))
				die("failure: linkat");
			if (close(tmpfile_fd))
				die("failure: close");
			if (is_setgid(open_tree_fd, FILE3, 0))
				die("failure: is_setgid");
			if (!expected_uid_gid(open_tree_fd, FILE3, 0, 0, 1000))
				die("failure: check ownership");
			if (unlinkat(open_tree_fd, FILE3, 0))
				die("failure: delete");
		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	if (fchownat(info->t_dir1_fd, "", -1, 0, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
		log_stderr("failure: fchownat");
		goto out;
	}

	if (fchownat(info->t_dir1_fd, "", -1, 0, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
		log_stderr("failure: fchownat");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 0, 1000, false))
			die("failure: switch_userns");

		if (!caps_down_fsetid())
			die("failure: caps_down_fsetid");

		/* create regular file via open() */
		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, S_IXGRP | S_ISGID);
		if (file1_fd < 0)
			die("failure: create");

		/* Neither in_group_p() nor capable_wrt_inode_uidgid() so setgid
		 * bit needs to be stripped.
		 */
		if (is_setgid(open_tree_fd, FILE1, 0))
			die("failure: is_setgid");

		/* create directory */
		if (mkdirat(open_tree_fd, DIR1, 0000))
			die("failure: create");

		/* Directories always inherit the setgid bit. */
		if (xfs_irix_sgid_inherit_enabled(info->t_fstype)) {
			/* We're not in_group_p(). */
			if (is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		} else {
			/* Directories always inherit the setgid bit. */
			if (!is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		}

		/* create a special file via mknodat() vfs_create */
		if (mknodat(open_tree_fd, FILE2, S_IFREG | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, FILE2, 0))
			die("failure: is_setgid");

		/* create a whiteout device via mknodat() vfs_mknod */
		if (mknodat(open_tree_fd, CHRDEV1, S_IFCHR | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, CHRDEV1, 0))
			die("failure: is_setgid");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 0))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, DIR1, 0, 0, 0))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, FILE2, 0, 0, 0))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, CHRDEV1, 0, 0, 0))
			die("failure: check ownership");

		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, DIR1, AT_REMOVEDIR))
			die("failure: delete");

		if (unlinkat(open_tree_fd, FILE2, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, CHRDEV1, 0))
			die("failure: delete");

		/* create tmpfile via filesystem tmpfile api */
		if (supported) {
			tmpfile_fd = openat(open_tree_fd, ".", O_TMPFILE | O_RDWR, S_IXGRP | S_ISGID);
			if (tmpfile_fd < 0)
				die("failure: create");
			/* link the temporary file into the filesystem, making it permanent */
			snprintf(path, PATH_MAX,  "/proc/self/fd/%d", tmpfile_fd);
			if (linkat(AT_FDCWD, path, open_tree_fd, FILE3, AT_SYMLINK_FOLLOW))
				die("failure: linkat");
			if (close(tmpfile_fd))
				die("failure: close");
			if (is_setgid(open_tree_fd, FILE3, 0))
				die("failure: is_setgid");
			if (!expected_uid_gid(open_tree_fd, FILE3, 0, 0, 0))
				die("failure: check ownership");
			if (unlinkat(open_tree_fd, FILE3, 0))
				die("failure: delete");
		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

/* Validate that setid transitions are handled correctly on idmapped mounts. */
int tcore_setid_binaries_idmapped_mounts(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, exec_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	if (mkdirat(info->t_mnt_fd, DIR1, 0777)) {
		log_stderr("failure: mkdirat");
		goto out;
	}

	/* create a file to be used as setuid binary */
	file1_fd = openat(info->t_dir1_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC | O_RDWR, 0644);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	/* open our own executable */
	exec_fd = openat(-EBADF, "/proc/self/exe", O_RDONLY | O_CLOEXEC, 0000);
	if (exec_fd < 0) {
		log_stderr("failure:openat ");
		goto out;
	}

	/* copy our own executable into the file we created */
	if (fd_to_fd(exec_fd, file1_fd)) {
		log_stderr("failure: fd_to_fd");
		goto out;
	}

	/* chown the file to the uid and gid we want to assume */
	if (fchown(file1_fd, 5000, 5000)) {
		log_stderr("failure: fchown");
		goto out;
	}

	/* set the setid bits and grant execute permissions to the group */
	if (fchmod(file1_fd, S_IXGRP | S_IEXEC | S_ISUID | S_ISGID), 0) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* Verify that the sid bits got raised. */
	if (!is_setid(info->t_dir1_fd, FILE1, 0)) {
		log_stderr("failure: is_setid");
		goto out;
	}

	safe_close(exec_fd);
	safe_close(file1_fd);

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	/* A detached mount will have an anonymous mount namespace attached to
	 * it. This means that we can't execute setid binaries on a detached
	 * mount because the mnt_may_suid() helper will fail the check_mount()
	 * part of its check which compares the caller's mount namespace to the
	 * detached mount's mount namespace. Since by definition an anonymous
	 * mount namespace is not equale to any mount namespace currently in
	 * use this can't work. So attach the mount to the filesystem first
	 * before performing this check.
	 */
	if (sys_move_mount(open_tree_fd, "", info->t_mnt_fd, DIR1, MOVE_MOUNT_F_EMPTY_PATH)) {
		log_stderr("failure: sys_move_mount");
		goto out;
	}

	/* Verify we run setid binary as uid and gid 10000 from idmapped mount mount. */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		static char *envp[] = {
			"IDMAP_MOUNT_TEST_RUN_SETID=1",
			"EXPECTED_EUID=15000",
			"EXPECTED_EGID=15000",
			NULL,
		};
		static char *argv[] = {
			NULL,
		};

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 15000, 15000))
			die("failure: expected_uid_gid");

		sys_execveat(open_tree_fd, FILE1, argv, envp, 0);
		die("failure: sys_execveat");

		exit(EXIT_FAILURE);
	}

	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(exec_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	snprintf(t_buf, sizeof(t_buf), "%s/" DIR1, info->t_mountpoint);
	sys_umount2(t_buf, MNT_DETACH);
	rm_r(info->t_mnt_fd, DIR1);

	return fret;
}

/* Validate that setid transitions are handled correctly on idmapped mounts
 * running in a user namespace where the uid and gid of the setid binary have no
 * mapping.
 */
int tcore_setid_binaries_idmapped_mounts_in_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, exec_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	if (mkdirat(info->t_mnt_fd, DIR1, 0777)) {
		log_stderr("failure: ");
		goto out;
	}

	/* create a file to be used as setuid binary */
	file1_fd = openat(info->t_dir1_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC | O_RDWR, 0644);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	/* open our own executable */
	exec_fd = openat(-EBADF, "/proc/self/exe", O_RDONLY | O_CLOEXEC, 0000);
	if (exec_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	/* copy our own executable into the file we created */
	if (fd_to_fd(exec_fd, file1_fd)) {
		log_stderr("failure: fd_to_fd");
		goto out;
	}

	safe_close(exec_fd);

	/* chown the file to the uid and gid we want to assume */
	if (fchown(file1_fd, 5000, 5000)) {
		log_stderr("failure: fchown");
		goto out;
	}

	/* set the setid bits and grant execute permissions to the group */
	if (fchmod(file1_fd, S_IXGRP | S_IEXEC | S_ISUID | S_ISGID), 0) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* Verify that the sid bits got raised. */
	if (!is_setid(info->t_dir1_fd, FILE1, 0)) {
		log_stderr("failure: is_setid");
		goto out;
	}

	safe_close(file1_fd);

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	/* A detached mount will have an anonymous mount namespace attached to
	 * it. This means that we can't execute setid binaries on a detached
	 * mount because the mnt_may_suid() helper will fail the check_mount()
	 * part of its check which compares the caller's mount namespace to the
	 * detached mount's mount namespace. Since by definition an anonymous
	 * mount namespace is not equale to any mount namespace currently in
	 * use this can't work. So attach the mount to the filesystem first
	 * before performing this check.
	 */
	if (sys_move_mount(open_tree_fd, "", info->t_mnt_fd, DIR1, MOVE_MOUNT_F_EMPTY_PATH)) {
		log_stderr("failure: sys_move_mount");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		static char *envp[] = {
			"IDMAP_MOUNT_TEST_RUN_SETID=1",
			"EXPECTED_EUID=5000",
			"EXPECTED_EGID=5000",
			NULL,
		};
		static char *argv[] = {
			NULL,
		};

		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 5000, 5000))
			die("failure: expected_uid_gid");

		sys_execveat(open_tree_fd, FILE1, argv, envp, 0);
		die("failure: sys_execveat");

		exit(EXIT_FAILURE);
	}

	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	file1_fd = openat(info->t_dir1_fd, FILE1, O_RDWR | O_CLOEXEC, 0644);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	/* chown the file to the uid and gid we want to assume */
	if (fchown(file1_fd, 0, 0)) {
		log_stderr("failure: fchown");
		goto out;
	}

	/* set the setid bits and grant execute permissions to the group */
	if (fchmod(file1_fd, S_IXOTH | S_IXGRP | S_IEXEC | S_ISUID | S_ISGID), 0) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* Verify that the sid bits got raised. */
	if (!is_setid(info->t_dir1_fd, FILE1, 0)) {
		log_stderr("failure: is_setid");
		goto out;
	}

	safe_close(file1_fd);

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		static char *envp[] = {
			"IDMAP_MOUNT_TEST_RUN_SETID=1",
			"EXPECTED_EUID=0",
			"EXPECTED_EGID=0",
			NULL,
		};
		static char *argv[] = {
			NULL,
		};

		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 5000, 5000, true))
			die("failure: switch_userns");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 0))
			die("failure: expected_uid_gid");

		sys_execveat(open_tree_fd, FILE1, argv, envp, 0);
		die("failure: sys_execveat");

		exit(EXIT_FAILURE);
	}

	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	file1_fd = openat(info->t_dir1_fd, FILE1, O_RDWR | O_CLOEXEC, 0644);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	/* chown the file to the uid and gid we want to assume */
	if (fchown(file1_fd, 30000, 30000)) {
		log_stderr("failure: fchown");
		goto out;
	}

	if (fchmod(file1_fd, S_IXOTH | S_IEXEC | S_ISUID | S_ISGID), 0) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* Verify that the sid bits got raised. */
	if (!is_setid(info->t_dir1_fd, FILE1, 0)) {
		log_stderr("failure: is_setid");
		goto out;
	}

	safe_close(file1_fd);

	/* Verify that we can't assume a uid and gid of a setid binary for which
	 * we have no mapping in our user namespace.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		char expected_euid[100];
		char expected_egid[100];
		static char *envp[4] = {
			NULL,
			NULL,
			NULL,
			NULL,
		};
		static char *argv[] = {
			NULL,
		};

		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		envp[0] = "IDMAP_MOUNT_TEST_RUN_SETID=0";
		snprintf(expected_euid, sizeof(expected_euid), "EXPECTED_EUID=%d", geteuid());
		envp[1] = expected_euid;
		snprintf(expected_egid, sizeof(expected_egid), "EXPECTED_EGID=%d", getegid());
		envp[2] = expected_egid;

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");

		sys_execveat(open_tree_fd, FILE1, argv, envp, 0);
		die("failure: sys_execveat");

		exit(EXIT_FAILURE);
	}

	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(exec_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	snprintf(t_buf, sizeof(t_buf), "%s/" DIR1, info->t_mountpoint);
	sys_umount2(t_buf, MNT_DETACH);
	rm_r(info->t_mnt_fd, DIR1);

	return fret;
}

/* Validate that setid transitions are handled correctly on idmapped mounts
 * running in a user namespace where the uid and gid of the setid binary have no
 * mapping.
 */
int tcore_setid_binaries_idmapped_mounts_in_userns_separate_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, exec_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	if (mkdirat(info->t_mnt_fd, DIR1, 0777)) {
		log_stderr("failure: mkdirat");
		goto out;
	}

	/* create a file to be used as setuid binary */
	file1_fd = openat(info->t_dir1_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC | O_RDWR, 0644);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	/* open our own executable */
	exec_fd = openat(-EBADF, "/proc/self/exe", O_RDONLY | O_CLOEXEC, 0000);
	if (exec_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	/* copy our own executable into the file we created */
	if (fd_to_fd(exec_fd, file1_fd)) {
		log_stderr("failure: fd_to_fd");
		goto out;
	}

	safe_close(exec_fd);

	/* change ownership of all files to uid 0 */
	if (chown_r(info->t_mnt_fd, T_DIR1, 20000, 20000)) {
		log_stderr("failure: chown_r");
		goto out;
	}

	/* chown the file to the uid and gid we want to assume */
	if (fchown(file1_fd, 25000, 25000)) {
		log_stderr("failure: fchown");
		goto out;
	}

	/* set the setid bits and grant execute permissions to the group */
	if (fchmod(file1_fd, S_IXGRP | S_IEXEC | S_ISUID | S_ISGID), 0) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* Verify that the sid bits got raised. */
	if (!is_setid(info->t_dir1_fd, FILE1, 0)) {
		log_stderr("failure: is_setid");
		goto out;
	}

	safe_close(file1_fd);

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(20000, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	/* A detached mount will have an anonymous mount namespace attached to
	 * it. This means that we can't execute setid binaries on a detached
	 * mount because the mnt_may_suid() helper will fail the check_mount()
	 * part of its check which compares the caller's mount namespace to the
	 * detached mount's mount namespace. Since by definition an anonymous
	 * mount namespace is not equale to any mount namespace currently in
	 * use this can't work. So attach the mount to the filesystem first
	 * before performing this check.
	 */
	if (sys_move_mount(open_tree_fd, "", info->t_mnt_fd, DIR1, MOVE_MOUNT_F_EMPTY_PATH)) {
		log_stderr("failure: sys_move_mount");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		int userns_fd;
		static char *envp[] = {
			"IDMAP_MOUNT_TEST_RUN_SETID=1",
			"EXPECTED_EUID=5000",
			"EXPECTED_EGID=5000",
			NULL,
		};
		static char *argv[] = {
			NULL,
		};

		userns_fd = get_userns_fd(0, 10000, 10000);
		if (userns_fd < 0)
			die("failure: get_userns_fd");

		if (!switch_userns(userns_fd, 0, 0, false))
			die("failure: switch_userns");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 5000, 5000))
			die("failure: expected_uid_gid");

		sys_execveat(open_tree_fd, FILE1, argv, envp, 0);
		die("failure: sys_execveat");

		exit(EXIT_FAILURE);
	}

	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	file1_fd = openat(info->t_dir1_fd, FILE1, O_RDWR | O_CLOEXEC, 0644);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	/* chown the file to the uid and gid we want to assume */
	if (fchown(file1_fd, 20000, 20000)) {
		log_stderr("failure: fchown");
		goto out;
	}

	/* set the setid bits and grant execute permissions to the group */
	if (fchmod(file1_fd, S_IXOTH | S_IXGRP | S_IEXEC | S_ISUID | S_ISGID), 0) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* Verify that the sid bits got raised. */
	if (!is_setid(info->t_dir1_fd, FILE1, 0)) {
		log_stderr("failure: is_setid");
		goto out;
	}

	safe_close(file1_fd);

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		int userns_fd;
		static char *envp[] = {
			"IDMAP_MOUNT_TEST_RUN_SETID=1",
			"EXPECTED_EUID=0",
			"EXPECTED_EGID=0",
			NULL,
		};
		static char *argv[] = {
			NULL,
		};

		userns_fd = get_userns_fd(0, 10000, 10000);
		if (userns_fd < 0)
			die("failure: get_userns_fd");

		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(userns_fd, 1000, 1000, true))
			die("failure: switch_userns");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 0))
			die("failure: expected_uid_gid");

		sys_execveat(open_tree_fd, FILE1, argv, envp, 0);
		die("failure: sys_execveat");

		exit(EXIT_FAILURE);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	file1_fd = openat(info->t_dir1_fd, FILE1, O_RDWR | O_CLOEXEC, 0644);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	/* chown the file to the uid and gid we want to assume */
	if (fchown(file1_fd, 0, 0)) {
		log_stderr("failure: fchown");
		goto out;
	}

	if (fchmod(file1_fd, S_IXOTH | S_IEXEC | S_ISUID | S_ISGID), 0) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* Verify that the sid bits got raised. */
	if (!is_setid(info->t_dir1_fd, FILE1, 0)) {
		log_stderr("failure: is_setid");
		goto out;
	}

	safe_close(file1_fd);

	/* Verify that we can't assume a uid and gid of a setid binary for
	 * which we have no mapping in our user namespace.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		int userns_fd;
		char expected_euid[100];
		char expected_egid[100];
		static char *envp[4] = {
			NULL,
			NULL,
			NULL,
			NULL,
		};
		static char *argv[] = {
			NULL,
		};

		userns_fd = get_userns_fd(0, 10000, 10000);
		if (userns_fd < 0)
			die("failure: get_userns_fd");

		if (!switch_userns(userns_fd, 0, 0, false))
			die("failure: switch_userns");

		envp[0] = "IDMAP_MOUNT_TEST_RUN_SETID=0";
		snprintf(expected_euid, sizeof(expected_euid), "EXPECTED_EUID=%d", geteuid());
		envp[1] = expected_euid;
		snprintf(expected_egid, sizeof(expected_egid), "EXPECTED_EGID=%d", getegid());
		envp[2] = expected_egid;

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, info->t_overflowuid, info->t_overflowgid))
			die("failure: expected_uid_gid");

		sys_execveat(open_tree_fd, FILE1, argv, envp, 0);
		die("failure: sys_execveat");

		exit(EXIT_FAILURE);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(exec_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	snprintf(t_buf, sizeof(t_buf), "%s/" DIR1, info->t_mountpoint);
	sys_umount2(t_buf, MNT_DETACH);
	rm_r(info->t_mnt_fd, DIR1);

	return fret;
}

int tcore_sticky_bit_unlink_idmapped_mounts(const struct vfstest_info *info)
{
	int fret = -1;
	int dir_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	if (!caps_supported())
		return 0;

	/* create directory */
	if (mkdirat(info->t_dir1_fd, DIR1, 0000)) {
		log_stderr("failure: mkdirat");
		goto out;
	}

	dir_fd = openat(info->t_dir1_fd, DIR1, O_DIRECTORY | O_CLOEXEC);
	if (dir_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}
	if (fchown(dir_fd, 10000, 10000)) {
		log_stderr("failure: fchown");
		goto out;
	}
	if (fchmod(dir_fd, 0777)) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE1, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE1, 10000, 10000, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE1, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE2, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE2, 12000, 12000, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE2, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(10000, 0, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(dir_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	/* The sticky bit is not set so we must be able to delete files not
	 * owned by us.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_ids(1000, 1000))
			die("failure: switch_ids");

		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: unlinkat");

		if (unlinkat(open_tree_fd, FILE2, 0))
			die("failure: unlinkat");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	/* set sticky bit */
	if (fchmod(dir_fd, 0777 | S_ISVTX)) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* validate sticky bit is set */
	if (!is_sticky(info->t_dir1_fd, DIR1, 0)) {
		log_stderr("failure: is_sticky");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE1, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE1, 10000, 10000, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE1, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE2, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE2, 12000, 12000, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE2, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* The sticky bit is set so we must not be able to delete files not
	 * owned by us.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_ids(1000, 1000))
			die("failure: switch_ids");

		if (!unlinkat(open_tree_fd, FILE1, 0))
			die("failure: unlinkat");
		if (errno != EPERM)
			die("failure: errno");

		if (!unlinkat(open_tree_fd, FILE2, 0))
			die("failure: unlinkat");
		if (errno != EPERM)
			die("failure: errno");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	/* The sticky bit is set and we own the files so we must be able to
	 * delete the files now.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		/* change ownership */
		if (fchownat(dir_fd, FILE1, 11000, -1, 0))
			die("failure: fchownat");
		if (!expected_uid_gid(dir_fd, FILE1, 0, 11000, 10000))
			die("failure: expected_uid_gid");
		if (fchownat(dir_fd, FILE2, 11000, -1, 0))
			die("failure: fchownat");
		if (!expected_uid_gid(dir_fd, FILE2, 0, 11000, 12000))
			die("failure: expected_uid_gid");

		if (!switch_ids(1000, 1000))
			die("failure: switch_ids");

		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: unlinkat");

		if (unlinkat(open_tree_fd, FILE2, 0))
			die("failure: unlinkat");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	/* change uid to unprivileged user */
	if (fchown(dir_fd, 11000, -1)) {
		log_stderr("failure: fchown");
		goto out;
	}
	if (fchmod(dir_fd, 0777 | S_ISVTX)) {
		log_stderr("failure: fchmod");
		goto out;
	}
	/* validate sticky bit is set */
	if (!is_sticky(info->t_dir1_fd, DIR1, 0)) {
		log_stderr("failure: is_sticky");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE1, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE1, 10000, 10000, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE1, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE2, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE2, 12000, 12000, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE2, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* The sticky bit is set and we own the directory so we must be able to
	 * delete the files now.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_ids(1000, 1000))
			die("failure: switch_ids");

		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: unlinkat");

		if (unlinkat(open_tree_fd, FILE2, 0))
			die("failure: unlinkat");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(dir_fd);
	safe_close(open_tree_fd);

	return fret;
}

/* Validate that the sticky bit behaves correctly on idmapped mounts for unlink
 * operations in a user namespace.
 */
int tcore_sticky_bit_unlink_idmapped_mounts_in_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int dir_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	if (!caps_supported())
		return 0;

	/* create directory */
	if (mkdirat(info->t_dir1_fd, DIR1, 0000)) {
		log_stderr("failure: mkdirat");
		goto out;
	}

	dir_fd = openat(info->t_dir1_fd, DIR1, O_DIRECTORY | O_CLOEXEC);
	if (dir_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}
	if (fchown(dir_fd, 0, 0)) {
		log_stderr("failure: fchown");
		goto out;
	}
	if (fchmod(dir_fd, 0777)) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE1, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE1, 0, 0, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE1, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE2, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE2, 2000, 2000, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE2, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(dir_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	/* The sticky bit is not set so we must be able to delete files not
	 * owned by us.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 1000, 1000, true))
			die("failure: switch_userns");

		if (unlinkat(dir_fd, FILE1, 0))
			die("failure: unlinkat");

		if (unlinkat(dir_fd, FILE2, 0))
			die("failure: unlinkat");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	/* set sticky bit */
	if (fchmod(dir_fd, 0777 | S_ISVTX)) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* validate sticky bit is set */
	if (!is_sticky(info->t_dir1_fd, DIR1, 0)) {
		log_stderr("failure: is_sticky");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE1, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE1, 0, 0, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE1, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE2, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE2, 2000, 2000, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE2, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* The sticky bit is set so we must not be able to delete files not
	 * owned by us.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 1000, 1000, true))
			die("failure: switch_userns");

		if (!unlinkat(dir_fd, FILE1, 0))
			die("failure: unlinkat");
		if (errno != EPERM)
			die("failure: errno");

		if (!unlinkat(dir_fd, FILE2, 0))
			die("failure: unlinkat");
		if (errno != EPERM)
			die("failure: errno");

		if (!unlinkat(open_tree_fd, FILE1, 0))
			die("failure: unlinkat");
		if (errno != EPERM)
			die("failure: errno");

		if (!unlinkat(open_tree_fd, FILE2, 0))
			die("failure: unlinkat");
		if (errno != EPERM)
			die("failure: errno");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	/* The sticky bit is set and we own the files so we must be able to
	 * delete the files now.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		/* change ownership */
		if (fchownat(dir_fd, FILE1, 1000, -1, 0))
			die("failure: fchownat");
		if (!expected_uid_gid(dir_fd, FILE1, 0, 1000, 0))
			die("failure: expected_uid_gid");
		if (fchownat(dir_fd, FILE2, 1000, -1, 0))
			die("failure: fchownat");
		if (!expected_uid_gid(dir_fd, FILE2, 0, 1000, 2000))
			die("failure: expected_uid_gid");

		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 1000, 1000, true))
			die("failure: switch_userns");

		if (!unlinkat(dir_fd, FILE1, 0))
			die("failure: unlinkat");
		if (errno != EPERM)
			die("failure: errno");

		if (!unlinkat(dir_fd, FILE2, 0))
			die("failure: unlinkat");
		if (errno != EPERM)
			die("failure: errno");

		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: unlinkat");

		if (unlinkat(open_tree_fd, FILE2, 0))
			die("failure: unlinkat");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	/* change uid to unprivileged user */
	if (fchown(dir_fd, 1000, -1)) {
		log_stderr("failure: fchown");
		goto out;
	}
	if (fchmod(dir_fd, 0777 | S_ISVTX)) {
		log_stderr("failure: fchmod");
		goto out;
	}
	/* validate sticky bit is set */
	if (!is_sticky(info->t_dir1_fd, DIR1, 0)) {
		log_stderr("failure: is_sticky");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE1, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE1, 0, 0, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE1, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE2, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE2, 2000, 2000, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE2, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* The sticky bit is set and we own the directory so we must be able to
	 * delete the files now.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 1000, 1000, true))
			die("failure: switch_userns");

		/* we don't own the directory from the original mount */
		if (!unlinkat(dir_fd, FILE1, 0))
			die("failure: unlinkat");
		if (errno != EPERM)
			die("failure: errno");

		if (!unlinkat(dir_fd, FILE2, 0))
			die("failure: unlinkat");
		if (errno != EPERM)
			die("failure: errno");

		/* we own the file from the idmapped mount */
		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: unlinkat");
		if (unlinkat(open_tree_fd, FILE2, 0))
			die("failure: unlinkat");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(dir_fd);
	safe_close(open_tree_fd);

	return fret;
}

int tcore_sticky_bit_rename_idmapped_mounts(const struct vfstest_info *info)
{
	int fret = -1;
	int dir_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	if (!caps_supported())
		return 0;

	/* create directory */
	if (mkdirat(info->t_dir1_fd, DIR1, 0000)) {
		log_stderr("failure: mkdirat");
		goto out;
	}

	dir_fd = openat(info->t_dir1_fd, DIR1, O_DIRECTORY | O_CLOEXEC);
	if (dir_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	if (fchown(dir_fd, 10000, 10000)) {
		log_stderr("failure: fchown");
		goto out;
	}

	if (fchmod(dir_fd, 0777)) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE1, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE1, 10000, 10000, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE1, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE2, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE2, 12000, 12000, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE2, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(10000, 0, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(dir_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	/* The sticky bit is not set so we must be able to delete files not
	 * owned by us.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_ids(1000, 1000))
			die("failure: switch_ids");

		if (renameat(open_tree_fd, FILE1, open_tree_fd, FILE1_RENAME))
			die("failure: renameat");

		if (renameat(open_tree_fd, FILE2, open_tree_fd, FILE2_RENAME))
			die("failure: renameat");

		if (renameat(open_tree_fd, FILE1_RENAME, open_tree_fd, FILE1))
			die("failure: renameat");

		if (renameat(open_tree_fd, FILE2_RENAME, open_tree_fd, FILE2))
			die("failure: renameat");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	/* set sticky bit */
	if (fchmod(dir_fd, 0777 | S_ISVTX)) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* validate sticky bit is set */
	if (!is_sticky(info->t_dir1_fd, DIR1, 0)) {
		log_stderr("failure: is_sticky");
		goto out;
	}

	/* The sticky bit is set so we must not be able to delete files not
	 * owned by us.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_ids(1000, 1000))
			die("failure: switch_ids");

		if (!renameat(open_tree_fd, FILE1, open_tree_fd, FILE1_RENAME))
			die("failure: renameat");
		if (errno != EPERM)
			die("failure: errno");

		if (!renameat(open_tree_fd, FILE2, open_tree_fd, FILE2_RENAME))
			die("failure: renameat");
		if (errno != EPERM)
			die("failure: errno");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	/* The sticky bit is set and we own the files so we must be able to
	 * delete the files now.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		/* change ownership */
		if (fchownat(dir_fd, FILE1, 11000, -1, 0))
			die("failure: fchownat");
		if (!expected_uid_gid(dir_fd, FILE1, 0, 11000, 10000))
			die("failure: expected_uid_gid");
		if (fchownat(dir_fd, FILE2, 11000, -1, 0))
			die("failure: fchownat");
		if (!expected_uid_gid(dir_fd, FILE2, 0, 11000, 12000))
			die("failure: expected_uid_gid");

		if (!switch_ids(1000, 1000))
			die("failure: switch_ids");

		if (renameat(open_tree_fd, FILE1, open_tree_fd, FILE1_RENAME))
			die("failure: renameat");

		if (renameat(open_tree_fd, FILE2, open_tree_fd, FILE2_RENAME))
			die("failure: renameat");

		if (renameat(open_tree_fd, FILE1_RENAME, open_tree_fd, FILE1))
			die("failure: renameat");

		if (renameat(open_tree_fd, FILE2_RENAME, open_tree_fd, FILE2))
			die("failure: renameat");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	/* change uid to unprivileged user */
	if (fchown(dir_fd, 11000, -1)) {
		log_stderr("failure: fchown");
		goto out;
	}
	if (fchmod(dir_fd, 0777 | S_ISVTX)) {
		log_stderr("failure: fchmod");
		goto out;
	}
	/* validate sticky bit is set */
	if (!is_sticky(info->t_dir1_fd, DIR1, 0)) {
		log_stderr("failure: is_sticky");
		goto out;
	}

	/* The sticky bit is set and we own the directory so we must be able to
	 * delete the files now.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_ids(1000, 1000))
			die("failure: switch_ids");

		if (renameat(open_tree_fd, FILE1, open_tree_fd, FILE1_RENAME))
			die("failure: renameat");

		if (renameat(open_tree_fd, FILE2, open_tree_fd, FILE2_RENAME))
			die("failure: renameat");

		if (renameat(open_tree_fd, FILE1_RENAME, open_tree_fd, FILE1))
			die("failure: renameat");

		if (renameat(open_tree_fd, FILE2_RENAME, open_tree_fd, FILE2))
			die("failure: renameat");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(dir_fd);
	safe_close(open_tree_fd);

	return fret;
}

/* Validate that the sticky bit behaves correctly on idmapped mounts for unlink
 * operations in a user namespace.
 */
int tcore_sticky_bit_rename_idmapped_mounts_in_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int dir_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	if (!caps_supported())
		return 0;

	/* create directory */
	if (mkdirat(info->t_dir1_fd, DIR1, 0000)) {
		log_stderr("failure: mkdirat");
		goto out;
	}

	dir_fd = openat(info->t_dir1_fd, DIR1, O_DIRECTORY | O_CLOEXEC);
	if (dir_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}
	if (fchown(dir_fd, 0, 0)) {
		log_stderr("failure: fchown");
		goto out;
	}
	if (fchmod(dir_fd, 0777)) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE1, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE1, 0, 0, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE1, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* create regular file via mknod */
	if (mknodat(dir_fd, FILE2, S_IFREG | 0000, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}
	if (fchownat(dir_fd, FILE2, 2000, 2000, 0)) {
		log_stderr("failure: fchownat");
		goto out;
	}
	if (fchmodat(dir_fd, FILE2, 0644, 0)) {
		log_stderr("failure: fchmodat");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(dir_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	/* The sticky bit is not set so we must be able to delete files not
	 * owned by us.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 1000, 1000, true))
			die("failure: switch_userns");

		if (renameat(dir_fd, FILE1, dir_fd, FILE1_RENAME))
			die("failure: renameat");

		if (renameat(dir_fd, FILE2, dir_fd, FILE2_RENAME))
			die("failure: renameat");

		if (renameat(dir_fd, FILE1_RENAME, dir_fd, FILE1))
			die("failure: renameat");

		if (renameat(dir_fd, FILE2_RENAME, dir_fd, FILE2))
			die("failure: renameat");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	/* set sticky bit */
	if (fchmod(dir_fd, 0777 | S_ISVTX)) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* validate sticky bit is set */
	if (!is_sticky(info->t_dir1_fd, DIR1, 0)) {
		log_stderr("failure: is_sticky");
		goto out;
	}

	/* The sticky bit is set so we must not be able to delete files not
	 * owned by us.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 1000, 1000, true))
			die("failure: switch_userns");

		if (!renameat(dir_fd, FILE1, dir_fd, FILE1_RENAME))
			die("failure: renameat");
		if (errno != EPERM)
			die("failure: errno");

		if (!renameat(dir_fd, FILE2, dir_fd, FILE2_RENAME))
			die("failure: renameat");
		if (errno != EPERM)
			die("failure: errno");

		if (!renameat(open_tree_fd, FILE1, open_tree_fd, FILE1_RENAME))
			die("failure: renameat");
		if (errno != EPERM)
			die("failure: errno");

		if (!renameat(open_tree_fd, FILE2, open_tree_fd, FILE2_RENAME))
			die("failure: renameat");
		if (errno != EPERM)
			die("failure: errno");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	/* The sticky bit is set and we own the files so we must be able to
	 * delete the files now.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		/* change ownership */
		if (fchownat(dir_fd, FILE1, 1000, -1, 0))
			die("failure: fchownat");
		if (!expected_uid_gid(dir_fd, FILE1, 0, 1000, 0))
			die("failure: expected_uid_gid");
		if (fchownat(dir_fd, FILE2, 1000, -1, 0))
			die("failure: fchownat");
		if (!expected_uid_gid(dir_fd, FILE2, 0, 1000, 2000))
			die("failure: expected_uid_gid");

		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 1000, 1000, true))
			die("failure: switch_userns");

		if (!renameat(dir_fd, FILE1, dir_fd, FILE1_RENAME))
			die("failure: renameat");
		if (errno != EPERM)
			die("failure: errno");

		if (!renameat(dir_fd, FILE2, dir_fd, FILE2_RENAME))
			die("failure: renameat");
		if (errno != EPERM)
			die("failure: errno");

		if (renameat(open_tree_fd, FILE1, open_tree_fd, FILE1_RENAME))
			die("failure: renameat");

		if (renameat(open_tree_fd, FILE2, open_tree_fd, FILE2_RENAME))
			die("failure: renameat");

		if (renameat(open_tree_fd, FILE1_RENAME, open_tree_fd, FILE1))
			die("failure: renameat");

		if (renameat(open_tree_fd, FILE2_RENAME, open_tree_fd, FILE2))
			die("failure: renameat");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	/* change uid to unprivileged user */
	if (fchown(dir_fd, 1000, -1)) {
		log_stderr("failure: fchown");
		goto out;
	}
	if (fchmod(dir_fd, 0777 | S_ISVTX)) {
		log_stderr("failure: fchmod");
		goto out;
	}
	/* validate sticky bit is set */
	if (!is_sticky(info->t_dir1_fd, DIR1, 0)) {
		log_stderr("failure: is_sticky");
		goto out;
	}

	/* The sticky bit is set and we own the directory so we must be able to
	 * delete the files now.
	 */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 1000, 1000, true))
			die("failure: switch_userns");

		/* we don't own the directory from the original mount */
		if (!renameat(dir_fd, FILE1, dir_fd, FILE1_RENAME))
			die("failure: renameat");
		if (errno != EPERM)
			die("failure: errno");

		if (!renameat(dir_fd, FILE2, dir_fd, FILE2_RENAME))
			die("failure: renameat");
		if (errno != EPERM)
			die("failure: errno");

		/* we own the file from the idmapped mount */
		if (renameat(open_tree_fd, FILE1, open_tree_fd, FILE1_RENAME))
			die("failure: renameat");

		if (renameat(open_tree_fd, FILE2, open_tree_fd, FILE2_RENAME))
			die("failure: renameat");

		if (renameat(open_tree_fd, FILE1_RENAME, open_tree_fd, FILE1))
			die("failure: renameat");

		if (renameat(open_tree_fd, FILE2_RENAME, open_tree_fd, FILE2))
			die("failure: renameat");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid)) {
		log_stderr("failure: wait_for_pid");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(open_tree_fd);
	safe_close(attr.userns_fd);
	safe_close(dir_fd);

	return fret;
}

int tcore_symlink_idmapped_mounts(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	if (!caps_supported())
		return 0;

	file1_fd = openat(info->t_dir1_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (file1_fd < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	if (chown_r(info->t_mnt_fd, T_DIR1, 0, 0)) {
		log_stderr("failure: chown_r");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_fsids(10000, 10000))
			die("failure: switch fsids");

		if (!caps_up())
			die("failure: raise caps");

		if (symlinkat(FILE1, open_tree_fd, FILE2))
			die("failure: create");

		if (fchownat(open_tree_fd, FILE2, 15000, 15000, AT_SYMLINK_NOFOLLOW))
			die("failure: change ownership");

		if (!expected_uid_gid(open_tree_fd, FILE2, AT_SYMLINK_NOFOLLOW, 15000, 15000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 10000, 10000))
			die("failure: check ownership");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

int tcore_symlink_idmapped_mounts_in_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;

	if (chown_r(info->t_mnt_fd, T_DIR1, 0, 0)) {
		log_stderr("failure: chown_r");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, 0644);
		if (file1_fd < 0)
			die("failure: create");
		safe_close(file1_fd);

		if (symlinkat(FILE1, open_tree_fd, FILE2))
			die("failure: create");

		if (fchownat(open_tree_fd, FILE2, 5000, 5000, AT_SYMLINK_NOFOLLOW))
			die("failure: change ownership");

		if (!expected_uid_gid(open_tree_fd, FILE2, AT_SYMLINK_NOFOLLOW, 5000, 5000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 0))
			die("failure: check ownership");

		exit(EXIT_SUCCESS);
	}

	if (wait_for_pid(pid))
		goto out;

	if (!expected_uid_gid(info->t_dir1_fd, FILE2, AT_SYMLINK_NOFOLLOW, 5000, 5000)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	if (!expected_uid_gid(info->t_dir1_fd, FILE1, 0, 0, 0)) {
		log_stderr("failure: expected_uid_gid");
		goto out;
	}

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

static int nested_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int ret;
	pid_t pid;
	unsigned int id;
	struct list *it, *next;
	struct userns_hierarchy hierarchy[] = {
		{ .level = 1, .fd_userns = -EBADF, },
		{ .level = 2, .fd_userns = -EBADF, },
		{ .level = 3, .fd_userns = -EBADF, },
		{ .level = 4, .fd_userns = -EBADF, },
		/* Dummy entry that marks the end. */
		{ .level = MAX_USERNS_LEVEL, .fd_userns = -EBADF, },
	};
	struct mount_attr attr_level1 = {
		.attr_set	= MOUNT_ATTR_IDMAP,
		.userns_fd	= -EBADF,
	};
	struct mount_attr attr_level2 = {
		.attr_set	= MOUNT_ATTR_IDMAP,
		.userns_fd	= -EBADF,
	};
	struct mount_attr attr_level3 = {
		.attr_set	= MOUNT_ATTR_IDMAP,
		.userns_fd	= -EBADF,
	};
	struct mount_attr attr_level4 = {
		.attr_set	= MOUNT_ATTR_IDMAP,
		.userns_fd	= -EBADF,
	};
	int fd_dir1 = -EBADF,
	    fd_open_tree_level1 = -EBADF,
	    fd_open_tree_level2 = -EBADF,
	    fd_open_tree_level3 = -EBADF,
	    fd_open_tree_level4 = -EBADF;
	const unsigned int id_file_range = 10000;

	list_init(&hierarchy[0].id_map);
	list_init(&hierarchy[1].id_map);
	list_init(&hierarchy[2].id_map);
	list_init(&hierarchy[3].id_map);

	/*
	 * Give a large map to the outermost user namespace so we can create
	 * comfortable nested maps.
	 */
	ret = add_map_entry(&hierarchy[0].id_map, 1000000, 0, 1000000000, ID_TYPE_UID);
	if (ret) {
		log_stderr("failure: adding uidmap for userns at level 1");
		goto out;
	}

	ret = add_map_entry(&hierarchy[0].id_map, 1000000, 0, 1000000000, ID_TYPE_GID);
	if (ret) {
		log_stderr("failure: adding gidmap for userns at level 1");
		goto out;
	}

	/* This is uid:0->2000000:100000000 in init userns. */
	ret = add_map_entry(&hierarchy[1].id_map, 1000000, 0, 100000000, ID_TYPE_UID);
	if (ret) {
		log_stderr("failure: adding uidmap for userns at level 2");
		goto out;
	}

	/* This is gid:0->2000000:100000000 in init userns. */
	ret = add_map_entry(&hierarchy[1].id_map, 1000000, 0, 100000000, ID_TYPE_GID);
	if (ret) {
		log_stderr("failure: adding gidmap for userns at level 2");
		goto out;
	}

	/* This is uid:0->3000000:999 in init userns. */
	ret = add_map_entry(&hierarchy[2].id_map, 1000000, 0, 999, ID_TYPE_UID);
	if (ret) {
		log_stderr("failure: adding uidmap for userns at level 3");
		goto out;
	}

	/* This is gid:0->3000000:999 in the init userns. */
	ret = add_map_entry(&hierarchy[2].id_map, 1000000, 0, 999, ID_TYPE_GID);
	if (ret) {
		log_stderr("failure: adding gidmap for userns at level 3");
		goto out;
	}

	/* id 999 will remain unmapped. */

	/* This is uid:1000->2001000:1 in init userns. */
	ret = add_map_entry(&hierarchy[2].id_map, 1000, 1000, 1, ID_TYPE_UID);
	if (ret) {
		log_stderr("failure: adding uidmap for userns at level 3");
		goto out;
	}

	/* This is gid:1000->2001000:1 in init userns. */
	ret = add_map_entry(&hierarchy[2].id_map, 1000, 1000, 1, ID_TYPE_GID);
	if (ret) {
		log_stderr("failure: adding gidmap for userns at level 3");
		goto out;
	}

	/* This is uid:1001->3001001:10000 in init userns. */
	ret = add_map_entry(&hierarchy[2].id_map, 1001001, 1001, 10000000, ID_TYPE_UID);
	if (ret) {
		log_stderr("failure: adding uidmap for userns at level 3");
		goto out;
	}

	/* This is gid:1001->3001001:10000 in init userns. */
	ret = add_map_entry(&hierarchy[2].id_map, 1001001, 1001, 10000000, ID_TYPE_GID);
	if (ret) {
		log_stderr("failure: adding gidmap for userns at level 3");
		goto out;
	}

	/* Don't write a mapping in the 4th userns. */
	list_empty(&hierarchy[4].id_map);

	/* Create the actual userns hierarchy. */
	ret = create_userns_hierarchy(hierarchy);
	if (ret) {
		log_stderr("failure: create userns hierarchy");
		goto out;
	}

	attr_level1.userns_fd = hierarchy[0].fd_userns;
	attr_level2.userns_fd = hierarchy[1].fd_userns;
	attr_level3.userns_fd = hierarchy[2].fd_userns;
	attr_level4.userns_fd = hierarchy[3].fd_userns;

	/*
	 * Create one directory where we create files for each uid/gid within
	 * the first userns.
	 */
	if (mkdirat(info->t_dir1_fd, DIR1, 0777)) {
		log_stderr("failure: mkdirat");
		goto out;
	}

	fd_dir1 = openat(info->t_dir1_fd, DIR1, O_DIRECTORY | O_CLOEXEC);
	if (fd_dir1 < 0) {
		log_stderr("failure: openat");
		goto out;
	}

	for (id = 0; id <= id_file_range; id++) {
		char file[256];

		snprintf(file, sizeof(file), DIR1 "/" FILE1 "_%u", id);

		if (mknodat(info->t_dir1_fd, file, S_IFREG | 0644, 0)) {
			log_stderr("failure: create %s", file);
			goto out;
		}

		if (fchownat(info->t_dir1_fd, file, id, id, AT_SYMLINK_NOFOLLOW)) {
			log_stderr("failure: fchownat %s", file);
			goto out;
		}

		if (!expected_uid_gid(info->t_dir1_fd, file, 0, id, id)) {
			log_stderr("failure: check ownership %s", file);
			goto out;
		}
	}

	/* Create detached mounts for all the user namespaces. */
	fd_open_tree_level1 = sys_open_tree(info->t_dir1_fd, DIR1,
					    AT_NO_AUTOMOUNT |
					    AT_SYMLINK_NOFOLLOW |
					    OPEN_TREE_CLOEXEC |
					    OPEN_TREE_CLONE);
	if (fd_open_tree_level1 < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	fd_open_tree_level2 = sys_open_tree(info->t_dir1_fd, DIR1,
					    AT_NO_AUTOMOUNT |
					    AT_SYMLINK_NOFOLLOW |
					    OPEN_TREE_CLOEXEC |
					    OPEN_TREE_CLONE);
	if (fd_open_tree_level2 < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	fd_open_tree_level3 = sys_open_tree(info->t_dir1_fd, DIR1,
					    AT_NO_AUTOMOUNT |
					    AT_SYMLINK_NOFOLLOW |
					    OPEN_TREE_CLOEXEC |
					    OPEN_TREE_CLONE);
	if (fd_open_tree_level3 < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	fd_open_tree_level4 = sys_open_tree(info->t_dir1_fd, DIR1,
					    AT_NO_AUTOMOUNT |
					    AT_SYMLINK_NOFOLLOW |
					    OPEN_TREE_CLOEXEC |
					    OPEN_TREE_CLONE);
	if (fd_open_tree_level4 < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	/* Turn detached mounts into detached idmapped mounts. */
	if (sys_mount_setattr(fd_open_tree_level1, "", AT_EMPTY_PATH,
			      &attr_level1, sizeof(attr_level1))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	if (sys_mount_setattr(fd_open_tree_level2, "", AT_EMPTY_PATH,
			      &attr_level2, sizeof(attr_level2))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	if (sys_mount_setattr(fd_open_tree_level3, "", AT_EMPTY_PATH,
			      &attr_level3, sizeof(attr_level3))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	if (sys_mount_setattr(fd_open_tree_level4, "", AT_EMPTY_PATH,
			      &attr_level4, sizeof(attr_level4))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	/* Verify that ownership looks correct for callers in the init userns. */
	for (id = 0; id <= id_file_range; id++) {
		bool bret;
		unsigned int id_level1, id_level2, id_level3;
		char file[256];

		snprintf(file, sizeof(file), FILE1 "_%u", id);

		id_level1 = id + 1000000;
		if (!expected_uid_gid(fd_open_tree_level1, file, 0, id_level1, id_level1)) {
			log_stderr("failure: check ownership %s", file);
			goto out;
		}

		id_level2 = id + 2000000;
		if (!expected_uid_gid(fd_open_tree_level2, file, 0, id_level2, id_level2)) {
			log_stderr("failure: check ownership %s", file);
			goto out;
		}

		if (id == 999) {
			/* This id is unmapped. */
			bret = expected_uid_gid(fd_open_tree_level3, file, 0, info->t_overflowuid, info->t_overflowgid);
		} else if (id == 1000) {
			id_level3 = id + 2000000; /* We punched a hole in the map at 1000. */
			bret = expected_uid_gid(fd_open_tree_level3, file, 0, id_level3, id_level3);
		} else {
			id_level3 = id + 3000000; /* Rest is business as usual. */
			bret = expected_uid_gid(fd_open_tree_level3, file, 0, id_level3, id_level3);
		}
		if (!bret) {
			log_stderr("failure: check ownership %s", file);
			goto out;
		}

		if (!expected_uid_gid(fd_open_tree_level4, file, 0, info->t_overflowuid, info->t_overflowgid)) {
			log_stderr("failure: check ownership %s", file);
			goto out;
		}
	}

	/* Verify that ownership looks correct for callers in the first userns. */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr_level1.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		for (id = 0; id <= id_file_range; id++) {
			bool bret;
			unsigned int id_level1, id_level2, id_level3;
			char file[256];

			snprintf(file, sizeof(file), FILE1 "_%u", id);

			id_level1 = id;
			if (!expected_uid_gid(fd_open_tree_level1, file, 0, id_level1, id_level1))
				die("failure: check ownership %s", file);

			id_level2 = id + 1000000;
			if (!expected_uid_gid(fd_open_tree_level2, file, 0, id_level2, id_level2))
				die("failure: check ownership %s", file);

			if (id == 999) {
				/* This id is unmapped. */
				bret = expected_uid_gid(fd_open_tree_level3, file, 0, info->t_overflowuid, info->t_overflowgid);
			} else if (id == 1000) {
				id_level3 = id + 1000000; /* We punched a hole in the map at 1000. */
				bret = expected_uid_gid(fd_open_tree_level3, file, 0, id_level3, id_level3);
			} else {
				id_level3 = id + 2000000; /* Rest is business as usual. */
				bret = expected_uid_gid(fd_open_tree_level3, file, 0, id_level3, id_level3);
			}
			if (!bret)
				die("failure: check ownership %s", file);

			if (!expected_uid_gid(fd_open_tree_level4, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);
		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	/* Verify that ownership looks correct for callers in the second userns. */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr_level2.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		for (id = 0; id <= id_file_range; id++) {
			bool bret;
			unsigned int id_level2, id_level3;
			char file[256];

			snprintf(file, sizeof(file), FILE1 "_%u", id);

			if (!expected_uid_gid(fd_open_tree_level1, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);

			id_level2 = id;
			if (!expected_uid_gid(fd_open_tree_level2, file, 0, id_level2, id_level2))
				die("failure: check ownership %s", file);

			if (id == 999) {
				/* This id is unmapped. */
				bret = expected_uid_gid(fd_open_tree_level3, file, 0, info->t_overflowuid, info->t_overflowgid);
			} else if (id == 1000) {
				id_level3 = id; /* We punched a hole in the map at 1000. */
				bret = expected_uid_gid(fd_open_tree_level3, file, 0, id_level3, id_level3);
			} else {
				id_level3 = id + 1000000; /* Rest is business as usual. */
				bret = expected_uid_gid(fd_open_tree_level3, file, 0, id_level3, id_level3);
			}
			if (!bret)
				die("failure: check ownership %s", file);

			if (!expected_uid_gid(fd_open_tree_level4, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);
		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	/* Verify that ownership looks correct for callers in the third userns. */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr_level3.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		for (id = 0; id <= id_file_range; id++) {
			bool bret;
			unsigned int id_level2, id_level3;
			char file[256];

			snprintf(file, sizeof(file), FILE1 "_%u", id);

			if (!expected_uid_gid(fd_open_tree_level1, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);

			if (id == 1000) {
				/*
				 * The idmapping of the third userns has a hole
				 * at uid/gid 1000. That means:
				 * - 1000->userns_0(2000000) // init userns
				 * - 1000->userns_1(2000000) // level 1
				 * - 1000->userns_2(1000000) // level 2
				 * - 1000->userns_3(1000)    // level 3 (because level 3 has a hole)
				 */
				id_level2 = id;
				bret = expected_uid_gid(fd_open_tree_level2, file, 0, id_level2, id_level2);
			} else {
				bret = expected_uid_gid(fd_open_tree_level2, file, 0, info->t_overflowuid, info->t_overflowgid);
			}
			if (!bret)
				die("failure: check ownership %s", file);


			if (id == 999) {
				/* This id is unmapped. */
				bret = expected_uid_gid(fd_open_tree_level3, file, 0, info->t_overflowuid, info->t_overflowgid);
			} else {
				id_level3 = id; /* Rest is business as usual. */
				bret = expected_uid_gid(fd_open_tree_level3, file, 0, id_level3, id_level3);
			}
			if (!bret)
				die("failure: check ownership %s", file);

			if (!expected_uid_gid(fd_open_tree_level4, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);
		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	/* Verify that ownership looks correct for callers in the fourth userns. */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (setns(attr_level4.userns_fd, CLONE_NEWUSER))
			die("failure: switch_userns");

		for (id = 0; id <= id_file_range; id++) {
			char file[256];

			snprintf(file, sizeof(file), FILE1 "_%u", id);

			if (!expected_uid_gid(fd_open_tree_level1, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);

			if (!expected_uid_gid(fd_open_tree_level2, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);

			if (!expected_uid_gid(fd_open_tree_level3, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);

			if (!expected_uid_gid(fd_open_tree_level4, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);
		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	/* Verify that chown works correctly for callers in the first userns. */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr_level1.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		for (id = 0; id <= id_file_range; id++) {
			bool bret;
			unsigned int id_level1, id_level2, id_level3, id_new;
			char file[256];

			snprintf(file, sizeof(file), FILE1 "_%u", id);

			id_new = id + 1;
			if (fchownat(fd_open_tree_level1, file, id_new, id_new, AT_SYMLINK_NOFOLLOW))
				die("failure: fchownat %s", file);

			id_level1 = id_new;
			if (!expected_uid_gid(fd_open_tree_level1, file, 0, id_level1, id_level1))
				die("failure: check ownership %s", file);

			id_level2 = id_new + 1000000;
			if (!expected_uid_gid(fd_open_tree_level2, file, 0, id_level2, id_level2))
				die("failure: check ownership %s", file);

			if (id_new == 999) {
				/* This id is unmapped. */
				bret = expected_uid_gid(fd_open_tree_level3, file, 0, info->t_overflowuid, info->t_overflowgid);
			} else if (id_new == 1000) {
				id_level3 = id_new + 1000000; /* We punched a hole in the map at 1000. */
				bret = expected_uid_gid(fd_open_tree_level3, file, 0, id_level3, id_level3);
			} else {
				id_level3 = id_new + 2000000; /* Rest is business as usual. */
				bret = expected_uid_gid(fd_open_tree_level3, file, 0, id_level3, id_level3);
			}
			if (!bret)
				die("failure: check ownership %s", file);

			if (!expected_uid_gid(fd_open_tree_level4, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);

			/* Revert ownership. */
			if (fchownat(fd_open_tree_level1, file, id, id, AT_SYMLINK_NOFOLLOW))
				die("failure: fchownat %s", file);
		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	/* Verify that chown works correctly for callers in the second userns. */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr_level2.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		for (id = 0; id <= id_file_range; id++) {
			bool bret;
			unsigned int id_level2, id_level3, id_new;
			char file[256];

			snprintf(file, sizeof(file), FILE1 "_%u", id);

			id_new = id + 1;
			if (fchownat(fd_open_tree_level2, file, id_new, id_new, AT_SYMLINK_NOFOLLOW))
				die("failure: fchownat %s", file);

			if (!expected_uid_gid(fd_open_tree_level1, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);

			id_level2 = id_new;
			if (!expected_uid_gid(fd_open_tree_level2, file, 0, id_level2, id_level2))
				die("failure: check ownership %s", file);

			if (id_new == 999) {
				/* This id is unmapped. */
				bret = expected_uid_gid(fd_open_tree_level3, file, 0, info->t_overflowuid, info->t_overflowgid);
			} else if (id_new == 1000) {
				id_level3 = id_new; /* We punched a hole in the map at 1000. */
				bret = expected_uid_gid(fd_open_tree_level3, file, 0, id_level3, id_level3);
			} else {
				id_level3 = id_new + 1000000; /* Rest is business as usual. */
				bret = expected_uid_gid(fd_open_tree_level3, file, 0, id_level3, id_level3);
			}
			if (!bret)
				die("failure: check ownership %s", file);

			if (!expected_uid_gid(fd_open_tree_level4, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);

			/* Revert ownership. */
			if (fchownat(fd_open_tree_level2, file, id, id, AT_SYMLINK_NOFOLLOW))
				die("failure: fchownat %s", file);
		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	/* Verify that chown works correctly for callers in the third userns. */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(attr_level3.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		for (id = 0; id <= id_file_range; id++) {
			unsigned int id_new;
			char file[256];

			snprintf(file, sizeof(file), FILE1 "_%u", id);

			id_new = id + 1;
			if (id_new == 999 || id_new == 1000) {
				/*
				 * We can't change ownership as we can't
				 * chown from or to an unmapped id.
				 */
				if (!fchownat(fd_open_tree_level3, file, id_new, id_new, AT_SYMLINK_NOFOLLOW))
					die("failure: fchownat %s", file);
			} else {
				if (fchownat(fd_open_tree_level3, file, id_new, id_new, AT_SYMLINK_NOFOLLOW))
					die("failure: fchownat %s", file);
			}

			if (!expected_uid_gid(fd_open_tree_level1, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);

			/* There's no id 1000 anymore as we changed ownership for id 1000 to 1001 above. */
			if (!expected_uid_gid(fd_open_tree_level2, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);

			if (id_new == 999) {
				/*
				 * We did not change ownership as we can't
				 * chown to an unmapped id.
				 */
				if (!expected_uid_gid(fd_open_tree_level3, file, 0, id, id))
					die("failure: check ownership %s", file);
			} else if (id_new == 1000) {
				/*
				 * We did not change ownership as we can't
				 * chown from an unmapped id.
				 */
				if (!expected_uid_gid(fd_open_tree_level3, file, 0, info->t_overflowuid, info->t_overflowgid))
					die("failure: check ownership %s", file);
			} else {
				if (!expected_uid_gid(fd_open_tree_level3, file, 0, id_new, id_new))
					die("failure: check ownership %s", file);
			}

			if (!expected_uid_gid(fd_open_tree_level4, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);

			/* Revert ownership. */
			if (id_new != 999 && id_new != 1000) {
				if (fchownat(fd_open_tree_level3, file, id, id, AT_SYMLINK_NOFOLLOW))
					die("failure: fchownat %s", file);
			}
		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	/* Verify that chown works correctly for callers in the fourth userns. */
	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (setns(attr_level4.userns_fd, CLONE_NEWUSER))
			die("failure: switch_userns");

		for (id = 0; id <= id_file_range; id++) {
			char file[256];
			unsigned long id_new;

			snprintf(file, sizeof(file), FILE1 "_%u", id);

			id_new = id + 1;
			if (!fchownat(fd_open_tree_level4, file, id_new, id_new, AT_SYMLINK_NOFOLLOW))
				die("failure: fchownat %s", file);

			if (!expected_uid_gid(fd_open_tree_level1, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);

			if (!expected_uid_gid(fd_open_tree_level2, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);

			if (!expected_uid_gid(fd_open_tree_level3, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);

			if (!expected_uid_gid(fd_open_tree_level4, file, 0, info->t_overflowuid, info->t_overflowgid))
				die("failure: check ownership %s", file);

		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");

out:
	list_for_each_safe(it, &hierarchy[0].id_map, next) {
		list_del(it);
		free(it->elem);
		free(it);
	}

	list_for_each_safe(it, &hierarchy[1].id_map, next) {
		list_del(it);
		free(it->elem);
		free(it);
	}

	list_for_each_safe(it, &hierarchy[2].id_map, next) {
		list_del(it);
		free(it->elem);
		free(it);
	}

	safe_close(hierarchy[0].fd_userns);
	safe_close(hierarchy[1].fd_userns);
	safe_close(hierarchy[2].fd_userns);
	safe_close(fd_dir1);
	safe_close(fd_open_tree_level1);
	safe_close(fd_open_tree_level2);
	safe_close(fd_open_tree_level3);
	safe_close(fd_open_tree_level4);
	return fret;
}

#define USER1 "fsgqa"
#define USER2 "fsgqa2"

/**
 * lookup_ids - lookup uid and gid for a username
 * @name: [in]  name of the user
 * @uid:  [out] pointer to the user-ID
 * @gid:  [out] pointer to the group-ID
 *
 * Lookup the uid and gid of a user.
 *
 * Return: On success, true is returned.
 *         On error, false is returned.
 */
static bool lookup_ids(const char *name, uid_t *uid, gid_t *gid)
{
	bool bret = false;
	struct passwd *pwentp = NULL;
	struct passwd pwent;
	char *buf;
	ssize_t bufsize;
	int ret;

	bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (bufsize < 0)
		bufsize = 1024;

	buf = malloc(bufsize);
	if (!buf)
		return bret;

	ret = getpwnam_r(name, &pwent, buf, bufsize, &pwentp);
	if (!ret && pwentp) {
		*uid = pwent.pw_uid;
		*gid = pwent.pw_gid;
		bret = true;
	}

	free(buf);
	return bret;
}

/**
 * setattr_fix_968219708108 - test for commit 968219708108 ("fs: handle circular mappings correctly")
 *
 * Test that ->setattr() works correctly for idmapped mounts with circular
 * idmappings such as:
 *
 * b:1000:1001:1
 * b:1001:1000:1
 *
 * Assume a directory /source with two files:
 *
 * /source/file1 | 1000:1000
 * /source/file2 | 1001:1001
 *
 * and we create an idmapped mount of /source at /target with an idmapped of:
 *
 * mnt_userns:        1000:1001:1
 *                    1001:1000:1
 *
 * In the idmapped mount file1 will be owned by uid 1001 and file2 by uid 1000:
 *
 * /target/file1 | 1001:1001
 * /target/file2 | 1000:1000
 *
 * Because in essence the idmapped mount switches ownership for {g,u}id 1000
 * and {g,u}id 1001.
 *
 * 1. A user with fs{g,u}id 1000 must be allowed to setattr /target/file2 from
 *    {g,u}id 1000 in the idmapped mount to {g,u}id 1000.
 * 2. A user with fs{g,u}id 1001 must be allowed to setattr /target/file1 from
 *    {g,u}id 1001 in the idmapped mount to {g,u}id 1001.
 * 3. A user with fs{g,u}id 1000 must fail to setattr /target/file1 from
 *    {g,u}id 1001 in the idmapped mount to {g,u}id 1000.
 *    This must fail with EPERM. The caller's fs{g,u}id doesn't match the
 *    {g,u}id of the file.
 * 4. A user with fs{g,u}id 1001 must fail to setattr /target/file2 from
 *    {g,u}id 1000 in the idmapped mount to {g,u}id 1000.
 *    This must fail with EPERM. The caller's fs{g,u}id doesn't match the
 *    {g,u}id of the file.
 * 5. Both, a user with fs{g,u}id 1000 and a user with fs{g,u}id 1001, must
 *    fail to setattr /target/file1 owned by {g,u}id 1001 in the idmapped mount
 *    and /target/file2 owned by {g,u}id 1000 in the idmapped mount to any
 *    {g,u}id apart from {g,u}id 1000 or 1001 with EINVAL.
 *    Only {g,u}id 1000 and 1001 have a mapping in the idmapped mount. Other
 *    {g,u}id are unmapped.
 */
static int setattr_fix_968219708108(const struct vfstest_info *info)
{
	int fret = -1;
	int open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set	= MOUNT_ATTR_IDMAP,
		.userns_fd	= -EBADF,
	};
	int ret;
	uid_t user1_uid, user2_uid;
	gid_t user1_gid, user2_gid;
	pid_t pid;
	struct list idmap;
	struct list *it_cur, *it_next;

	if (!caps_supported())
		return 0;

	list_init(&idmap);

	if (!lookup_ids(USER1, &user1_uid, &user1_gid)) {
		log_stderr("failure: lookup_user");
		goto out;
	}

	if (!lookup_ids(USER2, &user2_uid, &user2_gid)) {
		log_stderr("failure: lookup_user");
		goto out;
	}

	log_debug("Found " USER1 " with uid(%d) and gid(%d) and " USER2 " with uid(%d) and gid(%d)",
		  user1_uid, user1_gid, user2_uid, user2_gid);

	if (mkdirat(info->t_dir1_fd, DIR1, 0777)) {
		log_stderr("failure: mkdirat");
		goto out;
	}

	if (mknodat(info->t_dir1_fd, DIR1 "/" FILE1, S_IFREG | 0644, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}

	if (chown_r(info->t_mnt_fd, T_DIR1, user1_uid, user1_gid)) {
		log_stderr("failure: chown_r");
		goto out;
	}

	if (mknodat(info->t_dir1_fd, DIR1 "/" FILE2, S_IFREG | 0644, 0)) {
		log_stderr("failure: mknodat");
		goto out;
	}

	if (fchownat(info->t_dir1_fd, DIR1 "/" FILE2, user2_uid, user2_gid, AT_SYMLINK_NOFOLLOW)) {
		log_stderr("failure: fchownat");
		goto out;
	}

	print_r(info->t_mnt_fd, T_DIR1);

	/* u:1000:1001:1 */
	ret = add_map_entry(&idmap, user1_uid, user2_uid, 1, ID_TYPE_UID);
	if (ret) {
		log_stderr("failure: add_map_entry");
		goto out;
	}

	/* u:1001:1000:1 */
	ret = add_map_entry(&idmap, user2_uid, user1_uid, 1, ID_TYPE_UID);
	if (ret) {
		log_stderr("failure: add_map_entry");
		goto out;
	}

	/* g:1000:1001:1 */
	ret = add_map_entry(&idmap, user1_gid, user2_gid, 1, ID_TYPE_GID);
	if (ret) {
		log_stderr("failure: add_map_entry");
		goto out;
	}

	/* g:1001:1000:1 */
	ret = add_map_entry(&idmap, user2_gid, user1_gid, 1, ID_TYPE_GID);
	if (ret) {
		log_stderr("failure: add_map_entry");
		goto out;
	}

	attr.userns_fd = get_userns_fd_from_idmap(&idmap);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, DIR1,
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE |
				     AT_RECURSIVE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	print_r(open_tree_fd, "");

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		/* switch to {g,u}id 1001 */
		if (!switch_resids(user2_uid, user2_gid))
			die("failure: switch_resids");

		/* drop all capabilities */
		if (!caps_down())
			die("failure: caps_down");

		/*
		 * The {g,u}id 0 is not mapped in this idmapped mount so this
		 * needs to fail with EINVAL.
		 * errno should be EOVERFLOW after kernel commit b27c82e12965.
		 */
		if (!fchownat(open_tree_fd, FILE1, 0, 0, AT_SYMLINK_NOFOLLOW))
			die("failure: change ownership");
		if (errno != EINVAL && errno != EOVERFLOW)
			die("failure: errno");

		/*
		 * A user with fs{g,u}id 1001 must be allowed to change
		 * ownership of /target/file1 owned by {g,u}id 1001 in this
		 * idmapped mount to {g,u}id 1001.
		 */
		if (fchownat(open_tree_fd, FILE1, user2_uid, user2_gid,
			     AT_SYMLINK_NOFOLLOW))
			die("failure: change ownership");

		/* Verify that the ownership is still {g,u}id 1001. */
		if (!expected_uid_gid(open_tree_fd, FILE1, AT_SYMLINK_NOFOLLOW,
				      user2_uid, user2_gid))
			die("failure: check ownership");

		/*
		 * A user with fs{g,u}id 1001 must not be allowed to change
		 * ownership of /target/file1 owned by {g,u}id 1001 in this
		 * idmapped mount to {g,u}id 1000.
		 */
		if (!fchownat(open_tree_fd, FILE1, user1_uid, user1_gid,
			      AT_SYMLINK_NOFOLLOW))
			die("failure: change ownership");
		if (errno != EPERM)
			die("failure: errno");

		/* Verify that the ownership is still {g,u}id 1001. */
		if (!expected_uid_gid(open_tree_fd, FILE1, AT_SYMLINK_NOFOLLOW,
				      user2_uid, user2_gid))
			die("failure: check ownership");

		/*
		 * A user with fs{g,u}id 1001 must not be allowed to change
		 * ownership of /target/file2 owned by {g,u}id 1000 in this
		 * idmapped mount to {g,u}id 1000.
		 */
		if (!fchownat(open_tree_fd, FILE2, user1_uid, user1_gid,
			      AT_SYMLINK_NOFOLLOW))
			die("failure: change ownership");
		if (errno != EPERM)
			die("failure: errno");

		/* Verify that the ownership is still {g,u}id 1000. */
		if (!expected_uid_gid(open_tree_fd, FILE2, AT_SYMLINK_NOFOLLOW,
				      user1_uid, user1_gid))
			die("failure: check ownership");

		/*
		 * A user with fs{g,u}id 1001 must not be allowed to change
		 * ownership of /target/file2 owned by {g,u}id 1000 in this
		 * idmapped mount to {g,u}id 1001.
		 */
		if (!fchownat(open_tree_fd, FILE2, user2_uid, user2_gid,
			      AT_SYMLINK_NOFOLLOW))
			die("failure: change ownership");
		if (errno != EPERM)
			die("failure: errno");

		/* Verify that the ownership is still {g,u}id 1000. */
		if (!expected_uid_gid(open_tree_fd, FILE2, AT_SYMLINK_NOFOLLOW,
				      user1_uid, user1_gid))
			die("failure: check ownership");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		/* switch to {g,u}id 1000 */
		if (!switch_resids(user1_uid, user1_gid))
			die("failure: switch_resids");

		/* drop all capabilities */
		if (!caps_down())
			die("failure: caps_down");

		/*
		 * The {g,u}id 0 is not mapped in this idmapped mount so this
		 * needs to fail with EINVAL.
		 * errno should be EOVERFLOW after kernel commit b27c82e12965.
		 */
		if (!fchownat(open_tree_fd, FILE1, 0, 0, AT_SYMLINK_NOFOLLOW))
			die("failure: change ownership");
		if (errno != EINVAL && errno != EOVERFLOW)
			die("failure: errno");

		/*
		 * A user with fs{g,u}id 1000 must be allowed to change
		 * ownership of /target/file2 owned by {g,u}id 1000 in this
		 * idmapped mount to {g,u}id 1000.
		 */
		if (fchownat(open_tree_fd, FILE2, user1_uid, user1_gid,
			     AT_SYMLINK_NOFOLLOW))
			die("failure: change ownership");

		/* Verify that the ownership is still {g,u}id 1000. */
		if (!expected_uid_gid(open_tree_fd, FILE2, AT_SYMLINK_NOFOLLOW,
				      user1_uid, user1_gid))
			die("failure: check ownership");

		/*
		 * A user with fs{g,u}id 1000 must not be allowed to change
		 * ownership of /target/file2 owned by {g,u}id 1000 in this
		 * idmapped mount to {g,u}id 1001.
		 */
		if (!fchownat(open_tree_fd, FILE2, user2_uid, user2_gid,
			      AT_SYMLINK_NOFOLLOW))
			die("failure: change ownership");
		if (errno != EPERM)
			die("failure: errno");

		/* Verify that the ownership is still {g,u}id 1000. */
		if (!expected_uid_gid(open_tree_fd, FILE2, AT_SYMLINK_NOFOLLOW,
				      user1_uid, user1_gid))
			die("failure: check ownership");

		/*
		 * A user with fs{g,u}id 1000 must not be allowed to change
		 * ownership of /target/file1 owned by {g,u}id 1001 in this
		 * idmapped mount to {g,u}id 1000.
		 */
		if (!fchownat(open_tree_fd, FILE1, user1_uid, user1_gid,
			     AT_SYMLINK_NOFOLLOW))
			die("failure: change ownership");
		if (errno != EPERM)
			die("failure: errno");

		/* Verify that the ownership is still {g,u}id 1001. */
		if (!expected_uid_gid(open_tree_fd, FILE1, AT_SYMLINK_NOFOLLOW,
				      user2_uid, user2_gid))
			die("failure: check ownership");

		/*
		 * A user with fs{g,u}id 1000 must not be allowed to change
		 * ownership of /target/file1 owned by {g,u}id 1001 in this
		 * idmapped mount to {g,u}id 1001.
		 */
		if (!fchownat(open_tree_fd, FILE1, user2_uid, user2_gid,
			      AT_SYMLINK_NOFOLLOW))
			die("failure: change ownership");
		if (errno != EPERM)
			die("failure: errno");

		/* Verify that the ownership is still {g,u}id 1001. */
		if (!expected_uid_gid(open_tree_fd, FILE1, AT_SYMLINK_NOFOLLOW,
				      user2_uid, user2_gid))
			die("failure: check ownership");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(open_tree_fd);

	list_for_each_safe(it_cur, &idmap, it_next) {
		list_del(it_cur);
		free(it_cur->elem);
		free(it_cur);
	}

	return fret;
}

/**
 * setxattr_fix_705191b03d50 - test for commit 705191b03d50 ("fs: fix acl translation").
 */
static int setxattr_fix_705191b03d50(const struct vfstest_info *info)
{
	int fret = -1;
	int fd_userns = -EBADF;
	int ret;
	uid_t user1_uid;
	gid_t user1_gid;
	pid_t pid;
	struct list idmap;
	struct list *it_cur, *it_next;

	list_init(&idmap);

	if (!lookup_ids(USER1, &user1_uid, &user1_gid)) {
		log_stderr("failure: lookup_user");
		goto out;
	}

	log_debug("Found " USER1 " with uid(%d) and gid(%d)", user1_uid, user1_gid);

	if (mkdirat(info->t_dir1_fd, DIR1, 0777)) {
		log_stderr("failure: mkdirat");
		goto out;
	}

	if (chown_r(info->t_mnt_fd, T_DIR1, user1_uid, user1_gid)) {
		log_stderr("failure: chown_r");
		goto out;
	}

	print_r(info->t_mnt_fd, T_DIR1);

	/* u:0:user1_uid:1 */
	ret = add_map_entry(&idmap, user1_uid, 0, 1, ID_TYPE_UID);
	if (ret) {
		log_stderr("failure: add_map_entry");
		goto out;
	}

	/* g:0:user1_gid:1 */
	ret = add_map_entry(&idmap, user1_gid, 0, 1, ID_TYPE_GID);
	if (ret) {
		log_stderr("failure: add_map_entry");
		goto out;
	}

	/* u:100:10000:100 */
	ret = add_map_entry(&idmap, 10000, 100, 100, ID_TYPE_UID);
	if (ret) {
		log_stderr("failure: add_map_entry");
		goto out;
	}

	/* g:100:10000:100 */
	ret = add_map_entry(&idmap, 10000, 100, 100, ID_TYPE_GID);
	if (ret) {
		log_stderr("failure: add_map_entry");
		goto out;
	}

	fd_userns = get_userns_fd_from_idmap(&idmap);
	if (fd_userns < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!switch_userns(fd_userns, 0, 0, false))
			die("failure: switch_userns");

		/* create separate mount namespace */
		if (unshare(CLONE_NEWNS))
			die("failure: create new mount namespace");

		/* turn off mount propagation */
		if (sys_mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, 0))
			die("failure: turn mount propagation off");

		snprintf(t_buf, sizeof(t_buf), "%s/%s/%s", info->t_mountpoint, T_DIR1, DIR1);

		if (sys_mount("none", t_buf, "tmpfs", 0, "mode=0755"))
			die("failure: mount");

		snprintf(t_buf, sizeof(t_buf), "%s/%s/%s/%s", info->t_mountpoint, T_DIR1, DIR1, DIR3);
		if (mkdir(t_buf, 0700))
			die("failure: mkdir");

		snprintf(t_buf, sizeof(t_buf), "setfacl -m u:100:rwx %s/%s/%s/%s", info->t_mountpoint, T_DIR1, DIR1, DIR3);
		if (system(t_buf))
			die("failure: system");

		snprintf(t_buf, sizeof(t_buf), "getfacl -n -p %s/%s/%s/%s | grep -q user:100:rwx", info->t_mountpoint, T_DIR1, DIR1, DIR3);
		if (system(t_buf))
			die("failure: system");

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(fd_userns);

	list_for_each_safe(it_cur, &idmap, it_next) {
		list_del(it_cur);
		free(it_cur->elem);
		free(it_cur);
	}

	return fret;
}

/* The current_umask() is stripped from the mode directly in the vfs if the
 * filesystem either doesn't support acls or the filesystem has been
 * mounted without posic acl support.
 *
 * If the filesystem does support acls then current_umask() stripping is
 * deferred to posix_acl_create(). So when the filesystem calls
 * posix_acl_create() and there are no acls set or not supported then
 * current_umask() will be stripped.
 *
 * Use umask(S_IXGRP) to check whether inode strip S_ISGID works correctly
 * in idmapped situation.
 *
 * Test for commit ac6800e279a2 ("fs: Add missing umask strip in vfs_tmpfile")
 * and 1639a49ccdce ("fs: move S_ISGID stripping into the vfs_*() helpers").
 */
static int setgid_create_umask_idmapped(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;
	int tmpfile_fd = -EBADF;
	bool supported = false;
	char path[PATH_MAX];
	mode_t mode;

	if (!caps_supported())
		return 0;

	if (fchmod(info->t_dir1_fd, S_IRUSR |
			      S_IWUSR |
			      S_IRGRP |
			      S_IWGRP |
			      S_IROTH |
			      S_IWOTH |
			      S_IXUSR |
			      S_IXGRP |
			      S_IXOTH |
			      S_ISGID), 0) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* Verify that the sid bits got raised. */
	if (!is_setgid(info->t_dir1_fd, "", AT_EMPTY_PATH)) {
		log_stderr("failure: is_setgid");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	supported = openat_tmpfile_supported(open_tree_fd);

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		/* Only umask with S_IXGRP because inode strip S_ISGID will check mode
		 * whether has group execute or search permission.
		 */
		umask(S_IXGRP);
		mode = umask(S_IXGRP);
		if (!(mode & S_IXGRP))
			die("failure: umask");

		if (!switch_ids(10000, 11000))
			die("failure: switch fsids");

		/* create regular file via open() */
		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, S_IXGRP | S_ISGID);
		if (file1_fd < 0)
			die("failure: create");

		/* Neither in_group_p() nor capable_wrt_inode_uidgid() so setgid
		 * bit needs to be stripped.
		 */
		if (is_setgid(open_tree_fd, FILE1, 0))
			die("failure: is_setgid");

		if (is_ixgrp(open_tree_fd, FILE1, 0))
			die("failure: is_ixgrp");

		/* create directory */
		if (mkdirat(open_tree_fd, DIR1, 0000))
			die("failure: create");

		if (xfs_irix_sgid_inherit_enabled(info->t_fstype)) {
			/* We're not in_group_p(). */
			if (is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		} else {
			/* Directories always inherit the setgid bit. */
			if (!is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		}

		if (is_ixgrp(open_tree_fd, DIR1, 0))
			die("failure: is_ixgrp");

		/* create a special file via mknodat() vfs_create */
		if (mknodat(open_tree_fd, FILE2, S_IFREG | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, FILE2, 0))
			die("failure: is_setgid");

		if (is_ixgrp(open_tree_fd, FILE2, 0))
			die("failure: is_ixgrp");

		/* create a whiteout device via mknodat() vfs_mknod */
		if (mknodat(open_tree_fd, CHRDEV1, S_IFCHR | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, CHRDEV1, 0))
			die("failure: is_setgid");

		if (is_ixgrp(open_tree_fd, CHRDEV1, 0))
			die("failure: is_ixgrp");

		/*
		 * In setgid directories newly created files always inherit the
		 * gid from the parent directory. Verify that the file is owned
		 * by gid 10000, not by gid 11000.
		 */
		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 10000, 10000))
			die("failure: check ownership");

		/*
		 * In setgid directories newly created directories always
		 * inherit the gid from the parent directory. Verify that the
		 * directory is owned by gid 10000, not by gid 11000.
		 */
		if (!expected_uid_gid(open_tree_fd, DIR1, 0, 10000, 10000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, FILE2, 0, 10000, 10000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, CHRDEV1, 0, 10000, 10000))
			die("failure: check ownership");

		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, DIR1, AT_REMOVEDIR))
			die("failure: delete");

		if (unlinkat(open_tree_fd, FILE2, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, CHRDEV1, 0))
			die("failure: delete");

		/* create tmpfile via filesystem tmpfile api */
		if (supported) {
			tmpfile_fd = openat(open_tree_fd, ".", O_TMPFILE | O_RDWR, S_IXGRP | S_ISGID);
			if (tmpfile_fd < 0)
				die("failure: create");
			/* link the temporary file into the filesystem, making it permanent */
			snprintf(path, PATH_MAX,  "/proc/self/fd/%d", tmpfile_fd);
			if (linkat(AT_FDCWD, path, open_tree_fd, FILE3, AT_SYMLINK_FOLLOW))
				die("failure: linkat");
			if (close(tmpfile_fd))
				die("failure: close");
			if (is_setgid(open_tree_fd, FILE3, 0))
				die("failure: is_setgid");
			if (is_ixgrp(open_tree_fd, FILE3, 0))
				die("failure: is_ixgrp");
			if (!expected_uid_gid(open_tree_fd, FILE3, 0, 10000, 10000))
				die("failure: check ownership");
			if (unlinkat(open_tree_fd, FILE3, 0))
				die("failure: delete");
		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

/* The current_umask() is stripped from the mode directly in the vfs if the
 * filesystem either doesn't support acls or the filesystem has been
 * mounted without posic acl support.
 *
 * If the filesystem does support acls then current_umask() stripping is
 * deferred to posix_acl_create(). So when the filesystem calls
 * posix_acl_create() and there are no acls set or not supported then
 * current_umask() will be stripped.
 *
 * Use umask(S_IXGRP) to check whether inode strip S_ISGID works correctly
 * in idmapped_in_userns situation.
 *
 * Test for commit ac6800e279a2 ("fs: Add missing umask strip in vfs_tmpfile")
 * and 1639a49ccdce ("fs: move S_ISGID stripping into the vfs_*() helpers").
 */
static int setgid_create_umask_idmapped_in_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;
	int tmpfile_fd = -EBADF;
	bool supported = false;
	char path[PATH_MAX];
	mode_t mode;

	if (!caps_supported())
		return 0;

	if (fchmod(info->t_dir1_fd, S_IRUSR |
			      S_IWUSR |
			      S_IRGRP |
			      S_IWGRP |
			      S_IROTH |
			      S_IWOTH |
			      S_IXUSR |
			      S_IXGRP |
			      S_IXOTH |
			      S_ISGID), 0) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* Verify that the sid bits got raised. */
	if (!is_setgid(info->t_dir1_fd, "", AT_EMPTY_PATH)) {
		log_stderr("failure: is_setgid");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	supported = openat_tmpfile_supported(open_tree_fd);

	/*
	 * Below we verify that setgid inheritance for a newly created file or
	 * directory works correctly. As part of this we need to verify that
	 * newly created files or directories inherit their gid from their
	 * parent directory. So we change the parent directorie's gid to 1000
	 * and create a file with fs{g,u}id 0 and verify that the newly created
	 * file and directory inherit gid 1000, not 0.
	 */
	if (fchownat(info->t_dir1_fd, "", -1, 1000, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
		log_stderr("failure: fchownat");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		/* Only umask with S_IXGRP because inode strip S_ISGID will check mode
		 * whether has group execute or search permission.
		 */
		umask(S_IXGRP);
		mode = umask(S_IXGRP);
		if (!(mode & S_IXGRP))
			die("failure: umask");

		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		if (!caps_down_fsetid())
			die("failure: caps_down_fsetid");

		/* create regular file via open() */
		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, S_IXGRP | S_ISGID);
		if (file1_fd < 0)
			die("failure: create");

		/* Neither in_group_p() nor capable_wrt_inode_uidgid() so setgid
		 * bit needs to be stripped.
		 */
		if (is_setgid(open_tree_fd, FILE1, 0))
			die("failure: is_setgid");

		if (is_ixgrp(open_tree_fd, FILE1, 0))
			die("failure: is_ixgrp");

		/* create directory */
		if (mkdirat(open_tree_fd, DIR1, 0000))
			die("failure: create");

		if (xfs_irix_sgid_inherit_enabled(info->t_fstype)) {
			/* We're not in_group_p(). */
			if (is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		} else {
			/* Directories always inherit the setgid bit. */
			if (!is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		}

		if (is_ixgrp(open_tree_fd, DIR1, 0))
			die("failure: is_ixgrp");

		/* create a special file via mknodat() vfs_create */
		if (mknodat(open_tree_fd, FILE2, S_IFREG | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, FILE2, 0))
			die("failure: is_setgid");

		if (is_ixgrp(open_tree_fd, FILE2, 0))
			die("failure: is_ixgrp");

		/* create a whiteout device via mknodat() vfs_mknod */
		if (mknodat(open_tree_fd, CHRDEV1, S_IFCHR | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, CHRDEV1, 0))
			die("failure: is_setgid");

		if (is_ixgrp(open_tree_fd, CHRDEV1, 0))
			die("failure: is_ixgrp");

		/*
		 * In setgid directories newly created files always inherit the
		 * gid from the parent directory. Verify that the file is owned
		 * by gid 1000, not by gid 0.
		 */
		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 1000))
			die("failure: check ownership");

		/*
		 * In setgid directories newly created directories always
		 * inherit the gid from the parent directory. Verify that the
		 * directory is owned by gid 1000, not by gid 0.
		 */
		if (!expected_uid_gid(open_tree_fd, DIR1, 0, 0, 1000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, FILE2, 0, 0, 1000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, CHRDEV1, 0, 0, 1000))
			die("failure: check ownership");

		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, DIR1, AT_REMOVEDIR))
			die("failure: delete");

		if (unlinkat(open_tree_fd, FILE2, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, CHRDEV1, 0))
			die("failure: delete");

		/* create tmpfile via filesystem tmpfile api */
		if (supported) {
			tmpfile_fd = openat(open_tree_fd, ".", O_TMPFILE | O_RDWR, S_IXGRP | S_ISGID);
			if (tmpfile_fd < 0)
				die("failure: create");
			/* link the temporary file into the filesystem, making it permanent */
			snprintf(path, PATH_MAX,  "/proc/self/fd/%d", tmpfile_fd);
			if (linkat(AT_FDCWD, path, open_tree_fd, FILE3, AT_SYMLINK_FOLLOW))
				die("failure: linkat");
			if (close(tmpfile_fd))
				die("failure: close");
			if (is_setgid(open_tree_fd, FILE3, 0))
				die("failure: is_setgid");
			if (is_ixgrp(open_tree_fd, FILE3, 0))
				die("failure: is_ixgrp");
			if (!expected_uid_gid(open_tree_fd, FILE3, 0, 0, 1000))
				die("failure: check ownership");
			if (unlinkat(open_tree_fd, FILE3, 0))
				die("failure: delete");
		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

/*
 * If the parent directory has a default acl then permissions are based off
 * of that and current_umask() is ignored. Specifically, if the ACL has an
 * ACL_MASK entry, the group permissions correspond to the permissions of
 * the ACL_MASK entry. Otherwise, if the ACL has no ACL_MASK entry, the
 * group permissions correspond to the permissions of the ACL_GROUP_OBJ
 * entry.
 *
 * Use setfacl to check whether inode strip S_ISGID works correctly under
 * the above two situations when enabling idmapped.
 *
 * Test for commit
 * 1639a49ccdce ("fs: move S_ISGID stripping into the vfs_*() helpers").
 */
static int setgid_create_acl_idmapped(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;
	int tmpfile_fd = -EBADF;
	bool supported = false;
	char path[PATH_MAX];
	mode_t mode;

	if (!caps_supported())
		return 0;

	if (fchmod(info->t_dir1_fd, S_IRUSR |
			      S_IWUSR |
			      S_IRGRP |
			      S_IWGRP |
			      S_IROTH |
			      S_IWOTH |
			      S_IXUSR |
			      S_IXGRP |
			      S_IXOTH |
			      S_ISGID), 0) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* Verify that the sid bits got raised. */
	if (!is_setgid(info->t_dir1_fd, "", AT_EMPTY_PATH)) {
		log_stderr("failure: is_setgid");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	supported = openat_tmpfile_supported(open_tree_fd);

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		umask(S_IXGRP);
		mode = umask(S_IXGRP);
		if (!(mode & S_IXGRP))
			die("failure: umask");

		/* The group permissions correspond to the permissions of the
		 * ACL_MASK entry. Use setfacl to set ACL mask(m) as rw, so now
		 * the group permissions is rw. Also, umask doesn't affect
		 * group permissions because umask will be ignored if having
		 * acl.
		 */
		snprintf(t_buf, sizeof(t_buf), "setfacl -d -m u::rwx,g::rw,o::rwx,m:rw %s/%s", info->t_mountpoint, T_DIR1);
		if (system(t_buf))
			die("failure: system");

		if (!switch_ids(10000, 11000))
			die("failure: switch fsids");

		/* create regular file via open() */
		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, S_IXGRP | S_ISGID);
		if (file1_fd < 0)
			die("failure: create");

		/* Neither in_group_p() nor capable_wrt_inode_uidgid() so setgid
		 * bit needs to be stripped.
		 */
		if (is_setgid(open_tree_fd, FILE1, 0))
			die("failure: is_setgid");

		if (is_ixgrp(open_tree_fd, FILE1, 0))
			die("failure: is_ixgrp");

		/* create directory */
		if (mkdirat(open_tree_fd, DIR1, 0000))
			die("failure: create");

		if (xfs_irix_sgid_inherit_enabled(info->t_fstype)) {
			/* We're not in_group_p(). */
			if (is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		} else {
			/* Directories always inherit the setgid bit. */
			if (!is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		}

		if (is_ixgrp(open_tree_fd, DIR1, 0))
			die("failure: is_ixgrp");

		/* create a special file via mknodat() vfs_create */
		if (mknodat(open_tree_fd, FILE2, S_IFREG | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, FILE2, 0))
			die("failure: is_setgid");

		if (is_ixgrp(open_tree_fd, FILE2, 0))
			die("failure: is_ixgrp");

		/* create a whiteout device via mknodat() vfs_mknod */
		if (mknodat(open_tree_fd, CHRDEV1, S_IFCHR | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, CHRDEV1, 0))
			die("failure: is_setgid");

		if (is_ixgrp(open_tree_fd, CHRDEV1, 0))
			die("failure: is_ixgrp");

		/*
		 * In setgid directories newly created files always inherit the
		 * gid from the parent directory. Verify that the file is owned
		 * by gid 10000, not by gid 11000.
		 */
		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 10000, 10000))
			die("failure: check ownership");

		/*
		 * In setgid directories newly created directories always
		 * inherit the gid from the parent directory. Verify that the
		 * directory is owned by gid 10000, not by gid 11000.
		 */
		if (!expected_uid_gid(open_tree_fd, DIR1, 0, 10000, 10000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, FILE2, 0, 10000, 10000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, CHRDEV1, 0, 10000, 10000))
			die("failure: check ownership");

		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, DIR1, AT_REMOVEDIR))
			die("failure: delete");

		if (unlinkat(open_tree_fd, FILE2, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, CHRDEV1, 0))
			die("failure: delete");

		/* create tmpfile via filesystem tmpfile api */
		if (supported) {
			tmpfile_fd = openat(open_tree_fd, ".", O_TMPFILE | O_RDWR, S_IXGRP | S_ISGID);
			if (tmpfile_fd < 0)
				die("failure: create");
			/* link the temporary file into the filesystem, making it permanent */
			snprintf(path, PATH_MAX,  "/proc/self/fd/%d", tmpfile_fd);
			if (linkat(AT_FDCWD, path, open_tree_fd, FILE3, AT_SYMLINK_FOLLOW))
				die("failure: linkat");
			if (close(tmpfile_fd))
				die("failure: close");
			if (is_setgid(open_tree_fd, FILE3, 0))
				die("failure: is_setgid");
			if (is_ixgrp(open_tree_fd, FILE3, 0))
				die("failure: is_ixgrp");
			if (!expected_uid_gid(open_tree_fd, FILE3, 0, 10000, 10000))
				die("failure: check ownership");
			if (unlinkat(open_tree_fd, FILE3, 0))
				die("failure: delete");
		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		umask(S_IXGRP);
		mode = umask(S_IXGRP);
		if (!(mode & S_IXGRP))
			die("failure: umask");

		/* The group permissions correspond to the permissions of the
		 * ACL_GROUP_OBJ entry. Don't use setfacl to set ACL_MASK, so
		 * the group permissions is equal to ACL_GROUP_OBJ(g)
		 * entry(rwx). Also, umask doesn't affect group permissions
		 * because umask will be ignored if having acl.
		 */
		snprintf(t_buf, sizeof(t_buf), "setfacl -d -m u::rwx,g::rwx,o::rwx, %s/%s", info->t_mountpoint, T_DIR1);
		if (system(t_buf))
			die("failure: system");

		if (!switch_ids(10000, 11000))
			die("failure: switch fsids");

		/* create regular file via open() */
		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, S_IXGRP | S_ISGID);
		if (file1_fd < 0)
			die("failure: create");

		/* Neither in_group_p() nor capable_wrt_inode_uidgid() so setgid
		 * bit needs to be stripped.
		 */
		if (is_setgid(open_tree_fd, FILE1, 0))
			die("failure: is_setgid");

		if (!is_ixgrp(open_tree_fd, FILE1, 0))
			die("failure: is_ixgrp");

		/* create directory */
		if (mkdirat(open_tree_fd, DIR1, 0000))
			die("failure: create");

		if (xfs_irix_sgid_inherit_enabled(info->t_fstype)) {
			/* We're not in_group_p(). */
			if (is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		} else {
			/* Directories always inherit the setgid bit. */
			if (!is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		}

		if (is_ixgrp(open_tree_fd, DIR1, 0))
			die("failure: is_ixgrp");

		/* create a special file via mknodat() vfs_create */
		if (mknodat(open_tree_fd, FILE2, S_IFREG | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, FILE2, 0))
			die("failure: is_setgid");

		if (!is_ixgrp(open_tree_fd, FILE2, 0))
			die("failure: is_ixgrp");

		/* create a whiteout device via mknodat() vfs_mknod */
		if (mknodat(open_tree_fd, CHRDEV1, S_IFCHR | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, CHRDEV1, 0))
			die("failure: is_setgid");

		if (!is_ixgrp(open_tree_fd, CHRDEV1, 0))
			die("failure: is_ixgrp");

		/*
		 * In setgid directories newly created files always inherit the
		 * gid from the parent directory. Verify that the file is owned
		 * by gid 10000, not by gid 11000.
		 */
		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 10000, 10000))
			die("failure: check ownership");

		/*
		 * In setgid directories newly created directories always
		 * inherit the gid from the parent directory. Verify that the
		 * directory is owned by gid 10000, not by gid 11000.
		 */
		if (!expected_uid_gid(open_tree_fd, DIR1, 0, 10000, 10000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, FILE2, 0, 10000, 10000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, CHRDEV1, 0, 10000, 10000))
			die("failure: check ownership");

		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, DIR1, AT_REMOVEDIR))
			die("failure: delete");

		if (unlinkat(open_tree_fd, FILE2, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, CHRDEV1, 0))
			die("failure: delete");

		/* create tmpfile via filesystem tmpfile api */
		if (supported) {
			tmpfile_fd = openat(open_tree_fd, ".", O_TMPFILE | O_RDWR, S_IXGRP | S_ISGID);
			if (tmpfile_fd < 0)
				die("failure: create");
			/* link the temporary file into the filesystem, making it permanent */
			snprintf(path, PATH_MAX,  "/proc/self/fd/%d", tmpfile_fd);
			if (linkat(AT_FDCWD, path, open_tree_fd, FILE3, AT_SYMLINK_FOLLOW))
				die("failure: linkat");
			if (close(tmpfile_fd))
				die("failure: close");
			if (is_setgid(open_tree_fd, FILE3, 0))
				die("failure: is_setgid");
			if (!is_ixgrp(open_tree_fd, FILE3, 0))
				die("failure: is_ixgrp");
			if (!expected_uid_gid(open_tree_fd, FILE3, 0, 10000, 10000))
				die("failure: check ownership");
			if (unlinkat(open_tree_fd, FILE3, 0))
				die("failure: delete");
		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

/*
 * If the parent directory has a default acl then permissions are based off
 * of that and current_umask() is ignored. Specifically, if the ACL has an
 * ACL_MASK entry, the group permissions correspond to the permissions of
 * the ACL_MASK entry. Otherwise, if the ACL has no ACL_MASK entry, the
 * group permissions correspond to the permissions of the ACL_GROUP_OBJ
 * entry.
 *
 * Use setfacl to check whether inode strip S_ISGID works correctly under
 * the above two situations when enabling userns and idmapped feature.
 *
 * Test for commit
 * 1639a49ccdce ("fs: move S_ISGID stripping into the vfs_*() helpers").
 */
static int setgid_create_acl_idmapped_in_userns(const struct vfstest_info *info)
{
	int fret = -1;
	int file1_fd = -EBADF, open_tree_fd = -EBADF;
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_IDMAP,
	};
	pid_t pid;
	int tmpfile_fd = -EBADF;
	bool supported = false;
	char path[PATH_MAX];
	mode_t mode;

	if (!caps_supported())
		return 0;

	if (fchmod(info->t_dir1_fd, S_IRUSR |
			      S_IWUSR |
			      S_IRGRP |
			      S_IWGRP |
			      S_IROTH |
			      S_IWOTH |
			      S_IXUSR |
			      S_IXGRP |
			      S_IXOTH |
			      S_ISGID), 0) {
		log_stderr("failure: fchmod");
		goto out;
	}

	/* Verify that the sid bits got raised. */
	if (!is_setgid(info->t_dir1_fd, "", AT_EMPTY_PATH)) {
		log_stderr("failure: is_setgid");
		goto out;
	}

	/* Changing mount properties on a detached mount. */
	attr.userns_fd	= get_userns_fd(0, 10000, 10000);
	if (attr.userns_fd < 0) {
		log_stderr("failure: get_userns_fd");
		goto out;
	}

	open_tree_fd = sys_open_tree(info->t_dir1_fd, "",
				     AT_EMPTY_PATH |
				     AT_NO_AUTOMOUNT |
				     AT_SYMLINK_NOFOLLOW |
				     OPEN_TREE_CLOEXEC |
				     OPEN_TREE_CLONE);
	if (open_tree_fd < 0) {
		log_stderr("failure: sys_open_tree");
		goto out;
	}

	if (sys_mount_setattr(open_tree_fd, "", AT_EMPTY_PATH, &attr, sizeof(attr))) {
		log_stderr("failure: sys_mount_setattr");
		goto out;
	}

	supported = openat_tmpfile_supported(open_tree_fd);

	/*
	 * Below we verify that setgid inheritance for a newly created file or
	 * directory works correctly. As part of this we need to verify that
	 * newly created files or directories inherit their gid from their
	 * parent directory. So we change the parent directorie's gid to 1000
	 * and create a file with fs{g,u}id 0 and verify that the newly created
	 * file and directory inherit gid 1000, not 0.
	 */
	if (fchownat(info->t_dir1_fd, "", -1, 1000, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
		log_stderr("failure: fchownat");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		umask(S_IXGRP);
		mode = umask(S_IXGRP);
		if (!(mode & S_IXGRP))
			die("failure: umask");

		/* The group permissions correspond to the permissions of the
		 * ACL_MASK entry. Use setfacl to set ACL mask(m) as rw, so now
		 * the group permissions is rw. Also, umask doesn't affect
		 * group permissions because umask will be ignored if having
		 * acl.
		 */
		snprintf(t_buf, sizeof(t_buf), "setfacl -d -m u::rwx,g::rw,o::rwx,m:rw %s/%s", info->t_mountpoint, T_DIR1);
		if (system(t_buf))
			die("failure: system");

		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		if (!caps_down_fsetid())
			die("failure: caps_down_fsetid");

		/* create regular file via open() */
		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, S_IXGRP | S_ISGID);
		if (file1_fd < 0)
			die("failure: create");

		/* Neither in_group_p() nor capable_wrt_inode_uidgid() so setgid
		 * bit needs to be stripped.
		 */
		if (is_setgid(open_tree_fd, FILE1, 0))
			die("failure: is_setgid");

		if (is_ixgrp(open_tree_fd, FILE1, 0))
			die("failure: is_ixgrp");

		/* create directory */
		if (mkdirat(open_tree_fd, DIR1, 0000))
			die("failure: create");

		if (xfs_irix_sgid_inherit_enabled(info->t_fstype)) {
			/* We're not in_group_p(). */
			if (is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		} else {
			/* Directories always inherit the setgid bit. */
			if (!is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		}

		if (is_ixgrp(open_tree_fd, DIR1, 0))
			die("failure: is_ixgrp");

		/* create a special file via mknodat() vfs_create */
		if (mknodat(open_tree_fd, FILE2, S_IFREG | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, FILE2, 0))
			die("failure: is_setgid");

		if (is_ixgrp(open_tree_fd, FILE2, 0))
			die("failure: is_ixgrp");

		/* create a whiteout device via mknodat() vfs_mknod */
		if (mknodat(open_tree_fd, CHRDEV1, S_IFCHR | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, CHRDEV1, 0))
			die("failure: is_setgid");

		if (is_ixgrp(open_tree_fd, CHRDEV1, 0))
			die("failure: is_ixgrp");

		/*
		 * In setgid directories newly created files always inherit the
		 * gid from the parent directory. Verify that the file is owned
		 * by gid 1000, not by gid 0.
		 */
		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 1000))
			die("failure: check ownership");

		/*
		 * In setgid directories newly created directories always
		 * inherit the gid from the parent directory. Verify that the
		 * directory is owned by gid 1000, not by gid 0.
		 */
		if (!expected_uid_gid(open_tree_fd, DIR1, 0, 0, 1000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, FILE2, 0, 0, 1000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, CHRDEV1, 0, 0, 1000))
			die("failure: check ownership");

		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, DIR1, AT_REMOVEDIR))
			die("failure: delete");

		if (unlinkat(open_tree_fd, FILE2, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, CHRDEV1, 0))
			die("failure: delete");

		/* create tmpfile via filesystem tmpfile api */
		if (supported) {
			tmpfile_fd = openat(open_tree_fd, ".", O_TMPFILE | O_RDWR, S_IXGRP | S_ISGID);
			if (tmpfile_fd < 0)
				die("failure: create");
			/* link the temporary file into the filesystem, making it permanent */
			snprintf(path, PATH_MAX,  "/proc/self/fd/%d", tmpfile_fd);
			if (linkat(AT_FDCWD, path, open_tree_fd, FILE3, AT_SYMLINK_FOLLOW))
				die("failure: linkat");
			if (close(tmpfile_fd))
				die("failure: close");
			if (is_setgid(open_tree_fd, FILE3, 0))
				die("failure: is_setgid");
			if (is_ixgrp(open_tree_fd, FILE3, 0))
				die("failure: is_ixgrp");
			if (!expected_uid_gid(open_tree_fd, FILE3, 0, 0, 1000))
				die("failure: check ownership");
			if (unlinkat(open_tree_fd, FILE3, 0))
				die("failure: delete");
		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	pid = fork();
	if (pid < 0) {
		log_stderr("failure: fork");
		goto out;
	}
	if (pid == 0) {
		umask(S_IXGRP);
		mode = umask(S_IXGRP);
		if (!(mode & S_IXGRP))
			die("failure: umask");

		/* The group permissions correspond to the permissions of the
		 * ACL_GROUP_OBJ entry. Don't use setfacl to set ACL_MASK, so
		 * the group permissions is equal to ACL_GROUP_OBJ(g)
		 * entry(rwx). Also, umask doesn't affect group permissions
		 * because umask will be ignored if having acl.
		 */
		snprintf(t_buf, sizeof(t_buf), "setfacl -d -m u::rwx,g::rwx,o::rwx, %s/%s", info->t_mountpoint, T_DIR1);
		if (system(t_buf))
			die("failure: system");

		if (!caps_supported()) {
			log_debug("skip: capability library not installed");
			exit(EXIT_SUCCESS);
		}

		if (!switch_userns(attr.userns_fd, 0, 0, false))
			die("failure: switch_userns");

		if (!caps_down_fsetid())
			die("failure: caps_down_fsetid");

		/* create regular file via open() */
		file1_fd = openat(open_tree_fd, FILE1, O_CREAT | O_EXCL | O_CLOEXEC, S_IXGRP | S_ISGID);
		if (file1_fd < 0)
			die("failure: create");

		/* Neither in_group_p() nor capable_wrt_inode_uidgid() so setgid
		 * bit needs to be stripped.
		 */
		if (is_setgid(open_tree_fd, FILE1, 0))
			die("failure: is_setgid");

		if (!is_ixgrp(open_tree_fd, FILE1, 0))
			die("failure: is_ixgrp");

		/* create directory */
		if (mkdirat(open_tree_fd, DIR1, 0000))
			die("failure: create");

		if (xfs_irix_sgid_inherit_enabled(info->t_fstype)) {
			/* We're not in_group_p(). */
			if (is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		} else {
			/* Directories always inherit the setgid bit. */
			if (!is_setgid(open_tree_fd, DIR1, 0))
				die("failure: is_setgid");
		}

		if (is_ixgrp(open_tree_fd, DIR1, 0))
			die("failure: is_ixgrp");

		/* create a special file via mknodat() vfs_create */
		if (mknodat(open_tree_fd, FILE2, S_IFREG | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, FILE2, 0))
			die("failure: is_setgid");

		if (!is_ixgrp(open_tree_fd, FILE2, 0))
			 die("failure: is_ixgrp");
		/* create a whiteout device via mknodat() vfs_mknod */
		if (mknodat(open_tree_fd, CHRDEV1, S_IFCHR | S_ISGID | S_IXGRP, 0))
			die("failure: mknodat");

		if (is_setgid(open_tree_fd, CHRDEV1, 0))
			die("failure: is_setgid");

		if (!is_ixgrp(open_tree_fd, CHRDEV1, 0))
			 die("failure: is_ixgrp");

		/*
		 * In setgid directories newly created files always inherit the
		 * gid from the parent directory. Verify that the file is owned
		 * by gid 1000, not by gid 0.
		 */
		if (!expected_uid_gid(open_tree_fd, FILE1, 0, 0, 1000))
			die("failure: check ownership");

		/*
		 * In setgid directories newly created directories always
		 * inherit the gid from the parent directory. Verify that the
		 * directory is owned by gid 1000, not by gid 0.
		 */
		if (!expected_uid_gid(open_tree_fd, DIR1, 0, 0, 1000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, FILE2, 0, 0, 1000))
			die("failure: check ownership");

		if (!expected_uid_gid(open_tree_fd, CHRDEV1, 0, 0, 1000))
			die("failure: check ownership");

		if (unlinkat(open_tree_fd, FILE1, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, DIR1, AT_REMOVEDIR))
			die("failure: delete");

		if (unlinkat(open_tree_fd, FILE2, 0))
			die("failure: delete");

		if (unlinkat(open_tree_fd, CHRDEV1, 0))
			die("failure: delete");

		/* create tmpfile via filesystem tmpfile api */
		if (supported) {
			tmpfile_fd = openat(open_tree_fd, ".", O_TMPFILE | O_RDWR, S_IXGRP | S_ISGID);
			if (tmpfile_fd < 0)
				die("failure: create");
			/* link the temporary file into the filesystem, making it permanent */
			snprintf(path, PATH_MAX,  "/proc/self/fd/%d", tmpfile_fd);
			if (linkat(AT_FDCWD, path, open_tree_fd, FILE3, AT_SYMLINK_FOLLOW))
				die("failure: linkat");
			if (close(tmpfile_fd))
				die("failure: close");
			if (is_setgid(open_tree_fd, FILE3, 0))
				die("failure: is_setgid");
			if (!is_ixgrp(open_tree_fd, FILE3, 0))
				die("failure: is_ixgrp");
			if (!expected_uid_gid(open_tree_fd, FILE3, 0, 0, 1000))
				die("failure: check ownership");
			if (unlinkat(open_tree_fd, FILE3, 0))
				die("failure: delete");
		}

		exit(EXIT_SUCCESS);
	}
	if (wait_for_pid(pid))
		goto out;

	fret = 0;
	log_debug("Ran test");
out:
	safe_close(attr.userns_fd);
	safe_close(file1_fd);
	safe_close(open_tree_fd);

	return fret;
}

static const struct test_struct t_idmapped_mounts[] = {
	{ tcore_acls,                                                         true,   "posix acls on regular mounts",                                                                 },
	{ tcore_create_in_userns,                                             true,   "create operations in user namespace",                                                          },
	{ tcore_device_node_in_userns,                                        true,   "device node in user namespace",                                                                },
	{ tcore_expected_uid_gid_idmapped_mounts,				true,	"expected ownership on idmapped mounts",							},
	{ tcore_fscaps_idmapped_mounts,					true,	"fscaps on idmapped mounts",									},
	{ tcore_fscaps_idmapped_mounts_in_userns,				true,	"fscaps on idmapped mounts in user namespace",							},
	{ tcore_fscaps_idmapped_mounts_in_userns_separate_userns,		true,	"fscaps on idmapped mounts in user namespace with different id mappings",			},
	{ tcore_fsids_mapped,                                                 true,   "mapped fsids",                                                                                 },
	{ tcore_fsids_unmapped,                                               true,   "unmapped fsids",                                                                               },
	{ tcore_hardlink_crossing_idmapped_mounts,				true,	"cross idmapped mount hardlink",								},
	{ tcore_hardlink_from_idmapped_mount,					true,	"hardlinks from idmapped mounts",								},
	{ tcore_hardlink_from_idmapped_mount_in_userns,			true,	"hardlinks from idmapped mounts in user namespace",						},
#ifdef HAVE_LIBURING_H
	{ tcore_io_uring_idmapped,						true,	"io_uring from idmapped mounts",								},
	{ tcore_io_uring_idmapped_userns,					true,	"io_uring from idmapped mounts in user namespace",						},
	{ tcore_io_uring_idmapped_unmapped,					true,	"io_uring from idmapped mounts with unmapped ids",						},
	{ tcore_io_uring_idmapped_unmapped_userns,				true,	"io_uring from idmapped mounts with unmapped ids in user namespace",				},
#endif
	{ tcore_protected_symlinks_idmapped_mounts,				true,	"following protected symlinks on idmapped mounts",						},
	{ tcore_protected_symlinks_idmapped_mounts_in_userns,			true,	"following protected symlinks on idmapped mounts in user namespace",				},
	{ tcore_rename_crossing_idmapped_mounts,				true,	"cross idmapped mount rename",									},
	{ tcore_rename_from_idmapped_mount,					true,	"rename from idmapped mounts",									},
	{ tcore_rename_from_idmapped_mount_in_userns,				true,	"rename from idmapped mounts in user namespace",						},
	{ tcore_setattr_truncate_idmapped,					true,	"setattr truncate on idmapped mounts",								},
	{ tcore_setattr_truncate_idmapped_in_userns,				true,	"setattr truncate on idmapped mounts in user namespace",					},
	{ tcore_setgid_create_idmapped,					true,	"create operations in directories with setgid bit set on idmapped mounts",			},
	{ tcore_setgid_create_idmapped_in_userns,				true,	"create operations in directories with setgid bit set on idmapped mounts in user namespace",	},
	{ tcore_setid_binaries_idmapped_mounts,				true,	"setid binaries on idmapped mounts",								},
	{ tcore_setid_binaries_idmapped_mounts_in_userns,			true,	"setid binaries on idmapped mounts in user namespace",						},
	{ tcore_setid_binaries_idmapped_mounts_in_userns_separate_userns,	true,	"setid binaries on idmapped mounts in user namespace with different id mappings",		},
	{ tcore_sticky_bit_unlink_idmapped_mounts,				true,	"sticky bit unlink operations on idmapped mounts",						},
	{ tcore_sticky_bit_unlink_idmapped_mounts_in_userns,			true,	"sticky bit unlink operations on idmapped mounts in user namespace",				},
	{ tcore_sticky_bit_rename_idmapped_mounts,				true,	"sticky bit rename operations on idmapped mounts",						},
	{ tcore_sticky_bit_rename_idmapped_mounts_in_userns,			true,	"sticky bit rename operations on idmapped mounts in user namespace",				},
	{ tcore_symlink_idmapped_mounts,					true,	"symlink from idmapped mounts",									},
	{ tcore_symlink_idmapped_mounts_in_userns,				true,	"symlink from idmapped mounts in user namespace",						},
};

const struct test_suite s_idmapped_mounts = {
	.tests		= t_idmapped_mounts,
	.nr_tests	= ARRAY_SIZE(t_idmapped_mounts),
};

static const struct test_struct t_fscaps_in_ancestor_userns[] = {
	{ fscaps_idmapped_mounts_in_userns_valid_in_ancestor_userns,	true,	"fscaps on idmapped mounts in user namespace writing fscap valid in ancestor userns",		},
};

const struct test_suite s_fscaps_in_ancestor_userns = {
	.tests		= t_fscaps_in_ancestor_userns,
	.nr_tests	= ARRAY_SIZE(t_fscaps_in_ancestor_userns),
};

static const struct test_struct t_nested_userns[] = {
	{ nested_userns,						T_REQUIRE_IDMAPPED_MOUNTS,	"test that nested user namespaces behave correctly when attached to idmapped mounts",		},
};

const struct test_suite s_nested_userns = {
	.tests = t_nested_userns,
	.nr_tests = ARRAY_SIZE(t_nested_userns),
};

/* Test for commit 968219708108 ("fs: handle circular mappings correctly"). */
static const struct test_struct t_setattr_fix_968219708108[] = {
	{ setattr_fix_968219708108,					T_REQUIRE_IDMAPPED_MOUNTS,	"test that setattr works correctly",								},
};

const struct test_suite s_setattr_fix_968219708108 = {
	.tests = t_setattr_fix_968219708108,
	.nr_tests = ARRAY_SIZE(t_setattr_fix_968219708108),
};

/* Test for commit 705191b03d50 ("fs: fix acl translation"). */
static const struct test_struct t_setxattr_fix_705191b03d50[] = {
	{ setxattr_fix_705191b03d50,					T_REQUIRE_USERNS,	"test that setxattr works correctly for userns mountable filesystems",				},
};

const struct test_suite s_setxattr_fix_705191b03d50 = {
	.tests = t_setxattr_fix_705191b03d50,
	.nr_tests = ARRAY_SIZE(t_setxattr_fix_705191b03d50),
};

static const struct test_struct t_setgid_create_umask_idmapped_mounts[] = {
	{ setgid_create_umask_idmapped,					T_REQUIRE_IDMAPPED_MOUNTS,	"create operations by using umask in directories with setgid bit set on idmapped mount",		},
	{ setgid_create_umask_idmapped_in_userns,			T_REQUIRE_IDMAPPED_MOUNTS,	"create operations by using umask in directories with setgid bit set on idmapped mount inside userns",	},
};

const struct test_suite s_setgid_create_umask_idmapped_mounts = {
	.tests = t_setgid_create_umask_idmapped_mounts,
	.nr_tests = ARRAY_SIZE(t_setgid_create_umask_idmapped_mounts),
};

static const struct test_struct t_setgid_create_acl_idmapped_mounts[] = {
	{ setgid_create_acl_idmapped,					T_REQUIRE_IDMAPPED_MOUNTS,	"create operations by using acl in directories with setgid bit set on idmapped mount",                },
	{ setgid_create_acl_idmapped_in_userns,				T_REQUIRE_IDMAPPED_MOUNTS,	"create operations by using acl in directories with setgid bit set on idmapped mount inside userns",  },
};

const struct test_suite s_setgid_create_acl_idmapped_mounts = {
	.tests = t_setgid_create_acl_idmapped_mounts,
	.nr_tests = ARRAY_SIZE(t_setgid_create_acl_idmapped_mounts),
};
