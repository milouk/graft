/* bare.h — interface between the test kernel's platform code and its tests. */
#ifndef _BARE_H_
#define _BARE_H_

struct bare_stats {
	long malloc_live;	/* port_malloc() allocations not yet freed */
	long malloc_bytes;
	long pages_live;	/* pages from port_pages_alloc() not yet freed */
	long membufs_live;
	long maps_live;		/* port_membuf_map() not yet unmapped */
	long locks_live;	/* initialised and not destroyed */
	unsigned long ipis;	/* port_ipi_broadcast() calls */
};
extern struct bare_stats bare_stats;

/* What port_return_needed() reports; tests flip it. */
extern bool bare_return_needed;

void	bare_init(uint64_t ram_bytes);
void	bare_exit(int code) __attribute__((__noreturn__));
/* The memory behind a "user" address handed to NVMM_IOC_HVA_MAP. */
void *	bare_hva_ptr(uintptr_t hva);
void	trap_handler(uint64_t vector, uint64_t error, uint64_t rip,
	    uint64_t cr2);
void	kmain(void);

#endif /* _BARE_H_ */
