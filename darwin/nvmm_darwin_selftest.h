/* nvmm_darwin_selftest.h — hooks of the glue self-test build. */
#ifndef _NVMM_DARWIN_SELFTEST_H_
#define _NVMM_DARWIN_SELFTEST_H_

int	nvmm_selftest_attach(void);
int	nvmm_selftest_detach(void);

/* Sleep and wake notifications seen since load. */
extern volatile unsigned int nvmm_selftest_sleeps;
extern volatile unsigned int nvmm_selftest_wakes;

#endif /* _NVMM_DARWIN_SELFTEST_H_ */
