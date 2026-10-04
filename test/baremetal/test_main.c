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

static struct nvmm_vcpu_exit *
run(void)
{
	static struct nvmm_ioc_vcpu_run args;

	memset(&args, 0, sizeof(args));
	args.machid = machid;
	args.cpuid = 0;
	CHECK_EQ(ioc(NVMM_IOC_VCPU_RUN, &args), 0);
	return &args.exit;
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

	exit = run();
	CHECK_EQ(exit->reason, NVMM_VCPU_EXIT_IO);
	CHECK_EQ(exit->u.io.port, 0x80);
	CHECK_EQ(exit->u.io.in, false);
	CHECK_EQ(exit->u.io.operand_size, 1);
	CHECK_EQ(exit->u.io.npc, 0x1004);
	CHECK_EQ(get_gpr(NVMM_X64_GPR_RAX) & 0xFF, 0x42);
	CHECK_EQ(get_gpr(NVMM_X64_GPR_RIP), 0x1002);

	set_rip(exit->u.io.npc);
	exit = run();
	CHECK_EQ(exit->reason, NVMM_VCPU_EXIT_HALTED);
	CHECK_EQ(exit->u.insn.npc, 0x1008);
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

	exit = run();
	CHECK_EQ(exit->reason, NVMM_VCPU_EXIT_MEMORY);
	CHECK_EQ(exit->u.mem.gpa, MMIO_GPA);
	CHECK((exit->u.mem.prot & PROT_WRITE) != 0);
	CHECK_EQ(get_gpr(NVMM_X64_GPR_RIP), 0x2005);

	/* Back the address with memory and let the write happen. */
	gpa_map(RAM_SIZE, MMIO_GPA, 0x1000, PROT_READ | PROT_WRITE);
	CHECK_EQ(ram[RAM_SIZE], 0x00);
	exit = run();
	CHECK_EQ(exit->reason, NVMM_VCPU_EXIT_HALTED);
	CHECK_EQ(ram[RAM_SIZE], 0x55);

	/* Take the memory away again: the write must fault once more. */
	ram[RAM_SIZE] = 0;
	gpa_unmap(MMIO_GPA, 0x1000);
	set_rip(0x2005);
	exit = run();
	CHECK_EQ(exit->reason, NVMM_VCPU_EXIT_MEMORY);
	CHECK_EQ(exit->u.mem.gpa, MMIO_GPA);
	CHECK_EQ(ram[RAM_SIZE], 0x00);

	/* Read-only memory: reads would work, the write still faults. */
	gpa_map(RAM_SIZE, MMIO_GPA, 0x1000, PROT_READ);
	exit = run();
	CHECK_EQ(exit->reason, NVMM_VCPU_EXIT_MEMORY);
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
	struct nvmm_vcpu_exit *exit;
	uint32_t sig[3];

	load(0x3000, code, sizeof(code));
	set_rip(0x3000);

	exit = run();
	CHECK_EQ(exit->reason, NVMM_VCPU_EXIT_HALTED);
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
	struct nvmm_vcpu_exit *exit;
	uint64_t host_after;

	load(0x4000, code, sizeof(code));
	set_rip(0x4000);

	__asm volatile ("movq %0,%%xmm0" : : "r" (host_pattern));
	exit = run();
	__asm volatile ("movq %%xmm0,%0" : "=r" (host_after));

	CHECK_EQ(exit->reason, NVMM_VCPU_EXIT_HALTED);
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
