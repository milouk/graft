/*
 * npt.h — nested page tables for AMD-V, built by hand.
 *
 * AMD nested paging uses the ordinary x86-64 four-level page table format, so
 * this is a small page-table builder. The one complication is that the host
 * gives no way to turn a physical address back into a pointer, so every table
 * page is paired with a "shadow" page holding the virtual addresses of its
 * children. The hardware walks the real tables; this code walks the shadows.
 *
 * It is deliberately free of any OS dependency: the caller supplies the page
 * allocator. That is what lets the same file run in the macOS driver, in the
 * bare-metal test kernel, and in a userspace unit test.
 */

#ifndef _NVMM_NPT_H_
#define _NVMM_NPT_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define NPT_PROT_READ	0x1
#define NPT_PROT_WRITE	0x2
#define NPT_PROT_EXEC	0x4

struct npt_ops {
	/* One zeroed page; both its virtual and physical address. */
	int (*page_alloc)(void *ctx, void **va, uint64_t *pa);
	void (*page_free)(void *ctx, void *va, uint64_t pa);
	void *ctx;
};

struct npt_table {
	uint64_t *entries;		/* the page the CPU walks */
	uint64_t pa;			/* its physical address */
	struct npt_table **children;	/* shadow: 512 child pointers */
	uint64_t children_pa;		/* only kept so it can be freed */
	unsigned int used;		/* live entries in this table */
};

struct npt {
	struct npt_ops ops;
	struct npt_table *root;
	uint64_t npages;		/* 4K pages currently mapped */
	uint64_t ntables;		/* table pages currently allocated */
};

int	npt_init(struct npt *npt, const struct npt_ops *ops);
void	npt_destroy(struct npt *npt);

/* Physical address to load into the VMCB's nCR3 field. */
uint64_t npt_root_pa(const struct npt *npt);

/* Map one 4K page. Replaces an existing mapping at that address. */
int	npt_map(struct npt *npt, uint64_t gpa, uint64_t hpa, int prot);

/* Unmap [gpa, gpa+size). Empty tables are freed. Returns pages removed. */
uint64_t npt_unmap(struct npt *npt, uint64_t gpa, uint64_t size);

/* Look a guest address up. Returns false if it is not mapped. */
bool	npt_lookup(const struct npt *npt, uint64_t gpa, uint64_t *hpa,
	    int *prot);

#endif /* _NVMM_NPT_H_ */
