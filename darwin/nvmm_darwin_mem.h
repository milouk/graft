/* nvmm_darwin_mem.h — the C++ half of the macOS layer, as seen from C. */
#ifndef _NVMM_DARWIN_MEM_H_
#define _NVMM_DARWIN_MEM_H_

#ifdef __cplusplus
extern "C" {
#endif

void	nvmm_darwin_mem_init(void);
void	nvmm_darwin_mem_fini(void);

/* How the most recent buffer of 2M or more was built. Diagnostic only. */
struct nvmm_darwin_mem_stats {
	unsigned int last_large;	/* 2M runs obtained */
	unsigned int last_wanted;	/* 2M runs that would have fitted */
	unsigned int last_usec;		/* time spent asking for them */
};
extern struct nvmm_darwin_mem_stats nvmm_darwin_mem_stats;

void	nvmm_darwin_power_register(void);
void	nvmm_darwin_power_unregister(void);

/* Implemented in nvmm_darwin.c, called on the power-management thread. */
void	nvmm_darwin_will_sleep(void);
void	nvmm_darwin_did_wake(void);

#ifdef __cplusplus
}
#endif

#endif /* _NVMM_DARWIN_MEM_H_ */
