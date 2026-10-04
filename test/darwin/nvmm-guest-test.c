/*
 * nvmm-guest-test.c — the first real guests, for an AMD machine.
 *
 * Everything else in this repository runs either the engine under emulation
 * or the macOS glue without the engine. This runs both, on a real CPU, through
 * libnvmm: the same guest programs as test/baremetal/test_main.c.
 *
 * It goes in stages of rising risk, and prints each one BEFORE starting it,
 * so that if the machine panics the last line on the terminal (run it over
 * ssh, or with output to a file on another machine) says where.
 *
 * Usage (as root, with NVMM.kext loaded):
 *   nvmm-guest-test 1     open the device, read capabilities. No guest.
 *   nvmm-guest-test 2     + create and destroy a machine and a vCPU. No guest.
 *   nvmm-guest-test 3     + the first VMRUN: a guest that halts at once.
 *   nvmm-guest-test 4     + I/O, memory faults, CPUID, FPU, 2M pages.
 *   nvmm-guest-test       everything: + long runs, two vCPUs, many machines.
 */

#include <sys/mman.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nvmm.h>

static int failures, checks;

#define CHECK(cond)							\
	do {								\
		checks++;						\
		if (!(cond)) {						\
			printf("FAIL %s:%d: %s (errno %d: %s)\n",	\
			    __FILE__, __LINE__, #cond, errno,		\
			    strerror(errno));				\
			fflush(stdout);					\
			failures++;					\
		}							\
	} while (0)

#define STAGE(n, what)							\
	do {								\
		printf("stage %d: %s\n", (n), (what));			\
		fflush(stdout);						\
		fsync(STDOUT_FILENO);					\
	} while (0)

#define STEP(what)							\
	do {								\
		printf("  %s\n", (what));				\
		fflush(stdout);						\
		fsync(STDOUT_FILENO);					\
	} while (0)

#define RAM_SIZE	(16ULL << 20)	/* 2M aligned: eight 2M pages */
#define RAM_LOW		0x80000ULL	/* what a small test maps at 0 */
#define MMIO_GPA	0x90000ULL	/* a hole, in the small layout */

struct vm {
	struct nvmm_machine mach;
	struct nvmm_vcpu vcpu;
	uint8_t *ram;
};

static void
vm_create(struct vm *vm, uint64_t mapped)
{
	struct nvmm_x64_state *st;

	memset(vm, 0, sizeof(*vm));
	CHECK(nvmm_machine_create(&vm->mach) == 0);
	CHECK(nvmm_vcpu_create(&vm->mach, 0, &vm->vcpu) == 0);

	vm->ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	CHECK(vm->ram != MAP_FAILED);
	CHECK(nvmm_hva_map(&vm->mach, (uintptr_t)vm->ram, RAM_SIZE) == 0);
	CHECK(nvmm_gpa_map(&vm->mach, (uintptr_t)vm->ram, 0, mapped,
	    PROT_READ | PROT_WRITE | PROT_EXEC) == 0);

	/* Real mode, flat: CS base 0 instead of the reset vector. */
	CHECK(nvmm_vcpu_getstate(&vm->mach, &vm->vcpu,
	    NVMM_X64_STATE_ALL) == 0);
	st = vm->vcpu.state;
	st->segs[NVMM_X64_SEG_CS].selector = 0;
	st->segs[NVMM_X64_SEG_CS].base = 0;
	st->gprs[NVMM_X64_GPR_RSP] = 0x8000;
	st->gprs[NVMM_X64_GPR_RFLAGS] = 0x2;
	CHECK(nvmm_vcpu_setstate(&vm->mach, &vm->vcpu,
	    NVMM_X64_STATE_SEGS | NVMM_X64_STATE_GPRS) == 0);
}

static void
vm_destroy(struct vm *vm)
{
	CHECK(nvmm_vcpu_destroy(&vm->mach, &vm->vcpu) == 0);
	CHECK(nvmm_machine_destroy(&vm->mach) == 0);
	/* The kernel's mapping went with the machine; drop the range. */
	(void)munmap(vm->ram, RAM_SIZE);
}

static uint64_t
get_gpr(struct vm *vm, int reg)
{
	CHECK(nvmm_vcpu_getstate(&vm->mach, &vm->vcpu,
	    NVMM_X64_STATE_GPRS) == 0);
	return vm->vcpu.state->gprs[reg];
}

static void
set_gpr(struct vm *vm, int reg, uint64_t val)
{
	CHECK(nvmm_vcpu_getstate(&vm->mach, &vm->vcpu,
	    NVMM_X64_STATE_GPRS) == 0);
	vm->vcpu.state->gprs[reg] = val;
	CHECK(nvmm_vcpu_setstate(&vm->mach, &vm->vcpu,
	    NVMM_X64_STATE_GPRS) == 0);
}

/*
 * Run until the guest needs the caller. The engine also comes back with
 * "nothing to do" when the host had an interrupt or the in-kernel exit budget
 * ran out; those are counted and the guest is resumed.
 */
static uint64_t
run(struct vm *vm, unsigned long *returns)
{
	unsigned long n = 0;

	for (;;) {
		if (nvmm_vcpu_run(&vm->mach, &vm->vcpu) != 0) {
			CHECK(!"nvmm_vcpu_run failed");
			return NVMM_VCPU_EXIT_INVALID;
		}
		n++;
		if (vm->vcpu.exit->reason != NVMM_VCPU_EXIT_NONE)
			break;
		if (n > 100000000UL) {
			CHECK(!"guest never needed the caller");
			return NVMM_VCPU_EXIT_INVALID;
		}
	}
	if (returns != NULL)
		*returns = n;
	return vm->vcpu.exit->reason;
}

#define RUN_EXPECT(vm, want)						\
	do {								\
		uint64_t r_ = run((vm), NULL);				\
		if (r_ != (want)) {					\
			printf("  exit reason %#llx, wanted %#llx, "	\
			    "rip %#llx\n", (unsigned long long)r_,	\
			    (unsigned long long)(want),			\
			    (unsigned long long)get_gpr((vm),		\
			    NVMM_X64_GPR_RIP));				\
		}							\
		CHECK(r_ == (want));					\
	} while (0)

static void
load(struct vm *vm, uint64_t gpa, const uint8_t *code, size_t len)
{
	memcpy(vm->ram + gpa, code, len);
}

/* -------------------------------------------------------------------------- */

static void
stage1_open(void)
{
	struct nvmm_capability cap;

	STAGE(1, "open /dev/nvmm and read capabilities (no guest runs)");
	if (nvmm_init() != 0) {
		printf("nvmm_init failed: %s\nIs NVMM.kext loaded, and are "
		    "you root?\n", strerror(errno));
		exit(2);
	}
	memset(&cap, 0, sizeof(cap));
	CHECK(nvmm_capability(&cap) == 0);
	printf("  version %u, state size %u, max machines %u, max vcpus %u\n",
	    cap.version, cap.state_size, cap.max_machines, cap.max_vcpus);
	CHECK(cap.state_size == sizeof(struct nvmm_x64_state));
}

static void
stage2_objects(void)
{
	struct vm vm;
	int i;

	STAGE(2, "create and destroy machines and vCPUs (no guest runs)");
	for (i = 0; i < 4; i++) {
		vm_create(&vm, RAM_LOW);
		CHECK(vm.vcpu.state->gprs[NVMM_X64_GPR_RFLAGS] == 0x2);
		vm_destroy(&vm);
	}
}

static void
stage3_first_run(void)
{
	static const uint8_t code[] = { 0xF4 };		/* hlt */
	struct vm vm;

	STAGE(3, "THE FIRST VMRUN: a guest that halts at once");
	vm_create(&vm, RAM_LOW);
	load(&vm, 0x1000, code, sizeof(code));
	set_gpr(&vm, NVMM_X64_GPR_RIP, 0x1000);
	STEP("entering the guest");
	RUN_EXPECT(&vm, NVMM_VCPU_EXIT_HALTED);
	STEP("back in the host");
	CHECK(get_gpr(&vm, NVMM_X64_GPR_RIP) == 0x1001);

	/* And again, a few times, on whatever CPU the thread lands on. */
	for (int i = 0; i < 1000; i++) {
		set_gpr(&vm, NVMM_X64_GPR_RIP, 0x1000);
		RUN_EXPECT(&vm, NVMM_VCPU_EXIT_HALTED);
	}
	STEP("1000 more entries ok");
	vm_destroy(&vm);
}

static void
t_io_and_hlt(struct vm *vm)
{
	static const uint8_t code[] = {
		0xB0, 0x42,		/* mov  $0x42,%al   */
		0xE6, 0x80,		/* out  %al,$0x80   */
		0xBB, 0x34, 0x12,	/* mov  $0x1234,%bx */
		0xF4,			/* hlt              */
	};

	STEP("I/O and HLT exits");
	load(vm, 0x1000, code, sizeof(code));
	set_gpr(vm, NVMM_X64_GPR_RIP, 0x1000);
	RUN_EXPECT(vm, NVMM_VCPU_EXIT_IO);
	CHECK(vm->vcpu.exit->u.io.port == 0x80);
	CHECK(vm->vcpu.exit->u.io.in == false);
	CHECK(vm->vcpu.exit->u.io.operand_size == 1);
	/* Next-RIP save, which the emulator tests cannot cover. */
	CHECK(vm->vcpu.exit->u.io.npc == 0x1004);
	CHECK((get_gpr(vm, NVMM_X64_GPR_RAX) & 0xFF) == 0x42);

	set_gpr(vm, NVMM_X64_GPR_RIP, vm->vcpu.exit->u.io.npc);
	RUN_EXPECT(vm, NVMM_VCPU_EXIT_HALTED);
	CHECK(get_gpr(vm, NVMM_X64_GPR_RIP) == 0x1008);
	CHECK((get_gpr(vm, NVMM_X64_GPR_RBX) & 0xFFFF) == 0x1234);
}

static void
t_memory(struct vm *vm)
{
	static const uint8_t code[] = {
		0xB8, 0x00, 0x90,		/* mov  $0x9000,%ax  */
		0x8E, 0xD8,			/* mov  %ax,%ds      */
		0xC6, 0x06, 0x00, 0x00, 0x55,	/* movb $0x55,0x0000 */
		0xF4,				/* hlt               */
	};
	uint8_t *page = vm->ram + RAM_LOW;

	STEP("nested page faults; map, unmap, read-only");
	load(vm, 0x2000, code, sizeof(code));
	set_gpr(vm, NVMM_X64_GPR_RIP, 0x2000);
	RUN_EXPECT(vm, NVMM_VCPU_EXIT_MEMORY);
	CHECK(vm->vcpu.exit->u.mem.gpa == MMIO_GPA);
	CHECK((vm->vcpu.exit->u.mem.prot & PROT_WRITE) != 0);
	CHECK(get_gpr(vm, NVMM_X64_GPR_RIP) == 0x2005);

	/* Back the address with memory: the same instruction completes. */
	CHECK(nvmm_gpa_map(&vm->mach, (uintptr_t)page, MMIO_GPA, 0x1000,
	    PROT_READ | PROT_WRITE) == 0);
	*page = 0;
	RUN_EXPECT(vm, NVMM_VCPU_EXIT_HALTED);
	CHECK(*page == 0x55);

	/*
	 * Take it away again. The CPU has this translation in its TLB, so
	 * this is the stale-TLB case the emulator cannot show: if the flush
	 * is missed, the write lands and no fault is reported.
	 */
	*page = 0;
	CHECK(nvmm_gpa_unmap(&vm->mach, (uintptr_t)page, MMIO_GPA,
	    0x1000) == 0);
	set_gpr(vm, NVMM_X64_GPR_RIP, 0x2005);
	RUN_EXPECT(vm, NVMM_VCPU_EXIT_MEMORY);
	CHECK(vm->vcpu.exit->u.mem.gpa == MMIO_GPA);
	CHECK(*page == 0x00);

	/* Read-only: the write still faults. Same TLB concern. */
	CHECK(nvmm_gpa_map(&vm->mach, (uintptr_t)page, MMIO_GPA, 0x1000,
	    PROT_READ | PROT_WRITE) == 0);
	RUN_EXPECT(vm, NVMM_VCPU_EXIT_HALTED);
	CHECK(*page == 0x55);
	*page = 0;
	CHECK(nvmm_gpa_unmap(&vm->mach, (uintptr_t)page, MMIO_GPA,
	    0x1000) == 0);
	CHECK(nvmm_gpa_map(&vm->mach, (uintptr_t)page, MMIO_GPA, 0x1000,
	    PROT_READ) == 0);
	set_gpr(vm, NVMM_X64_GPR_RIP, 0x2005);
	RUN_EXPECT(vm, NVMM_VCPU_EXIT_MEMORY);
	CHECK((vm->vcpu.exit->u.mem.prot & PROT_WRITE) != 0);
	CHECK(*page == 0x00);
	CHECK(nvmm_gpa_unmap(&vm->mach, (uintptr_t)page, MMIO_GPA,
	    0x1000) == 0);
}

static void
t_cpuid(struct vm *vm)
{
	static const uint8_t code[] = {
		0x66, 0xB8, 0x00, 0x00, 0x00, 0x40,	/* mov $0x40000000,%eax */
		0x0F, 0xA2,				/* cpuid                */
		0xF4,					/* hlt                  */
	};
	uint32_t sig[3];

	STEP("CPUID answered by the engine");
	load(vm, 0x3000, code, sizeof(code));
	set_gpr(vm, NVMM_X64_GPR_RIP, 0x3000);
	RUN_EXPECT(vm, NVMM_VCPU_EXIT_HALTED);
	sig[0] = (uint32_t)get_gpr(vm, NVMM_X64_GPR_RBX);
	sig[1] = (uint32_t)get_gpr(vm, NVMM_X64_GPR_RCX);
	sig[2] = (uint32_t)get_gpr(vm, NVMM_X64_GPR_RDX);
	CHECK(memcmp(sig, "___ NVMM ___", 12) == 0);
}

static void
t_fpu(struct vm *vm)
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
	uint64_t host_after = 0;
	uint64_t reason;

	STEP("FPU: the host's registers survive a guest that uses SSE");
	load(vm, 0x4000, code, sizeof(code));
	set_gpr(vm, NVMM_X64_GPR_RIP, 0x4000);

	/*
	 * A process's xmm15 is preserved by the kernel across system calls
	 * and nothing in libnvmm's run path uses it, so if it changes, the
	 * engine leaked guest FPU state into the host.
	 */
	__asm volatile ("movq %0,%%xmm15" : : "r" (host_pattern) : "xmm15");
	reason = run(vm, NULL);
	__asm volatile ("movq %%xmm15,%0" : "=r" (host_after));
	CHECK(reason == NVMM_VCPU_EXIT_HALTED);
	CHECK(host_after == host_pattern);

	CHECK(nvmm_vcpu_getstate(&vm->mach, &vm->vcpu,
	    NVMM_X64_STATE_FPU) == 0);
	CHECK(memcmp(vm->vcpu.state->fpu.fx_xmm[0].xmm_bytes, expect, 4) == 0);
}

/* All 16M mapped in one go: where the host memory allows, as 2M pages. */
static void
t_large_pages(void)
{
	static const uint8_t code[] = {
		0xB8, 0x00, 0x90,		/* mov  $0x9000,%ax  */
		0x8E, 0xD8,			/* mov  %ax,%ds      */
		0xC6, 0x06, 0x00, 0x00, 0x66,	/* movb $0x66,0x0000 */
		0xB8, 0xFF, 0xFF,		/* mov  $0xffff,%ax  */
		0x8E, 0xD8,			/* mov  %ax,%ds      */
		0xC6, 0x06, 0x10, 0x80, 0x77,	/* movb $0x77,0x8010 */
		0xF4,				/* hlt               */
	};
	struct vm vm;

	STEP("guest RAM in 2M pages");
	vm_create(&vm, RAM_SIZE);
	load(&vm, 0x3000, code, sizeof(code));
	set_gpr(&vm, NVMM_X64_GPR_RIP, 0x3000);
	RUN_EXPECT(&vm, NVMM_VCPU_EXIT_HALTED);
	CHECK(vm.ram[MMIO_GPA] == 0x66);
	CHECK(vm.ram[0xFFFF0 + 0x8010] == 0x77);

	/*
	 * Splitting a 2M page is covered by the emulator tests; libnvmm only
	 * unmaps whole areas, so it cannot be reached from here. Unmap it
	 * all instead: the guest's first fetch must then fault, which a stale
	 * 2M translation left in the TLB would hide.
	 */
	CHECK(nvmm_gpa_unmap(&vm.mach, (uintptr_t)vm.ram, 0, RAM_SIZE) == 0);
	set_gpr(&vm, NVMM_X64_GPR_RIP, 0x3000);
	RUN_EXPECT(&vm, NVMM_VCPU_EXIT_MEMORY);
	CHECK(vm.vcpu.exit->u.mem.gpa == 0x3000);
	CHECK((vm.vcpu.exit->u.mem.prot & PROT_EXEC) != 0);
	vm_destroy(&vm);
}

static void
stage4_exits(void)
{
	struct vm vm;

	STAGE(4, "the exits an emulator relies on");
	vm_create(&vm, RAM_LOW);
	t_io_and_hlt(&vm);
	t_memory(&vm);
	t_cpuid(&vm);
	t_fpu(&vm);
	vm_destroy(&vm);
	t_large_pages();
}

/* A guest that counts down from 'n' and halts. */
static void
load_spin(struct vm *vm, uint32_t n)
{
	uint8_t code[] = {
		0x66, 0xB9, 0, 0, 0, 0,		/* mov  $n,%ecx  */
		0x66, 0x49,			/* dec  %ecx     */
		0x75, 0xFC,			/* jnz  back     */
		0xF4,				/* hlt           */
	};

	memcpy(code + 2, &n, 4);
	load(vm, 0x5000, code, sizeof(code));
	set_gpr(vm, NVMM_X64_GPR_RIP, 0x5000);
}

static void *
spin_thread(void *arg)
{
	struct vm *vm = arg;
	unsigned long returns = 0;
	uint64_t reason;

	reason = run(vm, &returns);
	CHECK(reason == NVMM_VCPU_EXIT_HALTED);
	CHECK((get_gpr(vm, NVMM_X64_GPR_RCX) & 0xFFFFFFFF) == 0);
	printf("    a guest finished after %lu return(s) to the host\n",
	    returns);
	fflush(stdout);
	return NULL;
}

static void
stage5_stress(void)
{
	static const uint8_t loop[] = {
		0x46,		/* inc  %si                            */
		0x0F, 0xA2,	/* cpuid: exits, handled in the kernel */
		0xEB, 0xFB,	/* jmp  back to the inc                */
	};
	struct vm vm, vms[2];
	pthread_t th[2];
	unsigned long returns = 0;
	uint64_t si0, si1;
	int i;

	STAGE(5, "long runs, host interrupts, two guests at once");

	/*
	 * Seconds inside the guest: the host's timer interrupts must get
	 * through, the thread must be preemptible between entries, and the
	 * guest's registers must survive every interruption.
	 */
	STEP("a guest that computes for a few seconds");
	vm_create(&vm, RAM_LOW);
	load_spin(&vm, 0xC0000000U);
	CHECK(run(&vm, &returns) == NVMM_VCPU_EXIT_HALTED);
	CHECK((get_gpr(&vm, NVMM_X64_GPR_RCX) & 0xFFFFFFFF) == 0);
	printf("    interrupted and resumed %lu time(s)\n", returns - 1);
	CHECK(returns > 1);

	STEP("a guest that exits in a tight loop: the in-kernel budget");
	load(&vm, 0x7000, loop, sizeof(loop));
	set_gpr(&vm, NVMM_X64_GPR_RSI, 0);
	set_gpr(&vm, NVMM_X64_GPR_RIP, 0x7000);
	CHECK(nvmm_vcpu_run(&vm.mach, &vm.vcpu) == 0);
	CHECK(vm.vcpu.exit->reason == NVMM_VCPU_EXIT_NONE);
	si0 = get_gpr(&vm, NVMM_X64_GPR_RSI) & 0xFFFF;
	CHECK(nvmm_vcpu_run(&vm.mach, &vm.vcpu) == 0);
	si1 = get_gpr(&vm, NVMM_X64_GPR_RSI) & 0xFFFF;
	printf("    %llu then %llu CPUID exits per call\n",
	    (unsigned long long)si0, (unsigned long long)(si1 - si0));
	/* A host interrupt can cut a batch short; it can never be longer. */
	CHECK(si0 >= 1 && si0 <= 32 && si1 - si0 >= 1 && si1 - si0 <= 32);
	vm_destroy(&vm);

	STEP("two machines running at the same time");
	for (i = 0; i < 2; i++) {
		vm_create(&vms[i], RAM_LOW);
		load_spin(&vms[i], 0x80000000U);
	}
	for (i = 0; i < 2; i++)
		CHECK(pthread_create(&th[i], NULL, spin_thread, &vms[i]) == 0);
	for (i = 0; i < 2; i++)
		CHECK(pthread_join(th[i], NULL) == 0);
	for (i = 0; i < 2; i++)
		vm_destroy(&vms[i]);

	STEP("200 machines created, run and destroyed");
	for (i = 0; i < 200; i++) {
		static const uint8_t hlt[] = { 0xF4 };

		vm_create(&vm, RAM_LOW);
		load(&vm, 0x1000, hlt, sizeof(hlt));
		set_gpr(&vm, NVMM_X64_GPR_RIP, 0x1000);
		RUN_EXPECT(&vm, NVMM_VCPU_EXIT_HALTED);
		vm_destroy(&vm);
	}
}

int
main(int argc, char **argv)
{
	const int last = (argc > 1) ? atoi(argv[1]) : 5;

	if (last < 1 || last > 5) {
		fprintf(stderr, "usage: nvmm-guest-test [1-5]\n");
		return 2;
	}

	stage1_open();
	if (last >= 2 && failures == 0)
		stage2_objects();
	if (last >= 3 && failures == 0)
		stage3_first_run();
	if (last >= 4 && failures == 0)
		stage4_exits();
	if (last >= 5 && failures == 0)
		stage5_stress();

	printf("%d checks, %d failed\n%s (through stage %d)\n", checks,
	    failures, failures ? "GUEST TEST FAILED" : "GUEST TEST PASSED",
	    last);
	return failures ? 1 : 0;
}
