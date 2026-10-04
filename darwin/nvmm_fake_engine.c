/*
 * nvmm_fake_engine.c — a stand-in for the SVM engine.
 *
 * The real engine needs an AMD CPU. Everything around it does not: the ioctl
 * interface, machines and vCPUs, the comm page shared with the process, guest
 * memory maps. This backend plugs into the same nvmm_impl slot and "runs" a
 * guest by obeying a command in RAX, so that libnvmm and the kernel core can
 * be exercised end to end on any x86_64 Mac.
 *
 * Built only into the self-test kext (NVMM_FAKE_ENGINE). It executes no guest
 * code and touches no virtualization hardware.
 */

#include "nvmm.h"
#include "nvmm_internal.h"
#include "x86/nvmm_x86_internal.h"
#include "nvmm_fake_engine.h"

struct fake_cpudata {
	struct nvmm_x64_state state;
	uint64_t runs;
};

static bool
fake_ident(void)
{
	return true;
}

static void
fake_init(void)
{
}

static void
fake_fini(void)
{
}

static void
fake_capability(struct nvmm_capability *cap)
{
	cap->arch.mach_conf_support = 0;
	cap->arch.vcpu_conf_support = 0;
	cap->arch.xcr0_mask = 0;
	cap->arch.mxcsr_mask = x86_fpu_mxcsr_mask;
	cap->arch.conf_cpuid_maxops = 0;
}

static void
fake_machine_create(struct nvmm_machine *mach)
{
	mach->machdata = NULL;
}

static void
fake_machine_destroy(struct nvmm_machine *mach __unused)
{
}

static int
fake_machine_configure(struct nvmm_machine *mach __unused,
    uint64_t op __unused, void *data __unused)
{
	return EINVAL;
}

static int
fake_vcpu_create(struct nvmm_machine *mach __unused, struct nvmm_cpu *vcpu)
{
	struct fake_cpudata *cpudata;

	cpudata = os_mem_zalloc(sizeof(*cpudata));
	if (cpudata == NULL)
		return ENOMEM;
	memcpy(&cpudata->state, &nvmm_x86_reset_state, sizeof(cpudata->state));
	vcpu->cpudata = cpudata;
	return 0;
}

static void
fake_vcpu_destroy(struct nvmm_machine *mach __unused, struct nvmm_cpu *vcpu)
{
	os_mem_free(vcpu->cpudata, sizeof(struct fake_cpudata));
	vcpu->cpudata = NULL;
}

static int
fake_vcpu_configure(struct nvmm_cpu *vcpu __unused, uint64_t op __unused,
    void *data __unused)
{
	return EINVAL;
}

#define FAKE_COPY(flag, field)						\
	do {								\
		if (flags & (flag))					\
			memcpy(&dst->field, &src->field, sizeof(dst->field)); \
	} while (0)

static void
fake_copy_state(struct nvmm_x64_state *dst, const struct nvmm_x64_state *src,
    uint64_t flags)
{
	FAKE_COPY(NVMM_X64_STATE_SEGS, segs);
	FAKE_COPY(NVMM_X64_STATE_GPRS, gprs);
	FAKE_COPY(NVMM_X64_STATE_CRS, crs);
	FAKE_COPY(NVMM_X64_STATE_DRS, drs);
	FAKE_COPY(NVMM_X64_STATE_MSRS, msrs);
	FAKE_COPY(NVMM_X64_STATE_INTR, intr);
	FAKE_COPY(NVMM_X64_STATE_FPU, fpu);
}

/* Same contract as the real engine: honour state_wanted, then mark cached. */
static void
fake_vcpu_setstate(struct nvmm_cpu *vcpu)
{
	struct nvmm_comm_page *comm = vcpu->comm;
	struct fake_cpudata *cpudata = vcpu->cpudata;
	const uint64_t flags = comm->state_wanted;

	fake_copy_state(&cpudata->state, &comm->state, flags);
	comm->state_wanted = 0;
	comm->state_cached |= flags;
}

static void
fake_vcpu_getstate(struct nvmm_cpu *vcpu)
{
	struct nvmm_comm_page *comm = vcpu->comm;
	struct fake_cpudata *cpudata = vcpu->cpudata;
	const uint64_t flags = comm->state_wanted;

	fake_copy_state(&comm->state, &cpudata->state, flags);
	comm->state_wanted = 0;
	comm->state_cached |= flags;
}

static int
fake_vcpu_run(struct nvmm_machine *mach, struct nvmm_cpu *vcpu,
    struct nvmm_vcpu_exit *exit)
{
	struct nvmm_comm_page *comm = vcpu->comm;
	struct fake_cpudata *cpudata = vcpu->cpudata;
	uint64_t *gprs = cpudata->state.gprs;
	paddr_t hpa;
	int hcpu;

	/* Take what the process changed, as the real engine does on entry. */
	comm->state_wanted = comm->state_commit;
	comm->state_commit = 0;
	fake_vcpu_setstate(vcpu);
	comm->state_cached = 0;

	/* The real loop runs with preemption off; keep that path honest. */
	os_preempt_disable();
	hcpu = (int)os_curcpu_number();
	vcpu->hcpu_last = hcpu;
	os_preempt_enable();

	cpudata->runs++;

	switch (gprs[NVMM_X64_GPR_RAX]) {
	case NVMM_FAKE_CMD_IO:
		exit->reason = NVMM_VCPU_EXIT_IO;
		exit->u.io.in = false;
		exit->u.io.port = (uint16_t)gprs[NVMM_X64_GPR_RBX];
		exit->u.io.seg = -1;
		exit->u.io.address_size = 2;
		exit->u.io.operand_size = 1;
		exit->u.io.rep = false;
		exit->u.io.str = false;
		exit->u.io.npc = gprs[NVMM_X64_GPR_RIP] + 2;
		break;

	case NVMM_FAKE_CMD_MEM:
		if (port_vm_guest_lookup(mach->vm, gprs[NVMM_X64_GPR_RBX] &
		    ~NVMM_PAGE_MASK, &hpa)) {
			gprs[NVMM_X64_GPR_RCX] = 1;
			gprs[NVMM_X64_GPR_RIP] += 1;
			exit->reason = NVMM_VCPU_EXIT_HALTED;
		} else {
			exit->reason = NVMM_VCPU_EXIT_MEMORY;
			exit->u.mem.prot = PROT_WRITE;
			exit->u.mem.gpa = gprs[NVMM_X64_GPR_RBX];
			exit->u.mem.inst_len = 0;
		}
		break;

	case NVMM_FAKE_CMD_HLT:
		gprs[NVMM_X64_GPR_RDX] = (uint64_t)hcpu;
		gprs[NVMM_X64_GPR_RIP] += 1;
		exit->reason = NVMM_VCPU_EXIT_HALTED;
		break;

	default:
		exit->reason = NVMM_VCPU_EXIT_INVALID;
		exit->u.inv.hwcode = gprs[NVMM_X64_GPR_RAX];
		break;
	}

	exit->exitstate.rflags = gprs[NVMM_X64_GPR_RFLAGS];
	exit->exitstate.cr8 = 0;
	exit->exitstate.int_shadow = 0;
	exit->exitstate.int_window_exiting = 0;
	exit->exitstate.nmi_window_exiting = 0;
	exit->exitstate.evt_pending = 0;
	return 0;
}

const struct nvmm_impl nvmm_x86_fake = {
	.name = "x86-fake (no guest code is executed)",
	.ident = fake_ident,
	.init = fake_init,
	.fini = fake_fini,
	.capability = fake_capability,
	.mach_conf_max = 0,
	.mach_conf_sizes = NULL,
	.vcpu_conf_max = 0,
	.vcpu_conf_sizes = NULL,
	.state_size = sizeof(struct nvmm_x64_state),
	.machine_create = fake_machine_create,
	.machine_destroy = fake_machine_destroy,
	.machine_configure = fake_machine_configure,
	.vcpu_create = fake_vcpu_create,
	.vcpu_destroy = fake_vcpu_destroy,
	.vcpu_configure = fake_vcpu_configure,
	.vcpu_setstate = fake_vcpu_setstate,
	.vcpu_getstate = fake_vcpu_getstate,
	.vcpu_run = fake_vcpu_run
};
