// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2019 Oracle.
 * All Rights Reserved.
 *
 * Test various aspects of behavior around setting the immutable flag.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <asm/unistd.h>
#include <linux/fs.h>

#ifndef FS_IOC_GETFLAGS
# define	FS_IOC_GETFLAGS			_IOR('f', 1, long)
#endif

#ifndef FS_IOC_SETFLAGS
# define	FS_IOC_SETFLAGS			_IOW('f', 2, long)
#endif

#ifndef FICLONERANGE
#define FICLONERANGE	_IOW(0x94, 13, struct file_clone_range)
struct file_clone_range {
	__s64 src_fd;
	__u64 src_offset;
	__u64 src_length;
	__u64 dest_offset;
};
#endif

static void die(const char *str)
{
	perror(str);
	exit(1);
}

static void seti(int fd)
{
	int ret, flags;

	ret = ioctl(fd, FS_IOC_GETFLAGS, &flags);
	if (ret)
		die("getflags");
	flags |= FS_IMMUTABLE_FL;
	ret = ioctl(fd, FS_IOC_SETFLAGS, &flags);
	if (ret)
		die("setflags");
}

static void cleari(int fd)
{
	int ret, flags;

	ret = ioctl(fd, FS_IOC_GETFLAGS, &flags);
	if (ret)
		die("getflags");
	flags &= ~FS_IMMUTABLE_FL;
	ret = ioctl(fd, FS_IOC_SETFLAGS, &flags);
	if (ret)
		die("setflags");
}

static int inc_projid(int fd)
{
	struct fsxattr	fa;
	int		ret;

	ret = ioctl(fd, FS_IOC_FSGETXATTR, &fa);
	if (ret)
		die("fsgetxattr");
	fa.fsx_projid++;
	return ioctl(fd, FS_IOC_FSSETXATTR, &fa);
}

static void eat_signal(int sig)
{
	fprintf(stderr, "signal %d received!\n", sig);
	exit(2);
}

#define BUFFER_SZ 65536
static char buffer[BUFFER_SZ];

int main(int argc, char *argv[])
{
	struct sigaction sa = {
		.sa_handler = eat_signal,
	};
	char *p;
	ssize_t sz;
	int fd;
	int ret;
	long int testnum;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s number file\n", argv[0]);
		return 1;
	}

	errno = 0;
	testnum = strtol(argv[1], NULL, 10);
	if (errno)
		die(argv[1]);

	/* Try to remove file. */
	fd = open(argv[2], O_RDONLY);
	if (fd >= 0) {
		cleari(fd);
		ret = close(fd);
		if (ret)
			die("rclose");
		ret = unlink(argv[2]);
		if (ret)
			die("unlink");
	}

	ret = sigaction(SIGBUS, &sa, NULL);
	if (ret)
		die("sigaction");

	/* Create file */
	fd = open(argv[2], O_CREAT | O_RDWR, 0700);
	if (fd < 0)
		die(argv[2]);

	sz = write(fd, buffer, BUFFER_SZ);
	if (sz < 0)
		die("write");

	ret = fsync(fd);
	if (ret)
		die("fsync");

	switch (testnum) {
	case 0:
		/* Test writing after being made immutable. */
		seti(fd);
		sz = write(fd, buffer, BUFFER_SZ);
		if (sz < 0)
			perror("write immutable");
		cleari(fd);
		break;
	case 1:
		/* Test writing to a clean mmap after being made immutable. */
		p = mmap(NULL, BUFFER_SZ, PROT_READ | PROT_WRITE, MAP_SHARED,
				fd, 0);
		if (p == MAP_FAILED)
			die("mmap");

		seti(fd);
		*p = 6;
		cleari(fd);
		break;
	case 2:
		/* Test writing to a dirty mmap after being made immutable. */
		p = mmap(NULL, BUFFER_SZ, PROT_READ | PROT_WRITE, MAP_SHARED,
				fd, 0);
		if (p == MAP_FAILED)
			die("mmap");

		*p = 7;
		seti(fd);
		*p = 8;
		cleari(fd);
		break;
	case 3:
		/* Test truncating after being made immutable. */
		seti(fd);
		ret = ftruncate(fd, BUFFER_SZ / 2);
		if (ret)
			perror("ftruncate");
		cleari(fd);
		break;
	case 4:
		/* Test fallocate after being made immutable. */
		seti(fd);
		ret = fallocate(fd, 0, 0, BUFFER_SZ);
		if (ret)
			perror("fallocate");
		cleari(fd);
		break;
	case 5:
		/* Test fchmod after being made immutable. */
		seti(fd);
		ret = fchmod(fd, 0000);
		if (ret)
			perror("fchmod");
		cleari(fd);
		break;
	case 6: {
#ifdef __NR_copy_file_range
		loff_t o1, o2;
#endif
		/* Test copy_file_range after being made immutable. */
		seti(fd);
#ifdef __NR_copy_file_range
		o1 = 0;
		o2 = 1;
		sz = syscall(__NR_copy_file_range, fd, &o1, fd, &o2, 1, 0);
#else
		errno = EPERM;
		sz = 0;
#endif
		if (sz != 1)
			perror("copy_file_range");
		cleari(fd);
		break;
	}
	case 7: {
		/* Test ficlonerange after being made immutable. */
		struct file_clone_range f = {
			.src_fd = fd,
			.src_offset = 0,
			.src_length = BUFFER_SZ,
			.dest_offset = BUFFER_SZ,
		};
		seti(fd);
		ret = ioctl(fd, FICLONERANGE, &f);
			perror("ficlonerange");
		cleari(fd);
		break;
	}
	case 8: {
		/* Test futimes after being made immutable. */
		struct timeval tv[2];
		seti(fd);
		ret = futimes(fd, tv);
		if (ret)
			perror("futimes");
		cleari(fd);
		break;
	}
	case 9: {
		/* Test trying to set project id on immutable file. */
		ret = inc_projid(fd);
		if (ret)
			die("setprojid 1");
		seti(fd);
		ret = inc_projid(fd);
		if (ret)
			perror("setprojid 2");
		cleari(fd);
		ret = inc_projid(fd);
		if (ret)
			die("setprojid 3");
		break;
	}
	default:
		die("unknown test number");
		break;
	}

	/* Bye! */
	ret = close(fd);
	if (ret)
		die("close");

	return 0;
}
