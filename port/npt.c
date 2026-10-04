/*
 * npt.c — nested page tables for AMD-V. See npt.h.
 */

#include "npt.h"

#define NPT_LEVELS	4
#define NPT_ENTRIES	512
#define NPT_PAGE_SIZE	4096ULL

/* x86-64 page table entry bits. */
#define PTE_P		(1ULL << 0)
#define PTE_W		(1ULL << 1)
/*
 * The CPU treats every nested-page-table access as a user-mode access, so the
 * user bit must be set at every level or the guest faults on its first fetch.
 */
#define PTE_U		(1ULL << 2)
/* In a page-directory entry: this maps a 2M page instead of a table. */
#define PTE_PS		(1ULL << 7)
#define PTE_NX		(1ULL << 63)
#define PTE_FRAME	0x000FFFFFFFFFF000ULL
#define PTE_PROT	(PTE_W | PTE_NX)

/* Guest-physical addresses are limited to 48 bits by the 4-level format. */
#define NPT_GPA_MAX	(1ULL << 48)

static inline unsigned int
npt_index(uint64_t gpa, int level)
{
	/* level 3 = PML4 ... level 0 = PT */
	return (gpa >> (12 + 9 * level)) & (NPT_ENTRIES - 1);
}

static struct npt_table *
npt_table_alloc(struct npt *npt)
{
	struct npt_table *t;
	void *meta_va, *ent_va, *child_va;
	uint64_t meta_pa, ent_pa, child_pa;

	/*
	 * The bookkeeping struct gets a page of its own. Wasteful, but it
	 * keeps the allocator interface down to "give me a page".
	 */
	if (npt->ops.page_alloc(npt->ops.ctx, &meta_va, &meta_pa) != 0)
		return NULL;
	if (npt->ops.page_alloc(npt->ops.ctx, &ent_va, &ent_pa) != 0) {
		npt->ops.page_free(npt->ops.ctx, meta_va, meta_pa);
		return NULL;
	}
	if (npt->ops.page_alloc(npt->ops.ctx, &child_va, &child_pa) != 0) {
		npt->ops.page_free(npt->ops.ctx, ent_va, ent_pa);
		npt->ops.page_free(npt->ops.ctx, meta_va, meta_pa);
		return NULL;
	}

	t = meta_va;
	t->entries = ent_va;
	t->pa = ent_pa;
	t->children = child_va;
	t->children_pa = child_pa;
	t->used = 0;
	/* Stash the meta page's own PA past the struct so it can be freed. */
	*(uint64_t *)((char *)meta_va + NPT_PAGE_SIZE - sizeof(uint64_t)) =
	    meta_pa;

	npt->ntables++;
	return t;
}

static void
npt_table_free(struct npt *npt, struct npt_table *t)
{
	uint64_t meta_pa;

	meta_pa = *(uint64_t *)((char *)t + NPT_PAGE_SIZE - sizeof(uint64_t));
	npt->ops.page_free(npt->ops.ctx, t->entries, t->pa);
	npt->ops.page_free(npt->ops.ctx, t->children, t->children_pa);
	npt->ops.page_free(npt->ops.ctx, t, meta_pa);
	npt->ntables--;
}

static void
npt_table_free_tree(struct npt *npt, struct npt_table *t, int level)
{
	unsigned int i;

	if (level > 0) {
		for (i = 0; i < NPT_ENTRIES; i++) {
			if (t->children[i] != NULL)
				npt_table_free_tree(npt, t->children[i],
				    level - 1);
		}
	}
	npt_table_free(npt, t);
}

int
npt_init(struct npt *npt, const struct npt_ops *ops)
{
	npt->ops = *ops;
	npt->npages = 0;
	npt->ntables = 0;
	npt->root = npt_table_alloc(npt);
	if (npt->root == NULL)
		return -1;
	return 0;
}

void
npt_destroy(struct npt *npt)
{
	if (npt->root != NULL) {
		npt_table_free_tree(npt, npt->root, NPT_LEVELS - 1);
		npt->root = NULL;
	}
	npt->npages = 0;
}

uint64_t
npt_root_pa(const struct npt *npt)
{
	return npt->root->pa;
}

static uint64_t
npt_leaf_bits(int prot)
{
	uint64_t pte = PTE_P | PTE_U;

	if (prot & NPT_PROT_WRITE)
		pte |= PTE_W;
	if ((prot & NPT_PROT_EXEC) == 0)
		pte |= PTE_NX;
	return pte;
}

/*
 * Replace the 2M page at 'pd->entries[idx]' with a table of 512 4K pages
 * that map the same memory with the same rights. The page count does not
 * change. Fails only if a table cannot be allocated.
 */
static int
npt_split(struct npt *npt, struct npt_table *pd, unsigned int idx)
{
	const uint64_t large = pd->entries[idx];
	const uint64_t base = large & PTE_FRAME & ~(NPT_LARGE_SIZE - 1);
	const uint64_t bits = (large & PTE_PROT) | PTE_P | PTE_U;
	struct npt_table *pt;
	unsigned int i;

	pt = npt_table_alloc(npt);
	if (pt == NULL)
		return -1;
	for (i = 0; i < NPT_ENTRIES; i++)
		pt->entries[i] = (base + (uint64_t)i * NPT_PAGE_SIZE) | bits;
	pt->used = NPT_ENTRIES;

	pd->children[idx] = pt;
	pd->entries[idx] = pt->pa | PTE_P | PTE_W | PTE_U;
	return 0;
}

/*
 * Walk down to the table at 'target' level (0 = page table, 1 = page
 * directory) that covers 'gpa', creating tables on the way.
 */
static struct npt_table *
npt_walk_create(struct npt *npt, uint64_t gpa, int target)
{
	struct npt_table *t = npt->root, *child;
	unsigned int idx;
	int level;

	for (level = NPT_LEVELS - 1; level > target; level--) {
		idx = npt_index(gpa, level);
		if (level == 1 && (t->entries[idx] & PTE_PS) != 0) {
			if (npt_split(npt, t, idx) != 0)
				return NULL;
		}
		child = t->children[idx];
		if (child == NULL) {
			child = npt_table_alloc(npt);
			if (child == NULL)
				return NULL;
			t->children[idx] = child;
			/*
			 * Intermediate entries are fully permissive; the leaf
			 * decides what the guest may actually do.
			 */
			t->entries[idx] = child->pa | PTE_P | PTE_W | PTE_U;
			t->used++;
		}
		t = child;
	}
	return t;
}

int
npt_map(struct npt *npt, uint64_t gpa, uint64_t hpa, int prot)
{
	struct npt_table *t;
	unsigned int idx;
	uint64_t pte;

	if (gpa >= NPT_GPA_MAX || (gpa & (NPT_PAGE_SIZE - 1)) != 0 ||
	    (hpa & ~PTE_FRAME) != 0)
		return -1;
	if ((prot & NPT_PROT_READ) == 0)
		return -1;

	t = npt_walk_create(npt, gpa, 0);
	if (t == NULL)
		return -1;

	idx = npt_index(gpa, 0);
	pte = hpa | npt_leaf_bits(prot);

	if ((t->entries[idx] & PTE_P) == 0) {
		t->used++;
		npt->npages++;
	}
	t->entries[idx] = pte;
	return 0;
}

int
npt_map_large(struct npt *npt, uint64_t gpa, uint64_t hpa, int prot)
{
	struct npt_table *pd, *old;
	unsigned int idx;

	if (gpa >= NPT_GPA_MAX || (gpa & (NPT_LARGE_SIZE - 1)) != 0 ||
	    (hpa & ~PTE_FRAME) != 0 || (hpa & (NPT_LARGE_SIZE - 1)) != 0)
		return -1;
	if ((prot & NPT_PROT_READ) == 0)
		return -1;

	pd = npt_walk_create(npt, gpa, 1);
	if (pd == NULL)
		return -1;

	idx = npt_index(gpa, 1);
	old = pd->children[idx];
	if (old != NULL) {
		/* 4K pages were mapped here; the 2M page replaces them all. */
		npt->npages -= old->used;
		npt_table_free(npt, old);
		pd->children[idx] = NULL;
	} else if ((pd->entries[idx] & PTE_P) != 0) {
		npt->npages -= NPT_ENTRIES;
	} else {
		pd->used++;
	}

	pd->entries[idx] = hpa | npt_leaf_bits(prot) | PTE_PS;
	npt->npages += NPT_ENTRIES;
	return 0;
}

/*
 * Unmap within one table. Returns true if the table ended up empty, in which
 * case the caller frees it and clears its own entry.
 */
static bool
npt_unmap_level(struct npt *npt, struct npt_table *t, int level,
    uint64_t base, uint64_t start, uint64_t end, uint64_t *removed)
{
	const uint64_t span = 1ULL << (12 + 9 * level);
	unsigned int i;

	for (i = 0; i < NPT_ENTRIES; i++) {
		const uint64_t lo = base + (uint64_t)i * span;
		const uint64_t hi = lo + span;

		if (hi <= start || lo >= end)
			continue;
		if ((t->entries[i] & PTE_P) == 0)
			continue;

		if (level == 0) {
			t->entries[i] = 0;
			t->used--;
			npt->npages--;
			(*removed)++;
			continue;
		}

		if (level == 1 && (t->entries[i] & PTE_PS) != 0) {
			/*
			 * A 2M page. If only part of it goes, split it and
			 * carry on into the new table. If there is no memory
			 * to split it with, the whole page goes: the guest
			 * then faults on the rest and stops, which is better
			 * than leaving it memory it was told to give up.
			 */
			if ((lo < start || hi > end) &&
			    npt_split(npt, t, i) == 0) {
				/* fall through to the recursion below */
			} else {
				t->entries[i] = 0;
				t->used--;
				npt->npages -= NPT_ENTRIES;
				(*removed) += NPT_ENTRIES;
				continue;
			}
		}

		if (npt_unmap_level(npt, t->children[i], level - 1, lo,
		    start, end, removed)) {
			npt_table_free(npt, t->children[i]);
			t->children[i] = NULL;
			t->entries[i] = 0;
			t->used--;
		}
	}

	return t->used == 0;
}

uint64_t
npt_unmap(struct npt *npt, uint64_t gpa, uint64_t size)
{
	uint64_t removed = 0;
	uint64_t end;

	if (size == 0 || gpa >= NPT_GPA_MAX)
		return 0;
	end = gpa + size;
	if (end > NPT_GPA_MAX || end < gpa)
		end = NPT_GPA_MAX;

	/* The root is never freed here, even when it empties. */
	(void)npt_unmap_level(npt, npt->root, NPT_LEVELS - 1, 0, gpa, end,
	    &removed);
	return removed;
}

bool
npt_lookup(const struct npt *npt, uint64_t gpa, uint64_t *hpa, int *prot)
{
	const struct npt_table *t = npt->root;
	unsigned int idx;
	uint64_t pte;
	int level;

	if (gpa >= NPT_GPA_MAX)
		return false;

	uint64_t offmask = NPT_PAGE_SIZE - 1;

	for (level = NPT_LEVELS - 1; level > 0; level--) {
		idx = npt_index(gpa, level);
		if (level == 1 && (t->entries[idx] & PTE_PS) != 0)
			break;
		if (t->children[idx] == NULL)
			return false;
		t = t->children[idx];
	}

	if (level == 1) {
		pte = t->entries[idx];
		offmask = NPT_LARGE_SIZE - 1;
	} else {
		pte = t->entries[npt_index(gpa, 0)];
	}
	if ((pte & PTE_P) == 0)
		return false;

	if (hpa != NULL)
		*hpa = (pte & PTE_FRAME & ~offmask) | (gpa & offmask);
	if (prot != NULL) {
		*prot = NPT_PROT_READ;
		if (pte & PTE_W)
			*prot |= NPT_PROT_WRITE;
		if ((pte & PTE_NX) == 0)
			*prot |= NPT_PROT_EXEC;
	}
	return true;
}
