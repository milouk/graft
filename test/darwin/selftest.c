/*
 * selftest.c — drive /dev/nvmm-selftest (the NVMMSelfTest kext).
 *
 * Runs the in-kernel checks, then checks from the process side the thing the
 * kernel cannot check alone: that memory the kext maps into this process is
 * the same memory the kernel sees, at the address that was asked for.
 *
 * Usage (as root, with NVMMSelfTest.kext loaded):
 *   nvmm-selftest            run everything once
 *   nvmm-selftest --leak     map, then exit without unmapping or closing
 */

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvmm_selftest_ioctl.h"

static int failures;

#define CHECK(cond)							\
	do {								\
		if (!(cond)) {						\
			printf("FAIL %s:%d: %s (errno %d: %s)\n",	\
			    __FILE__, __LINE__, #cond, errno,		\
			    strerror(errno));				\
			failures++;					\
		}							\
	} while (0)

/* Map through the kext, check both directions, optionally unmap. */
static void
map_roundtrip(int fd, uint64_t size, int fixed, uint32_t seed, int unmap)
{
	struct nvmm_selftest_map map;
	struct nvmm_selftest_verify ver;
	uint8_t *p, *want = NULL;
	uint64_t i, bad = 0;

	memset(&map, 0, sizeof(map));
	map.size = size;
	map.fixed = (uint32_t)fixed;
	map.seed = seed;

	if (fixed) {
		/*
		 * Reserve a range the way an emulator allocates guest RAM, and
		 * fill it, so that "replaced" is distinguishable from "left
		 * alone".
		 */
		want = mmap(NULL, size, PROT_READ | PROT_WRITE,
		    MAP_ANON | MAP_PRIVATE, -1, 0);
		CHECK(want != MAP_FAILED);
		if (want == MAP_FAILED)
			return;
		memset(want, 0xEE, size);
		map.addr = (uint64_t)(uintptr_t)want;
	}

	CHECK(ioctl(fd, NVMM_SELFTEST_IOC_MAP, &map) == 0);
	if (map.addr == 0) {
		failures++;
		return;
	}
	p = (uint8_t *)(uintptr_t)map.addr;
	if (fixed)
		CHECK(p == want);

	/* The kernel's pattern is visible here... */
	for (i = 0; i < size; i++) {
		if (p[i] != nvmm_selftest_pattern(seed, i)) {
			bad++;
			if (bad == 1)
				printf("  first mismatch at offset %#llx: "
				    "got %#x\n", (unsigned long long)i, p[i]);
		}
	}
	CHECK(bad == 0);

	/* ...and what is written here is visible to the kernel. */
	for (i = 0; i < size; i++)
		p[i] = (uint8_t)~nvmm_selftest_pattern(seed, i);
	memset(&ver, 0, sizeof(ver));
	ver.seed = seed;
	CHECK(ioctl(fd, NVMM_SELFTEST_IOC_VERIFY, &ver) == 0);
	CHECK(ver.ok == 1);

	printf("  map %s %llu KiB at %p: %s\n", fixed ? "fixed   " : "anywhere",
	    (unsigned long long)(size >> 10), (void *)p,
	    (bad == 0 && ver.ok == 1) ? "ok" : "MISMATCH");

	if (unmap) {
		CHECK(ioctl(fd, NVMM_SELFTEST_IOC_UNMAP) == 0);
		/* A second unmap has nothing to act on. */
		CHECK(ioctl(fd, NVMM_SELFTEST_IOC_UNMAP) == -1 &&
		    errno == ENOENT);
		errno = 0;
	}
}

int
main(int argc, char **argv)
{
	static struct nvmm_selftest_run run;
	const int leak = (argc > 1 && strcmp(argv[1], "--leak") == 0);
	int fd, fd2, round;

	fd = open("/dev/nvmm-selftest", O_RDWR);
	if (fd == -1) {
		perror("/dev/nvmm-selftest");
		return 2;
	}

	if (leak) {
		/* Leave a 64 MiB mapping behind; the kernel must clean up. */
		map_roundtrip(fd, 64ULL << 20, 1, 0x4C, 0);
		printf("exiting with the mapping in place\n");
		return failures ? 1 : 0;
	}

	CHECK(ioctl(fd, NVMM_SELFTEST_IOC_RUN, &run) == 0);
	run.log[NVMM_SELFTEST_LOGSZ - 1] = '\0';
	printf("in-kernel checks: %u passed, %u failed, %u cpus, "
	    "%u sleeps, %u wakes\n%s", run.passed, run.failed, run.ncpus,
	    run.sleeps, run.wakes, run.log);
	if (run.failed != 0)
		failures += (int)run.failed;

	printf("process-side mapping checks:\n");
	for (round = 0; round < 3; round++) {
		map_roundtrip(fd, 4096, 0, 0x10 + (uint32_t)round, 1);
		map_roundtrip(fd, 1ULL << 20, 0, 0x20 + (uint32_t)round, 1);
		map_roundtrip(fd, 4096, 1, 0x30 + (uint32_t)round, 1);
		map_roundtrip(fd, 16ULL << 20, 1, 0x40 + (uint32_t)round, 1);
	}

	/* Two opens are independent: each has its own mapping. */
	fd2 = open("/dev/nvmm-selftest", O_RDWR);
	CHECK(fd2 != -1);
	if (fd2 != -1) {
		map_roundtrip(fd, 1ULL << 20, 1, 0x51, 0);
		map_roundtrip(fd2, 1ULL << 20, 1, 0x52, 1);
		CHECK(ioctl(fd, NVMM_SELFTEST_IOC_UNMAP) == 0);
		/* Close with a mapping still in place: the sweep handles it. */
		map_roundtrip(fd2, 8ULL << 20, 0, 0x53, 0);
		CHECK(close(fd2) == 0);
	}

	CHECK(close(fd) == 0);
	printf("%s\n", failures ? "SELFTEST FAILED" : "SELFTEST PASSED");
	return failures ? 1 : 0;
}
