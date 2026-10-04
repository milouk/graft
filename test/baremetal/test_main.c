/*
 * test_main.c — drives the NVMM engine the way an emulator would, through the
 * same ioctl entry point, inside a machine that has no operating system.
 *
 * What this proves: the SVM engine, the VMRUN assembly, the nested page table
 * builder and the guest-memory layer work together on an AMD CPU, and give
 * back every resource they take.
 *
 * What it cannot prove: anything about the macOS glue (darwin/), which is not
 * in this binary, and anything that depends on a real TLB. The emulator this
 * runs under flushes its translation cache on every world switch, so a missing
 * TLB flush in the engine would go unnoticed here.
 */

#include "nvmm.h"
#include "nvmm_internal.h"
#include "nvmm_ioctl.h"
#include "x86/nvmm_x86_internal.h"
#include "bare.h"

static int nchecks;

#define CHECK(cond)							\
	do {								\
		nchecks++;						\
		if (!(cond)) {						\
			port_printf("FAIL %s:%d: %s\n", __FILE__,	\
			    __LINE__, #cond);				\
			bare_exit(1);					\
		}							\
	} while (0)

#define CHECK_EQ(a, b)							\
	do {								\
		const uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b);	\
		nchecks++;						\
		if (_a != _b) {						\
			port_printf("FAIL %s:%d: %s = %#lx, expected "	\
			    "%s = %#lx\n", __FILE__, __LINE__, #a, _a,	\
			    #b, _b);					\
			bare_exit(1);					\
		}							\
	} while (0)

/* A made-up "process address" for guest RAM; see bare_hva_ptr(). */
#define HVA_BASE	0x0000700000000000ULL
#define HVA_SIZE	0x100000ULL	/* 1 MiB of backing memory */
#define RAM_SIZE	0x80000ULL	/* of which 512 KiB is guest RAM at 0 */
#define MMIO_GPA	0x90000ULL	/* left unmapped on purpose */

static struct nvmm_owner owner = { .pid = 1 };
static nvmm_machid_t machid;
static struct nvmm_comm_page *comm;
static uint8_t *ram;

static int
ioc(unsigned long cmd, void *data)
{
	return nvmm_ioctl(&owner, cmd, data);
}

static void
get_state(uint64_t flags)
{
	struct nvmm_ioc_vcpu_getstate args = { .machid = machid, .cpuid = 0 };

	comm->state_wanted = flags & ~comm->state_cached;
	if (comm->state_wanted == 0)
		return;
	CHECK_EQ(ioc(NVMM_IOC_VCPU_GETSTATE, &args), 0);
}

static void
set_state(uint64_t flags)
{
	comm->state_commit |= flags;
	comm->state_cached |= flags;
}

static void
set_rip(uint64_t rip)
{
	get_state(NVMM_X64_STATE_GPRS);
	comm->state.gprs[NVMM_X64_GPR_RIP] = rip;
	set_state(NVMM_X64_STATE_GPRS);
}

static uint64_t
get_gpr(int reg)
{
	get_state(NVMM_X64_STATE_GPRS);
	return comm->state.gprs[reg];
}

/*
 * Host state that a world switch disturbs and the engine must put back.
 * VMRUN restores only part of it by itself; the rest is the job of the
 * VMSAVE/VMLOAD pair in svm_vmrun() and of the engine's MSR bookkeeping.
 */
struct host_state {
	uint64_t fsbase, gsbase, kgsbase;
	uint64_t star, lstar, cstar, sfmask;
	uint64_t efer, cr0, cr3, cr4, xcr0;
	uint64_t dr0, dr1, dr2, dr3, dr7;
	uint16_t tr, ldt, ds, es, fs, gs;
};

static void
host_state_read(struct host_state *h)
{
	h->fsbase = rdmsr(MSR_FSBASE);
	h->gsbase = rdmsr(MSR_GSBASE);
	h->kgsbase = rdmsr(MSR_KERNELGSBASE);
	h->star = rdmsr(MSR_STAR);
	h->lstar = rdmsr(MSR_LSTAR);
	h->cstar = rdmsr(MSR_CSTAR);
	h->sfmask = rdmsr(MSR_SFMASK);
	h->efer = rdmsr(MSR_EFER);
	h->cr0 = x86_get_cr0();
	h->cr3 = x86_get_cr3();
	h->cr4 = x86_get_cr4();
	h->xcr0 = (x86_xsave_features != 0) ? x86_get_xcr(0) : 0;
	h->dr0 = x86_get_dr0();
	h->dr1 = x86_get_dr1();
	h->dr2 = x86_get_dr2();
	h->dr3 = x86_get_dr3();
	h->dr7 = x86_get_dr7();
	__asm volatile ("str %0" : "=r" (h->tr));
	__asm volatile ("sldt %0" : "=r" (h->ldt));
	__asm volatile ("movw %%ds,%0" : "=r" (h->ds));
	__asm volatile ("movw %%es,%0" : "=r" (h->es));
	__asm volatile ("movw %%fs,%0" : "=r" (h->fs));
	__asm volatile ("movw %%gs,%0" : "=r" (h->gs));
}

/* Distinctive values, so "restored" cannot be confused with "still zero". */
static void
host_state_seed(void)
{
	wrmsr(MSR_FSBASE, 0x0000111111111000ULL);
	wrmsr(MSR_GSBASE, 0x0000222222222000ULL);
	wrmsr(MSR_KERNELGSBASE, 0x0000333333333000ULL);
	wrmsr(MSR_STAR, 0x0023001000000000ULL);
	wrmsr(MSR_LSTAR, 0xFFFFFFFF80444000ULL);
	wrmsr(MSR_CSTAR, 0xFFFFFFFF80555000ULL);
	wrmsr(MSR_SFMASK, 0x0000000000047700ULL);

	/*
	 * Four armed execution breakpoints on addresses nothing ever runs.
	 * A host debugger's breakpoints must survive a guest run; with DR7
	 * left at its reset value there would be nothing to lose.
	 */
	x86_set_dr0(0x00007F0000001000ULL);
	x86_set_dr1(0x00007F0000002000ULL);
	x86_set_dr2(0x00007F0000003000ULL);
	x86_set_dr3(0x00007F0000004000ULL);
	x86_set_dr7(0x00000455ULL);
}

static struct nvmm_vcpu_exit *
run(void)
{
	static struct nvmm_ioc_vcpu_run args;
	struct host_state before, after;

	memset(&args, 0, sizeof(args));
	args.machid = machid;
	args.cpuid = 0;

	host_state_read(&before);
	CHECK_EQ(ioc(NVMM_IOC_VCPU_RUN, &args), 0);
	host_state_read(&after);

	CHECK_EQ(after.fsbase, before.fsbase);
	CHECK_EQ(after.gsbase, before.gsbase);
	CHECK_EQ(after.kgsbase, before.kgsbase);
	CHECK_EQ(after.star, before.star);
	CHECK_EQ(after.lstar, before.lstar);
	CHECK_EQ(after.cstar, before.cstar);
	CHECK_EQ(after.sfmask, before.sfmask);
	CHECK_EQ(after.efer, before.efer);
	CHECK_EQ(after.cr0, before.cr0);
	CHECK_EQ(after.cr3, before.cr3);
	CHECK_EQ(after.cr4, before.cr4);
	CHECK_EQ(after.xcr0, before.xcr0);
	CHECK_EQ(after.dr0, before.dr0);
	CHECK_EQ(after.dr1, before.dr1);
	CHECK_EQ(after.dr2, before.dr2);
	CHECK_EQ(after.dr3, before.dr3);
	CHECK_EQ(after.dr7, before.dr7);
	CHECK_EQ(after.tr, before.tr);
	CHECK_EQ(after.ldt, before.ldt);
	CHECK_EQ(after.ds, before.ds);
	CHECK_EQ(after.es, before.es);
	CHECK_EQ(after.fs, before.fs);
	CHECK_EQ(after.gs, before.gs);
	CHECK(!port_preempt_disabled());

	return &args.exit;
}

/* Run, and say what happened if it is not what the test wanted. */
static struct nvmm_vcpu_exit *
run_expect(uint64_t reason)
{
	struct nvmm_vcpu_exit *exit = run();

	if (exit->reason != reason) {
		port_printf("\nunexpected exit: reason %#lx (wanted %#lx)\n",
		    exit->reason, reason);
		if (exit->reason == NVMM_VCPU_EXIT_MEMORY)
			port_printf("  memory: gpa %#lx prot %d\n",
			    exit->u.mem.gpa, exit->u.mem.prot);
		if (exit->reason == NVMM_VCPU_EXIT_INVALID)
			port_printf("  hardware exit code %#lx\n",
			    exit->u.inv.hwcode);
		get_state(NVMM_X64_STATE_GPRS | NVMM_X64_STATE_SEGS |
		    NVMM_X64_STATE_CRS);
		port_printf("  rip %#lx cs.base %#lx ds.base %#lx cr0 %#lx "
		    "cr4 %#lx\n", comm->state.gprs[NVMM_X64_GPR_RIP],
		    comm->state.segs[NVMM_X64_SEG_CS].base,
		    comm->state.segs[NVMM_X64_SEG_DS].base,
		    comm->state.crs[NVMM_X64_CR_CR0],
		    comm->state.crs[NVMM_X64_CR_CR4]);
	}
	CHECK_EQ(exit->reason, reason);
	return exit;
}

static void
gpa_map(uint64_t hva_off, uint64_t gpa, uint64_t size, int prot)
{
	struct nvmm_ioc_gpa_map args = {
		.machid = machid, .hva = HVA_BASE + hva_off, .gpa = gpa,
		.size = size, .prot = prot,
	};

	CHECK_EQ(ioc(NVMM_IOC_GPA_MAP, &args), 0);
}

static void
gpa_unmap(uint64_t gpa, uint64_t size)
{
	struct nvmm_ioc_gpa_unmap args = {
		.machid = machid, .gpa = gpa, .size = size,
	};

	CHECK_EQ(ioc(NVMM_IOC_GPA_UNMAP, &args), 0);
}

static void
load(uint64_t gpa, const uint8_t *code, size_t len)
{
	memcpy(ram + gpa, code, len);
}

/* -------------------------------------------------------------------------- */

static void
test_setup(void)
{
	struct nvmm_ioc_capability cap;
	struct nvmm_ioc_machine_create mc;
	struct nvmm_ioc_vcpu_create vc;
	struct nvmm_ioc_hva_map hm;

	port_init();
	CHECK_EQ(port_x86_init(), 0);
	CHECK_EQ(nvmm_init(), 0);
	CHECK(nvmm_impl != NULL);

	memset(&cap, 0, sizeof(cap));
	CHECK_EQ(ioc(NVMM_IOC_CAPABILITY, &cap), 0);
	CHECK_EQ(cap.cap.version, NVMM_KERN_VERSION);
	CHECK_EQ(cap.cap.state_size, sizeof(struct nvmm_x64_state));
	CHECK_EQ(cap.cap.max_vcpus, NVMM_MAX_VCPUS);
	CHECK(cap.cap.arch.mxcsr_mask != 0);

	memset(&mc, 0, sizeof(mc));
	CHECK_EQ(ioc(NVMM_IOC_MACHINE_CREATE, &mc), 0);
	machid = mc.machid;

	memset(&vc, 0, sizeof(vc));
	vc.machid = machid;
	vc.cpuid = 0;
	CHECK_EQ(ioc(NVMM_IOC_VCPU_CREATE, &vc), 0);
	comm = vc.comm;
	CHECK(comm != NULL);

	/* Creating the same vCPU twice must fail, not corrupt anything. */
	CHECK(ioc(NVMM_IOC_VCPU_CREATE, &vc) != 0);

	memset(&hm, 0, sizeof(hm));
	hm.machid = machid;
	hm.hva = HVA_BASE;
	hm.size = HVA_SIZE;
	CHECK_EQ(ioc(NVMM_IOC_HVA_MAP, &hm), 0);
	ram = bare_hva_ptr(HVA_BASE);
	CHECK(ram != NULL);

	gpa_map(0, 0, RAM_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);

	/* Real mode, flat: CS base 0 instead of the reset vector. */
	get_state(NVMM_X64_STATE_ALL);
	comm->state.segs[NVMM_X64_SEG_CS].selector = 0;
	comm->state.segs[NVMM_X64_SEG_CS].base = 0;
	comm->state.gprs[NVMM_X64_GPR_RSP] = 0x8000;
	comm->state.gprs[NVMM_X64_GPR_RFLAGS] = 0x2;
	set_state(NVMM_X64_STATE_SEGS | NVMM_X64_STATE_GPRS);
}

/* An OUT instruction and a HLT each stop the guest and report where. */
static void
test_io_and_hlt(void)
{
	static const uint8_t code[] = {
		0xB0, 0x42,		/* mov  $0x42,%al   */
		0xE6, 0x80,		/* out  %al,$0x80   */
		0xBB, 0x34, 0x12,	/* mov  $0x1234,%bx */
		0xF4,			/* hlt              */
	};
	struct nvmm_vcpu_exit *exit;

	load(0x1000, code, sizeof(code));
	set_rip(0x1000);

	exit = run_expect(NVMM_VCPU_EXIT_IO);
	CHECK_EQ(exit->u.io.port, 0x80);
	CHECK_EQ(exit->u.io.in, false);
	CHECK_EQ(exit->u.io.operand_size, 1);
	CHECK_EQ(exit->u.io.npc, 0x1004);
	CHECK_EQ(get_gpr(NVMM_X64_GPR_RAX) & 0xFF, 0x42);
	CHECK_EQ(get_gpr(NVMM_X64_GPR_RIP), 0x1002);

	set_rip(exit->u.io.npc);
	exit = run_expect(NVMM_VCPU_EXIT_HALTED);
	/* The engine steps past HLT itself; there is nothing to resume. */
	CHECK_EQ(get_gpr(NVMM_X64_GPR_RIP), 0x1008);
	CHECK_EQ(get_gpr(NVMM_X64_GPR_RBX) & 0xFFFF, 0x1234);
}

/*
 * A write to an address with no memory behind it is reported to the emulator.
 * Mapping memory there lets the same instruction complete; unmapping it or
 * making it read-only brings the report back.
 */
static void
test_memory(void)
{
	static const uint8_t code[] = {
		0xB8, 0x00, 0x90,		/* mov  $0x9000,%ax  */
		0x8E, 0xD8,			/* mov  %ax,%ds      */
		0xC6, 0x06, 0x00, 0x00, 0x55,	/* movb $0x55,0x0000 */
		0xF4,				/* hlt               */
	};
	struct nvmm_vcpu_exit *exit;
	const unsigned long ipis = bare_stats.ipis;

	load(0x2000, code, sizeof(code));
	set_rip(0x2000);

	exit = run_expect(NVMM_VCPU_EXIT_MEMORY);
	CHECK_EQ(exit->u.mem.gpa, MMIO_GPA);
	CHECK((exit->u.mem.prot & PROT_WRITE) != 0);
	CHECK_EQ(get_gpr(NVMM_X64_GPR_RIP), 0x2005);

	/* Back the address with memory and let the write happen. */
	gpa_map(RAM_SIZE, MMIO_GPA, 0x1000, PROT_READ | PROT_WRITE);
	CHECK_EQ(ram[RAM_SIZE], 0x00);
	exit = run_expect(NVMM_VCPU_EXIT_HALTED);
	CHECK_EQ(ram[RAM_SIZE], 0x55);

	/* Take the memory away again: the write must fault once more. */
	ram[RAM_SIZE] = 0;
	gpa_unmap(MMIO_GPA, 0x1000);
	set_rip(0x2005);
	exit = run_expect(NVMM_VCPU_EXIT_MEMORY);
	CHECK_EQ(exit->u.mem.gpa, MMIO_GPA);
	CHECK_EQ(ram[RAM_SIZE], 0x00);

	/* Read-only memory: reads would work, the write still faults. */
	gpa_map(RAM_SIZE, MMIO_GPA, 0x1000, PROT_READ);
	exit = run_expect(NVMM_VCPU_EXIT_MEMORY);
	CHECK_EQ(exit->u.mem.gpa, MMIO_GPA);
	CHECK((exit->u.mem.prot & PROT_WRITE) != 0);
	CHECK_EQ(ram[RAM_SIZE], 0x00);
	gpa_unmap(MMIO_GPA, 0x1000);

	/* Every change to the guest's address space kicked the other CPUs. */
	CHECK(bare_stats.ipis >= ipis + 4);

	/* Partial unmap in the middle of guest RAM, then put it back. */
	gpa_unmap(0x40000, 0x2000);
	gpa_map(0x40000, 0x40000, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
}

/* CPUID is answered by the engine without involving the emulator. */
static void
test_cpuid(void)
{
	static const uint8_t code[] = {
		0x66, 0xB8, 0x00, 0x00, 0x00, 0x40,	/* mov $0x40000000,%eax */
		0x0F, 0xA2,				/* cpuid                */
		0xF4,					/* hlt                  */
	};
	uint32_t sig[3];

	load(0x3000, code, sizeof(code));
	set_rip(0x3000);

	(void)run_expect(NVMM_VCPU_EXIT_HALTED);
	sig[0] = (uint32_t)get_gpr(NVMM_X64_GPR_RBX);
	sig[1] = (uint32_t)get_gpr(NVMM_X64_GPR_RCX);
	sig[2] = (uint32_t)get_gpr(NVMM_X64_GPR_RDX);
	CHECK(memcmp(sig, "___ NVMM ___", 12) == 0);
}

/*
 * The guest loads an SSE register. The host's copy of that register must
 * survive, and the guest's value must be readable through the state API.
 */
static void
test_fpu(void)
{
	static const uint8_t code[] = {
		0x0F, 0x20, 0xE0,			/* mov  %cr4,%eax        */
		0x66, 0x0D, 0x00, 0x06, 0x00, 0x00,	/* or   $0x600,%eax      */
		0x0F, 0x22, 0xE0,			/* mov  %eax,%cr4        */
		0x0F, 0x20, 0xC0,			/* mov  %cr0,%eax        */
		0x66, 0x83, 0xE0, 0xF3,			/* and  $~0xC,%eax       */
		0x0F, 0x22, 0xC0,			/* mov  %eax,%cr0        */
		0x66, 0xB8, 0xEF, 0xBE, 0xAD, 0xDE,	/* mov  $0xDEADBEEF,%eax */
		0x66, 0x0F, 0x6E, 0xC0,			/* movd %eax,%xmm0       */
		0xF4,					/* hlt                   */
	};
	static const uint8_t expect[4] = { 0xEF, 0xBE, 0xAD, 0xDE };
	const uint64_t host_pattern = 0x1122334455667788ULL;
	uint64_t host_after;

	load(0x4000, code, sizeof(code));
	set_rip(0x4000);

	/*
	 * The test kernel is built without SSE code generation, so nothing
	 * between these two statements touches %xmm0 except the engine.
	 */
	__asm volatile ("movq %0,%%xmm0" : : "r" (host_pattern));
	(void)run_expect(NVMM_VCPU_EXIT_HALTED);
	__asm volatile ("movq %%xmm0,%0" : "=r" (host_after));
	CHECK_EQ(host_after, host_pattern);

	get_state(NVMM_X64_STATE_FPU);
	CHECK(memcmp(comm->state.fpu.fx_xmm[0].xmm_bytes, expect, 4) == 0);
}

/* The vCPU loop stops when the host says it has something else to do. */
static void
test_return_needed(void)
{
	static const uint8_t code[] = {
		0x0F, 0xA2,	/* cpuid: exits, handled in the kernel */
		0xEB, 0xFC,	/* jmp back to the cpuid               */
	};
	struct nvmm_vcpu_exit *exit;

	load(0x5000, code, sizeof(code));
	set_rip(0x5000);

	/*
	 * Without the flag this guest would spin for ever, exiting and
	 * re-entering. With it, the first exit returns to the caller.
	 */
	bare_return_needed = true;
	exit = run();
	bare_return_needed = false;
	CHECK_EQ(exit->reason, NVMM_VCPU_EXIT_NONE);
	CHECK(!port_preempt_disabled());
}

static void
test_teardown(void)
{
	struct nvmm_ioc_vcpu_destroy vd = { .machid = machid, .cpuid = 0 };
	struct nvmm_ioc_machine_destroy md = { .machid = machid };

	CHECK_EQ(ioc(NVMM_IOC_VCPU_DESTROY, &vd), 0);
	CHECK(ioc(NVMM_IOC_VCPU_DESTROY, &vd) != 0);
	CHECK_EQ(ioc(NVMM_IOC_MACHINE_DESTROY, &md), 0);
	CHECK(ioc(NVMM_IOC_MACHINE_DESTROY, &md) != 0);

	/* What closing the device does for a process's leftover mappings. */
	port_vm_cleanup_pid(owner.pid);

	nvmm_fini();
	port_x86_fini();
	port_fini();

	/* Everything taken has been given back. */
	CHECK_EQ(bare_stats.malloc_live, 0);
	CHECK_EQ(bare_stats.malloc_bytes, 0);
	CHECK_EQ(bare_stats.pages_live, 0);
	CHECK_EQ(bare_stats.membufs_live, 0);
	CHECK_EQ(bare_stats.maps_live, 0);
	CHECK_EQ(bare_stats.locks_live, 0);
	CHECK(!port_preempt_disabled());
}

static void
report_cpu(void)
{
	cpuid_desc_t d;
	char vendor[13];

	x86_get_cpuid(0, &d);
	memcpy(vendor, &d.ebx, 4);
	memcpy(vendor + 4, &d.edx, 4);
	memcpy(vendor + 8, &d.ecx, 4);
	vendor[12] = '\0';
	port_printf("cpu: %s\n", vendor);

	x86_get_cpuid(0x80000001, &d);
	port_printf("cpu: svm=%u\n", (d.ecx >> 2) & 1);
	x86_get_cpuid(0x8000000A, &d);
	port_printf("cpu: svm rev %u, nasid %u, npt=%u nrips=%u "
	    "flushbyasid=%u decodeassists=%u\n", d.eax & 0xFF, d.ebx,
	    d.edx & 1, (d.edx >> 3) & 1, (d.edx >> 6) & 1, (d.edx >> 7) & 1);
}

#define RUN(t)								\
	do {								\
		port_printf("  %-22s", #t);				\
		t();							\
		port_printf("ok\n");					\
	} while (0)

void
kmain(void)
{
	int pass;

	bare_init(256ULL << 20);
	port_printf("\nnvmm bare-metal test kernel\n");
	report_cpu();
	host_state_seed();

	/* Twice over: the second pass catches state left behind by the first. */
	for (pass = 1; pass <= 2; pass++) {
		port_printf("pass %d\n", pass);
		RUN(test_setup);
		RUN(test_io_and_hlt);
		RUN(test_memory);
		RUN(test_cpuid);
		RUN(test_fpu);
		RUN(test_return_needed);
		RUN(test_teardown);
	}

	port_printf("ALL TESTS PASSED (%d checks)\n", nchecks);
	bare_exit(0);
}
