/*
 * npt_test.c — userspace test for port/npt.c.
 *
 * The page allocator hands out real memory with made-up physical addresses,
 * and the test then walks the tables the way an AMD CPU would: starting from
 * the root's physical address and following physical addresses only. That is
 * an independent check on what the builder wrote, not just on what it reports.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "npt.h"

#define PAGE	4096ULL
#define MAXPG	65536

static struct {
	void *va;
	uint64_t pa;
} pages[MAXPG];
static unsigned int npages_live, npages_total;
static uint64_t next_pa = 0x100000000ULL;	/* fake, above 4G */
static int fail_after = -1;			/* inject allocation failure */

static int
t_alloc(void *ctx, void **va, uint64_t *pa)
{
	(void)ctx;
	if (fail_after == 0)
		return -1;
	if (fail_after > 0)
		fail_after--;
	if (npages_total >= MAXPG)
		return -1;
	if (posix_memalign(va, PAGE, PAGE) != 0)
		return -1;
	memset(*va, 0, PAGE);
	*pa = next_pa;
	next_pa += PAGE;
	pages[npages_total].va = *va;
	pages[npages_total].pa = *pa;
	npages_total++;
	npages_live++;
	return 0;
}

static void
t_free(void *ctx, void *va, uint64_t pa)
{
	unsigned int i;

	(void)ctx;
	for (i = 0; i < npages_total; i++) {
		if (pages[i].pa == pa && pages[i].va == va) {
			pages[i].va = NULL;
			npages_live--;
			free(va);
			return;
		}
	}
	fprintf(stderr, "FAIL: freed a page that was never allocated\n");
	exit(1);
}

static uint64_t *
pa_to_va(uint64_t pa)
{
	unsigned int i;

	for (i = 0; i < npages_total; i++) {
		if (pages[i].va != NULL && pages[i].pa == pa)
			return pages[i].va;
	}
	return NULL;
}

/* Walk as the hardware does. Returns 0 if not mapped. */
static int
hw_walk(uint64_t root_pa, uint64_t gpa, uint64_t *leaf)
{
	uint64_t *t = pa_to_va(root_pa);
	int level;

	for (level = 3; level >= 0; level--) {
		uint64_t e;

		if (t == NULL) {
			fprintf(stderr, "FAIL: table at level %d is not a "
			    "live page\n", level);
			exit(1);
		}
		e = t[(gpa >> (12 + 9 * level)) & 511];
		if ((e & 1) == 0)
			return 0;
		if ((e & 4) == 0) {
			fprintf(stderr, "FAIL: user bit clear at level %d "
			    "for gpa %#llx\n", level, (unsigned long long)gpa);
			exit(1);
		}
		if (level == 0) {
			*leaf = e;
			return 1;
		}
		if ((e & 2) == 0) {
			fprintf(stderr, "FAIL: intermediate entry not "
			    "writable at level %d\n", level);
			exit(1);
		}
		t = pa_to_va(e & 0x000FFFFFFFFFF000ULL);
	}
	return 0;
}

static int checks;
#define CHECK(cond)							\
	do {								\
		checks++;						\
		if (!(cond)) {						\
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,	\
			    __LINE__, #cond);				\
			exit(1);					\
		}							\
	} while (0)

int
main(void)
{
	const struct npt_ops ops = { t_alloc, t_free, NULL };
	struct npt npt;
	uint64_t leaf, hpa, gpa, i;
	int prot;

	/* --- empty table -------------------------------------------------- */
	CHECK(npt_init(&npt, &ops) == 0);
	CHECK(npt.ntables == 1);
	CHECK(npages_live == 3);		/* meta + entries + shadow */
	CHECK(pa_to_va(npt_root_pa(&npt)) != NULL);
	CHECK(!npt_lookup(&npt, 0x1000, NULL, NULL));
	CHECK(hw_walk(npt_root_pa(&npt), 0x1000, &leaf) == 0);

	/* --- one page, each permission combination ------------------------ */
	CHECK(npt_map(&npt, 0x1000, 0xABCDE000, NPT_PROT_READ) == 0);
	CHECK(npt.ntables == 4 && npt.npages == 1);
	CHECK(hw_walk(npt_root_pa(&npt), 0x1000, &leaf) == 1);
	CHECK((leaf & 0x000FFFFFFFFFF000ULL) == 0xABCDE000);
	CHECK((leaf & 2) == 0);			/* not writable */
	CHECK((leaf >> 63) == 1);		/* not executable */
	CHECK(npt_lookup(&npt, 0x1234, &hpa, &prot));
	CHECK(hpa == 0xABCDE234 && prot == NPT_PROT_READ);

	/* Remapping the same address replaces it and does not double count. */
	CHECK(npt_map(&npt, 0x1000, 0x55555000,
	    NPT_PROT_READ | NPT_PROT_WRITE | NPT_PROT_EXEC) == 0);
	CHECK(npt.npages == 1 && npt.ntables == 4);
	CHECK(hw_walk(npt_root_pa(&npt), 0x1000, &leaf) == 1);
	CHECK((leaf & 0x000FFFFFFFFFF000ULL) == 0x55555000);
	CHECK((leaf & 2) != 0 && (leaf >> 63) == 0);

	/* --- rejected inputs ---------------------------------------------- */
	CHECK(npt_map(&npt, 0x1001, 0x1000, NPT_PROT_READ) != 0); /* unaligned gpa */
	CHECK(npt_map(&npt, 0x2000, 0x1001, NPT_PROT_READ) != 0); /* unaligned hpa */
	CHECK(npt_map(&npt, 1ULL << 48, 0x1000, NPT_PROT_READ) != 0);
	CHECK(npt_map(&npt, 0x2000, 0x1000, NPT_PROT_WRITE) != 0); /* no read */
	CHECK(npt.npages == 1);

	/* --- a range that crosses table boundaries at every level --------- */
	/* 1024 pages straddling a 512G boundary: touches a second PML4 slot. */
	gpa = (1ULL << 39) - 512 * PAGE;
	for (i = 0; i < 1024; i++)
		CHECK(npt_map(&npt, gpa + i * PAGE, 0x40000000 + i * PAGE,
		    NPT_PROT_READ | NPT_PROT_WRITE) == 0);
	CHECK(npt.npages == 1025);
	for (i = 0; i < 1024; i++) {
		CHECK(hw_walk(npt_root_pa(&npt), gpa + i * PAGE, &leaf) == 1);
		CHECK((leaf & 0x000FFFFFFFFFF000ULL) ==
		    0x40000000 + i * PAGE);
	}
	CHECK(hw_walk(npt_root_pa(&npt), gpa - PAGE, &leaf) == 0);
	CHECK(hw_walk(npt_root_pa(&npt), gpa + 1024 * PAGE, &leaf) == 0);

	/* --- partial unmap ------------------------------------------------ */
	/* Remove the middle 512 pages, spanning the boundary. */
	CHECK(npt_unmap(&npt, gpa + 256 * PAGE, 512 * PAGE) == 512);
	CHECK(npt.npages == 513);
	for (i = 0; i < 1024; i++) {
		int mapped = hw_walk(npt_root_pa(&npt), gpa + i * PAGE, &leaf);
		CHECK(mapped == (i < 256 || i >= 768));
	}
	/* Unmapping something that is not there is harmless. */
	CHECK(npt_unmap(&npt, gpa + 256 * PAGE, 512 * PAGE) == 0);
	CHECK(npt_unmap(&npt, 0x7000000000ULL, 1ULL << 30) == 0);
	CHECK(npt_unmap(&npt, 0, 0) == 0);

	/* --- unmap everything: tables are reclaimed ----------------------- */
	CHECK(npt_unmap(&npt, 0, 1ULL << 48) == 513);
	CHECK(npt.npages == 0);
	CHECK(npt.ntables == 1);		/* only the root is left */
	CHECK(npages_live == 3);
	CHECK(hw_walk(npt_root_pa(&npt), 0x1000, &leaf) == 0);

	/* The emptied table is still usable. */
	CHECK(npt_map(&npt, 0xFFFFF000, 0x1000, NPT_PROT_READ) == 0);
	CHECK(hw_walk(npt_root_pa(&npt), 0xFFFFF000, &leaf) == 1);

	/* A range ending exactly at the top of the address space. */
	CHECK(npt_map(&npt, (1ULL << 48) - PAGE, 0x2000, NPT_PROT_READ) == 0);
	CHECK(npt_unmap(&npt, (1ULL << 48) - PAGE, PAGE) == 1);
	/* An overflowing size is clamped, not wrapped. */
	CHECK(npt_unmap(&npt, 0xFFFFF000, ~0ULL) == 1);
	CHECK(npt.npages == 0);

	/* --- allocation failure mid-walk leaves a consistent table -------- */
	fail_after = 4;		/* enough for one new table, not for three */
	CHECK(npt_map(&npt, 0x8000000000ULL, 0x3000, NPT_PROT_READ) != 0);
	fail_after = -1;
	CHECK(npt.npages == 0);
	CHECK(!npt_lookup(&npt, 0x8000000000ULL, NULL, NULL));
	CHECK(hw_walk(npt_root_pa(&npt), 0x8000000000ULL, &leaf) == 0);
	/* The partly built path is reused, and reclaimed like any other. */
	CHECK(npt_map(&npt, 0x8000000000ULL, 0x3000, NPT_PROT_READ) == 0);
	CHECK(hw_walk(npt_root_pa(&npt), 0x8000000000ULL, &leaf) == 1);
	CHECK(npt_unmap(&npt, 0, 1ULL << 48) == 1);
	CHECK(npt.ntables == 1);

	/* --- destroy frees every page ------------------------------------- */
	for (i = 0; i < 2000; i++)
		CHECK(npt_map(&npt, i * 0x40000000ULL / 16, 0x1000,
		    NPT_PROT_READ) == 0);
	npt_destroy(&npt);
	CHECK(npages_live == 0);

	printf("npt_test: %d checks passed, %u pages allocated in total\n",
	    checks, npages_total);
	return 0;
}
