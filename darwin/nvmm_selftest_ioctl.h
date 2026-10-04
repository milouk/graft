/*
 * nvmm_selftest_ioctl.h — interface of the glue self-test device.
 * Shared between the kernel extension and test/darwin/selftest.c.
 */
#ifndef _NVMM_SELFTEST_IOCTL_H_
#define _NVMM_SELFTEST_IOCTL_H_

#include <sys/ioccom.h>
#include <stdint.h>

#define NVMM_SELFTEST_LOGSZ	3072

/* Run the in-kernel checks and report. */
struct nvmm_selftest_run {
	uint32_t passed;
	uint32_t failed;
	uint32_t ncpus;
	uint32_t sleeps;	/* sleep notifications seen since load */
	uint32_t wakes;
	uint32_t pad;
	char log[NVMM_SELFTEST_LOGSZ];
};

/*
 * Create a wired buffer and map it into the calling process, the way NVMM maps
 * guest RAM. If 'fixed', it must land exactly at 'addr', replacing what is
 * there. The kernel writes a pattern derived from 'seed' through its own
 * mapping; the caller checks it through the user mapping.
 */
struct nvmm_selftest_map {
	uint64_t size;
	uint64_t addr;		/* in: wanted address if fixed; out: actual */
	uint32_t fixed;
	uint32_t seed;
};

/* Ask the kernel whether it sees the pattern the caller wrote back. */
struct nvmm_selftest_verify {
	uint32_t seed;
	uint32_t ok;		/* out */
	uint64_t first_bad;	/* out: offset of the first mismatch */
};

#define NVMM_SELFTEST_IOC_RUN	 _IOWR('S', 1, struct nvmm_selftest_run)
#define NVMM_SELFTEST_IOC_MAP	 _IOWR('S', 2, struct nvmm_selftest_map)
#define NVMM_SELFTEST_IOC_VERIFY _IOWR('S', 3, struct nvmm_selftest_verify)
#define NVMM_SELFTEST_IOC_UNMAP	 _IO  ('S', 4)

/* The byte the kernel writes, and expects back inverted, at each offset. */
static inline uint8_t
nvmm_selftest_pattern(uint32_t seed, uint64_t off)
{
	return (uint8_t)((off * 2654435761u) >> 13) ^ (uint8_t)seed ^
	    (uint8_t)(off >> 12);
}

#endif /* _NVMM_SELFTEST_IOCTL_H_ */
