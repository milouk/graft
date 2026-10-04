/*
 * nvmm_fake_engine.h — the commands the stand-in engine understands.
 * Shared with test/darwin/nvmm-fake-test.c.
 *
 * A "guest" for the stand-in engine is just a value in RAX when the vCPU is
 * run; see nvmm_fake_engine.c.
 */
#ifndef _NVMM_FAKE_ENGINE_H_
#define _NVMM_FAKE_ENGINE_H_

/* 0 (the value RAX has after reset) halts without advancing. */

/* Exit with an I/O port write to the port in RBX. */
#define NVMM_FAKE_CMD_IO	1
/*
 * Touch the guest-physical address in RBX. If the machine has memory mapped
 * there, halt with RCX = 1; if not, exit with a memory fault at that address.
 */
#define NVMM_FAKE_CMD_MEM	2
/* Halt, with the host CPU number in RDX. */
#define NVMM_FAKE_CMD_HLT	3

#endif /* _NVMM_FAKE_ENGINE_H_ */
