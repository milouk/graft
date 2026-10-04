/*
 * nvmm-fake-test.c — libnvmm against the self-test kext's stand-in engine.
 *
 * This is what an emulator does, through the same library and the same
 * ioctls, but against a backend that runs no guest code: it obeys a command in
 * RAX (see darwin/nvmm_fake_engine.h). So it checks everything between the
 * emulator and the engine: opening the device, machines and vCPUs, the comm
 * page the process shares with the kernel, guest RAM replacing an existing
 * mapping, the guest-physical map, and teardown.
 *
 * Usage (as root, with NVMMSelfTest.kext loaded):
 *   nvmm-fake-test            run everything
 *   nvmm-fake-test --abandon  set a machine up, then exit without destroying it
 */

#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nvmm.h>
#include "nvmm_fake_engine.h"

static int failures, checks;

#define CHECK(cond)							\
	do {								\
		checks++;						\
		if (!(cond)) {						\
			printf("FAIL %s:%d: %s (errno %d: %s)\n",	\
			    __FILE__, __LINE__, #cond, errno,		\
			    strerror(errno));				\
			failures++;					\
		}							\
	} while (0)

#define RAM_SIZE	(8ULL << 20)
#define HOLE_GPA	(64ULL << 20)	/* never mapped */

struct vm {
	struct nvmm_machine mach;
	struct nvmm_vcpu vcpu;
	uint8_t *ram;
};

static void
set_gpr(struct vm *vm, int reg, uint64_t val)
{
	CHECK(nvmm_vcpu_getstate(&vm->mach, &vm->vcpu,
	    NVMM_X64_STATE_GPRS) == 0);
	vm->vcpu.state->gprs[reg] = val;
	CHECK(nvmm_vcpu_setstate(&vm->mach, &vm->vcpu,
	    NVMM_X64_STATE_GPRS) == 0);
}

static uint64_t
get_gpr(struct vm *vm, int reg)
{
	CHECK(nvmm_vcpu_getstate(&vm->mach, &vm->vcpu,
	    NVMM_X64_STATE_GPRS) == 0);
	return vm->vcpu.state->gprs[reg];
}

static void
vm_create(struct vm *vm)
{
	memset(vm, 0, sizeof(*vm));
	CHECK(nvmm_machine_create(&vm->mach) == 0);
	CHECK(nvmm_vcpu_create(&vm->mach, 0, &vm->vcpu) == 0);
	CHECK(vm->vcpu.state != NULL);

	/*
	 * Guest RAM the way an emulator does it: allocate, fill, then hand
	 * the range to the kernel, which replaces it with shared wired memory.
	 */
	vm->ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	CHECK(vm->ram != MAP_FAILED);
	memset(vm->ram, 0xEE, RAM_SIZE);
	CHECK(nvmm_hva_map(&vm->mach, (uintptr_t)vm->ram, RAM_SIZE) == 0);

	/* The replacement is fresh zeroed memory at the same address. */
	CHECK(vm->ram[0] == 0 && vm->ram[RAM_SIZE - 1] == 0);
	memset(vm->ram, 0x5A, RAM_SIZE);
	CHECK(vm->ram[RAM_SIZE / 2] == 0x5A);

	CHECK(nvmm_gpa_map(&vm->mach, (uintptr_t)vm->ram, 0, RAM_SIZE,
	    PROT_READ | PROT_WRITE | PROT_EXEC) == 0);
}

static void
vm_destroy(struct vm *vm)
{
	CHECK(nvmm_gpa_unmap(&vm->mach, (uintptr_t)vm->ram, 0, RAM_SIZE) == 0);
	CHECK(nvmm_hva_unmap(&vm->mach, (uintptr_t)vm->ram, RAM_SIZE) == 0);
	CHECK(nvmm_vcpu_destroy(&vm->mach, &vm->vcpu) == 0);
	CHECK(nvmm_machine_destroy(&vm->mach) == 0);
}

static void
test_state(struct vm *vm)
{
	uint64_t rip;

	/* The vCPU starts in the x86 reset state. */
	CHECK(nvmm_vcpu_getstate(&vm->mach, &vm->vcpu,
	    NVMM_X64_STATE_ALL) == 0);
	CHECK(vm->vcpu.state->gprs[NVMM_X64_GPR_RIP] == 0xFFF0);
	CHECK(vm->vcpu.state->segs[NVMM_X64_SEG_CS].selector == 0xF000);

	/* What is written through the comm page comes back from the kernel. */
	set_gpr(vm, NVMM_X64_GPR_R15, 0x1122334455667788ULL);
	set_gpr(vm, NVMM_X64_GPR_RIP, 0x1000);
	vm->vcpu.state->gprs[NVMM_X64_GPR_R15] = 0;	/* scribble locally */
	set_gpr(vm, NVMM_X64_GPR_RAX, NVMM_FAKE_CMD_HLT);
	CHECK(nvmm_vcpu_run(&vm->mach, &vm->vcpu) == 0);
	CHECK(vm->vcpu.exit->reason == NVMM_VCPU_EXIT_HALTED);
	rip = get_gpr(vm, NVMM_X64_GPR_RIP);
	CHECK(rip == 0x1001);
	CHECK(get_gpr(vm, NVMM_X64_GPR_R15) == 0x1122334455667788ULL);
	CHECK(get_gpr(vm, NVMM_X64_GPR_RDX) < 4096);	/* a host CPU number */
}

static void
test_exits(struct vm *vm)
{
	set_gpr(vm, NVMM_X64_GPR_RAX, NVMM_FAKE_CMD_IO);
	set_gpr(vm, NVMM_X64_GPR_RBX, 0x3F8);
	set_gpr(vm, NVMM_X64_GPR_RIP, 0x2000);
	CHECK(nvmm_vcpu_run(&vm->mach, &vm->vcpu) == 0);
	CHECK(vm->vcpu.exit->reason == NVMM_VCPU_EXIT_IO);
	CHECK(vm->vcpu.exit->u.io.port == 0x3F8);
	CHECK(vm->vcpu.exit->u.io.in == false);
	CHECK(vm->vcpu.exit->u.io.npc == 0x2002);

	/* An address with RAM behind it. */
	set_gpr(vm, NVMM_X64_GPR_RAX, NVMM_FAKE_CMD_MEM);
	set_gpr(vm, NVMM_X64_GPR_RBX, RAM_SIZE - 4096);
	set_gpr(vm, NVMM_X64_GPR_RCX, 0);
	CHECK(nvmm_vcpu_run(&vm->mach, &vm->vcpu) == 0);
	CHECK(vm->vcpu.exit->reason == NVMM_VCPU_EXIT_HALTED);
	CHECK(get_gpr(vm, NVMM_X64_GPR_RCX) == 1);

	/* An address with nothing behind it. */
	set_gpr(vm, NVMM_X64_GPR_RAX, NVMM_FAKE_CMD_MEM);
	set_gpr(vm, NVMM_X64_GPR_RBX, HOLE_GPA);
	CHECK(nvmm_vcpu_run(&vm->mach, &vm->vcpu) == 0);
	CHECK(vm->vcpu.exit->reason == NVMM_VCPU_EXIT_MEMORY);
	CHECK(vm->vcpu.exit->u.mem.gpa == HOLE_GPA);

	/* Map part of RAM there too, and it resolves; unmap, and it does not. */
	CHECK(nvmm_gpa_map(&vm->mach, (uintptr_t)vm->ram, HOLE_GPA, 1 << 20,
	    PROT_READ | PROT_WRITE) == 0);
	CHECK(nvmm_vcpu_run(&vm->mach, &vm->vcpu) == 0);
	CHECK(vm->vcpu.exit->reason == NVMM_VCPU_EXIT_HALTED);
	CHECK(nvmm_gpa_unmap(&vm->mach, (uintptr_t)vm->ram, HOLE_GPA,
	    1 << 20) == 0);
	set_gpr(vm, NVMM_X64_GPR_RAX, NVMM_FAKE_CMD_MEM);
	CHECK(nvmm_vcpu_run(&vm->mach, &vm->vcpu) == 0);
	CHECK(vm->vcpu.exit->reason == NVMM_VCPU_EXIT_MEMORY);

	/* A command the engine does not know is reported, not swallowed. */
	set_gpr(vm, NVMM_X64_GPR_RAX, 0xBAD);
	CHECK(nvmm_vcpu_run(&vm->mach, &vm->vcpu) == 0);
	CHECK(vm->vcpu.exit->reason == NVMM_VCPU_EXIT_INVALID);
	CHECK(vm->vcpu.exit->u.inv.hwcode == 0xBAD);
}

static void
test_errors(struct vm *vm)
{
	struct nvmm_vcpu dup;
	struct nvmm_machine ghost = { .machid = 9999 };

	/* The same vCPU twice. */
	CHECK(nvmm_vcpu_create(&vm->mach, 0, &dup) == -1);
	/* Guest memory that was never registered with nvmm_hva_map(). */
	CHECK(nvmm_gpa_map(&vm->mach, 0x10000, 1ULL << 30, 4096,
	    PROT_READ) == -1);
	/* A machine that does not exist. */
	CHECK(nvmm_machine_destroy(&ghost) == -1);
	errno = 0;
}

int
main(int argc, char **argv)
{
	const int abandon = (argc > 1 && strcmp(argv[1], "--abandon") == 0);
	struct nvmm_capability cap;
	struct vm a, b;
	pid_t pid;
	int status, round;

	if (nvmm_init() == -1) {
		perror("nvmm_init");
		return 2;
	}

	CHECK(nvmm_capability(&cap) == 0);
	printf("capability: version %u, state %u bytes, comm %u bytes, "
	    "%u machines, %u vcpus\n", cap.version, cap.state_size,
	    cap.comm_size, cap.max_machines, cap.max_vcpus);
	CHECK(cap.version == NVMM_KERN_VERSION);
	CHECK(cap.state_size == sizeof(struct nvmm_x64_state));

	if (abandon) {
		vm_create(&a);
		printf("exiting with a machine, a vCPU and %llu MiB of guest "
		    "RAM still set up\n", RAM_SIZE >> 20);
		return failures ? 1 : 0;
	}

	for (round = 0; round < 3; round++) {
		vm_create(&a);
		test_state(&a);
		test_exits(&a);
		test_errors(&a);

		/* A second machine is independent of the first. */
		vm_create(&b);
		set_gpr(&a, NVMM_X64_GPR_R14, 0xAAAA);
		set_gpr(&b, NVMM_X64_GPR_R14, 0xBBBB);
		CHECK(get_gpr(&a, NVMM_X64_GPR_R14) == 0xAAAA);
		CHECK(get_gpr(&b, NVMM_X64_GPR_R14) == 0xBBBB);
		vm_destroy(&b);

		vm_destroy(&a);
	}

	/*
	 * A child that sets a machine up and dies without tidying. The kernel
	 * has to reclaim the machine and unwire its guest RAM by itself.
	 */
	pid = fork();
	CHECK(pid != -1);
	if (pid == 0) {
		execl(argv[0], argv[0], "--abandon", (char *)NULL);
		_exit(127);
	}
	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	/* And this process can still work afterwards. */
	vm_create(&a);
	test_state(&a);
	vm_destroy(&a);

	printf("%d checks, %d failed\n", checks, failures);
	printf("%s\n", failures ? "FAKE-ENGINE TEST FAILED" :
	    "FAKE-ENGINE TEST PASSED");
	return failures ? 1 : 0;
}
