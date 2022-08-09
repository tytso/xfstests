/* SPDX-License-Identifier: GPL-2.0-or-later WITH Linux-syscall-note */
/*
 * FIEXCHANGE ioctl definitions, to facilitate exchanging parts of files.
 *
 * Copyright (C) 2022 Oracle.  All Rights Reserved.
 *
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef _LINUX_FIEXCHANGE_H
#define _LINUX_FIEXCHANGE_H

#include <linux/types.h>

/*
 * Exchange part of file1 with part of the file that this ioctl that is being
 * called against (which we'll call file2).  Filesystems must be able to
 * restart and complete the operation even after the system goes down.
 */
struct file_xchg_range {
	__s64		file1_fd;
	__s64		file1_offset;	/* file1 offset, bytes */
	__s64		file2_offset;	/* file2 offset, bytes */
	__s64		length;		/* bytes to exchange */

	__u64		flags;		/* see FILE_XCHG_RANGE_* below */

	/* file2 metadata for optional freshness checks */
	__s64		file2_ino;	/* inode number */
	__s64		file2_mtime;	/* modification time */
	__s64		file2_ctime;	/* change time */
	__s32		file2_mtime_nsec; /* mod time, nsec */
	__s32		file2_ctime_nsec; /* change time, nsec */

	__u64		pad[6];		/* must be zeroes */
};

/*
 * Atomic exchange operations are not required.  This relaxes the requirement
 * that the filesystem must be able to complete the operation after a crash.
 */
#define FILE_XCHG_RANGE_NONATOMIC	(1 << 0)

/*
 * Check that file2's inode number, mtime, and ctime against the values
 * provided, and return -EBUSY if there isn't an exact match.
 */
#define FILE_XCHG_RANGE_FILE2_FRESH	(1 << 1)

/*
 * Check that the file1's length is equal to file1_offset + length, and that
 * file2's length is equal to file2_offset + length.  Returns -EDOM if there
 * isn't an exact match.
 */
#define FILE_XCHG_RANGE_FULL_FILES	(1 << 2)

/*
 * Exchange file data all the way to the ends of both files, and then exchange
 * the file sizes.  This flag can be used to replace a file's contents with a
 * different amount of data.  length will be ignored.
 */
#define FILE_XCHG_RANGE_TO_EOF		(1 << 3)

/* Flush all changes in file data and file metadata to disk before returning. */
#define FILE_XCHG_RANGE_FSYNC		(1 << 4)

/* Dry run; do all the parameter verification but do not change anything. */
#define FILE_XCHG_RANGE_DRY_RUN		(1 << 5)

/*
 * Do not exchange any part of the range where file1's mapping is a hole.  This
 * can be used to emulate scatter-gather atomic writes with a temp file.
 */
#define FILE_XCHG_RANGE_SKIP_FILE1_HOLES (1 << 6)

/*
 * Commit the contents of file1 into file2 if file2 has the same inode number,
 * mtime, and ctime as the arguments provided to the call.  The old contents of
 * file2 will be moved to file1.
 *
 * With this flag, all committed information can be retrieved even if the
 * system crashes or is rebooted.  This includes writing through or flushing a
 * disk cache if present.  The call blocks until the device reports that the
 * commit is complete.
 *
 * This flag should not be combined with NONATOMIC.  It can be combined with
 * SKIP_FILE1_HOLES.
 */
#define FILE_XCHG_RANGE_COMMIT		(FILE_XCHG_RANGE_FILE2_FRESH | \
					 FILE_XCHG_RANGE_FSYNC)

#define FILE_XCHG_RANGE_ALL_FLAGS	(FILE_XCHG_RANGE_NONATOMIC | \
					 FILE_XCHG_RANGE_FILE2_FRESH | \
					 FILE_XCHG_RANGE_FULL_FILES | \
					 FILE_XCHG_RANGE_TO_EOF | \
					 FILE_XCHG_RANGE_FSYNC | \
					 FILE_XCHG_RANGE_DRY_RUN | \
					 FILE_XCHG_RANGE_SKIP_FILE1_HOLES)

#define FIEXCHANGE_RANGE	_IOWR('X', 129, struct file_xchg_range)

#endif /* _LINUX_FIEXCHANGE_H */
