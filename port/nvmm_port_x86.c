/*
 * nvmm_port_x86.c — per-CPU host state storage for nvmm_port_x86.h.
 */

#include "nvmm.h"
#include "nvmm_internal.h"
#include "x86/nvmm_x86_internal.h"

struct port_pcpu port_pcpu[OS_MAXCPUS];
uint64_t port_xsave_features;
uint32_t port_fpu_mxcsr_mask;

/* Layout of the legacy FXSAVE image, as far as this file cares. */
struct port_fxsave {
	uint8_t pad[24];
	uint32_t mxcsr;
	uint32_t mxcsr_mask;
	uint8_t rest[512 - 32];
};

int
port_x86_init(void)
{
	cpuid_desc_t d;
	unsigned int i, ncpus = port_ncpus();
	struct port_fxsave *fx;
	void *va;
	uint64_t pa;

	if (ncpus > OS_MAXCPUS)
		return ENOTSUP;

	/* XSAVE is in use on the host iff CR4.OSXSAVE is set. */
	x86_get_cpuid(0x00000001, &d);
	if ((d.ecx & (1U << 27)) != 0)
		port_xsave_features = x86_get_xcr(0);
	else
		port_xsave_features = 0;

	/*
	 * The save area must hold every state component the host has enabled.
	 * CPUID.0xD.0:EBX is that size for the current XCR0.
	 */
	if (port_xsave_features != 0) {
		x86_get_cpuid2(0x0000000D, 0, &d);
		if (d.ebx > PORT_XSAVE_AREA_SIZE)
			return ENOTSUP;
	}

	for (i = 0; i < ncpus; i++) {
		if (port_pages_alloc(1, &va, &pa) != 0) {
			port_x86_fini();
			return ENOMEM;
		}
		port_pcpu[i].hfpu = va;
		port_pcpu[i].hfpu_pa = pa;
	}

	/* The MXCSR mask comes from an FXSAVE image; 0 means the default. */
	fx = (struct port_fxsave *)port_pcpu[0].hfpu;
	{
		const uint64_t cr0 = x86_get_cr0();

		port_preempt_disable();
		__asm volatile ("clts" ::: "memory");
		__asm volatile ("fxsave64 (%0)" : : "r" (fx) : "memory");
		if (cr0 & CR0_TS)
			x86_set_cr0(cr0);
		port_preempt_enable();
	}
	port_fpu_mxcsr_mask = fx->mxcsr_mask ? fx->mxcsr_mask : 0x0000FFBF;
	memset(fx, 0, PORT_XSAVE_AREA_SIZE);

	return 0;
}

void
port_x86_fini(void)
{
	unsigned int i;

	for (i = 0; i < OS_MAXCPUS; i++) {
		if (port_pcpu[i].hfpu != NULL) {
			port_pages_free(port_pcpu[i].hfpu,
			    port_pcpu[i].hfpu_pa, 1);
			port_pcpu[i].hfpu = NULL;
		}
	}
}
