/*
 * nvmm_port_x86.h — the x86 primitives NVMM's engine expects from the host
 * kernel, written out directly so they do not depend on any one kernel.
 *
 * Included from nvmm_x86_internal.h, after its CR0_ and CR4_ definitions.
 */

#ifndef _NVMM_PORT_X86_H_
#define _NVMM_PORT_X86_H_

/* RFLAGS bits, under their BSD names. */
#ifndef PSL_I
#define PSL_I		0x00000200
#endif
#ifndef PSL_RF
#define PSL_RF		0x00010000
#endif

/* CPUID. */
static inline void
x86_get_cpuid2(uint32_t leaf, uint32_t subleaf, cpuid_desc_t *d)
{
	__asm volatile (
		"cpuid"
		: "=a" (d->eax), "=b" (d->ebx), "=c" (d->ecx), "=d" (d->edx)
		: "a" (leaf), "c" (subleaf)
	);
}

/* MSRs and the TSC. The names are the BSD ones the engine uses. */
static inline uint64_t
port_rdmsr(uint32_t msr)
{
	uint32_t lo, hi;

	__asm volatile ("rdmsr" : "=a" (lo), "=d" (hi) : "c" (msr));
	return lo | ((uint64_t)hi << 32);
}

static inline void
port_wrmsr(uint32_t msr, uint64_t val)
{
	__asm volatile ("wrmsr"
	    :
	    : "c" (msr), "a" ((uint32_t)val), "d" ((uint32_t)(val >> 32))
	    : "memory");
}

static inline uint64_t
port_rdtsc(void)
{
	uint32_t lo, hi;

	__asm volatile ("rdtsc" : "=a" (lo), "=d" (hi));
	return lo | ((uint64_t)hi << 32);
}

#undef rdmsr
#undef wrmsr
#undef rdtsc
#define rdmsr(msr)		port_rdmsr(msr)
#define wrmsr(msr, val)		port_wrmsr((msr), (val))
#define rdtsc()			port_rdtsc()

/* Control registers. */
#define PORT_CR_ACCESSORS(n)						\
static inline uint64_t							\
x86_get_cr##n(void)							\
{									\
	uint64_t v;							\
	__asm volatile ("movq %%cr" #n ",%0" : "=r" (v));		\
	return v;							\
}									\
static inline void							\
x86_set_cr##n(uint64_t v)						\
{									\
	__asm volatile ("movq %0,%%cr" #n : : "r" (v) : "memory");	\
}
PORT_CR_ACCESSORS(0)
PORT_CR_ACCESSORS(2)
PORT_CR_ACCESSORS(3)
PORT_CR_ACCESSORS(4)

/* Debug registers. */
#define PORT_DR_ACCESSORS(n)						\
static inline uint64_t							\
x86_get_dr##n(void)							\
{									\
	uint64_t v;							\
	__asm volatile ("movq %%dr" #n ",%0" : "=r" (v));		\
	return v;							\
}									\
static inline void							\
x86_set_dr##n(uint64_t v)						\
{									\
	__asm volatile ("movq %0,%%dr" #n : : "r" (v));			\
}
PORT_DR_ACCESSORS(0)
PORT_DR_ACCESSORS(1)
PORT_DR_ACCESSORS(2)
PORT_DR_ACCESSORS(3)
PORT_DR_ACCESSORS(6)
PORT_DR_ACCESSORS(7)

/*
 * Per-CPU scratch space for the host state the engine parks while a guest
 * owns the debug and floating-point registers. Only touched with preemption
 * disabled, so indexing by the current CPU is safe.
 */
#define PORT_XSAVE_AREA_SIZE	4096

struct port_pcpu {
	uint64_t dr[4];
	uint64_t dr6;
	uint64_t dr7;
	uint64_t cr0;
	uint8_t *hfpu;		/* PORT_XSAVE_AREA_SIZE bytes, page-aligned */
	uint64_t hfpu_pa;
};
extern struct port_pcpu port_pcpu[OS_MAXCPUS];

/* Host XCR0, or 0 when the host does not use XSAVE. */
extern uint64_t port_xsave_features;
extern uint32_t port_fpu_mxcsr_mask;
#define x86_xsave_features	port_xsave_features
#define x86_fpu_mxcsr_mask	port_fpu_mxcsr_mask

int	port_x86_init(void);
void	port_x86_fini(void);

static inline void
x86_curthread_save_dbregs(void)
{
	struct port_pcpu *pc = &port_pcpu[port_curcpu()];

	pc->dr[0] = x86_get_dr0();
	pc->dr[1] = x86_get_dr1();
	pc->dr[2] = x86_get_dr2();
	pc->dr[3] = x86_get_dr3();
	pc->dr6 = x86_get_dr6();
	pc->dr7 = x86_get_dr7();
}

static inline void
x86_curthread_restore_dbregs(void)
{
	struct port_pcpu *pc = &port_pcpu[port_curcpu()];

	/* Breakpoints stay off until their addresses are back in place. */
	x86_set_dr7(0);
	x86_set_dr0(pc->dr[0]);
	x86_set_dr1(pc->dr[1]);
	x86_set_dr2(pc->dr[2]);
	x86_set_dr3(pc->dr[3]);
	x86_set_dr6(pc->dr6);
	x86_set_dr7(pc->dr7);
}

/* FPU. A mask of 0 means "no XSAVE", and FXSAVE is used instead. */
static inline void
x86_save_fpu(void *area, uint64_t mask)
{
	if (mask != 0) {
		__asm volatile ("xsave64 (%0)"
		    :
		    : "r" (area), "a" ((uint32_t)mask),
		      "d" ((uint32_t)(mask >> 32))
		    : "memory");
	} else {
		__asm volatile ("fxsave64 (%0)" : : "r" (area) : "memory");
	}
}

static inline void
x86_restore_fpu(const void *area, uint64_t mask)
{
	if (mask != 0) {
		__asm volatile ("xrstor64 (%0)"
		    :
		    : "r" (area), "a" ((uint32_t)mask),
		      "d" ((uint32_t)(mask >> 32))
		    : "memory");
	} else {
		__asm volatile ("fxrstor64 (%0)" : : "r" (area) : "memory");
	}
}

/*
 * Park whatever is in the FPU registers and take them over.
 *
 * The host may be switching FPU state lazily, in which case CR0.TS is set and
 * the registers hold some other thread's values. Either way the right thing
 * is the same: clear TS so the registers can be touched, save them exactly as
 * found, and put both back afterwards. Preemption is off for the whole time
 * the guest owns the registers, so no host thread can observe the gap.
 */
static inline void
x86_curthread_save_fpu(void)
{
	struct port_pcpu *pc = &port_pcpu[port_curcpu()];

	pc->cr0 = x86_get_cr0();
	__asm volatile ("clts" ::: "memory");
	x86_save_fpu(pc->hfpu, port_xsave_features);
}

static inline void
x86_curthread_restore_fpu(void)
{
	struct port_pcpu *pc = &port_pcpu[port_curcpu()];

	x86_restore_fpu(pc->hfpu, port_xsave_features);
	if (pc->cr0 & CR0_TS)
		x86_set_cr0(pc->cr0);
}

#endif /* _NVMM_PORT_X86_H_ */
