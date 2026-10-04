/*
 * nvmm_darwin.h — what the macOS kernel provides, as seen by port/nvmm_port.h.
 */
#ifndef _NVMM_DARWIN_H_
#define _NVMM_DARWIN_H_

#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/mman.h>
#include <libkern/libkern.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

/*
 * The kernel's own panic() is a macro. NVMM's panics go through port_panic()
 * so that they are tagged; see nvmm_port.h.
 */
#undef panic

/* <i386/proc_reg.h> names that collide with the engine's own definitions. */
#undef rdmsr
#undef wrmsr
#undef rdtsc

#endif /* _NVMM_DARWIN_H_ */
