// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 *
 * Test program to try to force the VMM to swap pages in and out of memory.
 */
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/swap.h>
#include <sys/time.h>
#include <sys/resource.h>

int main(int argc, char *argv[])
{
	struct rlimit rlim;
	long long nr_bytes;
	long pagesize;
	char *p, *pstart, *pend;
	int ret;

	if (argc != 2 && argc != 4) {
		printf("Usage: %s mem_in_bytes [swapfile1 swapfile2]\n",
				argv[0]);
		return 1;
	}

	pagesize = sysconf(_SC_PAGESIZE);
	if (pagesize < 1) {
		fprintf(stderr, "Cannot determine system page size.\n");
		return 2;
	}

	errno = 0;
	nr_bytes = strtol(argv[1], &p, 0);
	if (errno) {
		perror(argv[1]);
		return 3;
	}

	printf("Allocating %llu memory.\n", nr_bytes);
	fflush(stdout);

	/* Allocate a large memory buffer. */
	pstart = malloc(nr_bytes);
	if (!pstart) {
		perror("malloc");
		return 4;
	}
	pend = pstart + nr_bytes;

	printf("Dirtying memory.\n");
	fflush(stdout);

	/*
	 * For each memory page, copy our process' pointer into the start of
	 * each of those pages so that we can check them later.  Dirtying every
	 * page should result in at least some of them being paged out.
	 */
	for (p = pstart; p < pend; p += pagesize)
		memcpy(p, &p, sizeof(char *));

	/*
	 * If the caller does not provide any swapfile names, mlock the buffer
	 * to test if memory usage enforcement actually works.
	 */
	if (argc == 2) {
		printf("Now mlocking memory.\n");
		fflush(stdout);

		rlim.rlim_cur = RLIM_INFINITY;
		rlim.rlim_max = RLIM_INFINITY;
		ret = setrlimit(RLIMIT_MEMLOCK, &rlim);
		if (ret) {
			perror("setrlimit");
		}

		ret = mlock(pstart, nr_bytes);
		if (ret) {
			perror("mlock");
			return 0;
		}

		printf("Should not have survived mlock!\n");
		fflush(stdout);
		return 6;
	}

	/*
	 * Try to force the system to swap this program back into memory by
	 * activating the second swapfile (at maximum priority) and
	 * deactivating the first swapfile.
	 */
	printf("Now activating swapfile2.\n");
	fflush(stdout);

	ret = swapon(argv[3], SWAP_FLAG_PREFER | SWAP_FLAG_PRIO_MASK);
	if (ret) {
		perror("swapon");
		return 7;
	}

	printf("Now deactivating swapfile1.\n");
	fflush(stdout);

	ret = swapoff(argv[2]);
	if (ret) {
		perror("swapoff");
		return 8;
	}

	/*
	 * For each memory page, check the pointer that we stashed earlier at
	 * the start of each of those pages.  Dirty every page to force some
	 * more paging activity.
	 */
	for (p = pstart; p < pend; p += pagesize) {
		char *bort;

		memcpy(&bort, p, sizeof(char *));
		if (memcmp(&bort, &p, sizeof(char *)))
			printf("Saw %p, expected %p at offset %zu.\n", bort, p,
					p - pstart);
		*p = 'Y';
	}

	return 0;
}
