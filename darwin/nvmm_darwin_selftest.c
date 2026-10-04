/*
 * nvmm_darwin_selftest.c — exercise the macOS glue without the SVM engine.
 *
 * The engine needs an AMD CPU. The glue under it does not: wired memory,
 * mappings into a process, nested page tables built from real physical
 * addresses, locks, preemption control, cross-CPU calls, FPU state parking.
 * This builds into a separate kext (NVMM_DARWIN_SELFTEST) that skips the
 * engine entirely and tests those pieces on whatever x86_64 Mac it is loaded
 * on, so that they have run somewhere before they meet a real guest.
 *
 * It creates /dev/nvmm-selftest; test/darwin/selftest.c drives it.
 */

#include "nvmm.h"
#include "nvmm_internal.h"
#include "x86/nvmm_x86_internal.h"

#include <sys/conf.h>
#include <sys/proc.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <IOKit/IOLib.h>

#include "nvmm_selftest_ioctl.h"
#include "nvmm_darwin_selftest.h"
#include "nvmm_darwin_mem.h"

extern int cpu_number(void);
extern unsigned int real_ncpus;

volatile unsigned int nvmm_selftest_sleeps;
volatile unsigned int nvmm_selftest_wakes;
/* How often devfs asked for a minor: once per lookup, not once per open. */
static volatile unsigned int nvmm_selftest_lookups;

/* -------------------------------------------------------------------------- */
/* A tiny reporting harness. */

struct st {
	struct nvmm_selftest_run *out;
	size_t used;
};

static void
st_log(struct st *st, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (st->used >= NVMM_SELFTEST_LOGSZ - 1)
		return;
	va_start(ap, fmt);
	n = vsnprintf(st->out->log + st->used, NVMM_SELFTEST_LOGSZ - st->used,
	    fmt, ap);
	va_end(ap);
	if (n > 0)
		st->used += (size_t)n;
	if (st->used > NVMM_SELFTEST_LOGSZ - 1)
		st->used = NVMM_SELFTEST_LOGSZ - 1;
}

#define ST_CHECK(st, cond)						\
	do {								\
		if (cond) {						\
			(st)->out->passed++;				\
		} else {						\
			(st)->out->failed++;				\
			st_log((st), "FAIL %s:%d: %s\n", __func__,	\
			    __LINE__, #cond);				\
		}							\
	} while (0)

/* -------------------------------------------------------------------------- */

static void
st_memory(struct st *st)
{
	void *p, *va;
	uint64_t pa;
	uint8_t *b;
	size_t i;
	bool zero;

	p = port_malloc(123);
	ST_CHECK(st, p != NULL);
	if (p != NULL) {
		memset(p, 0x5A, 123);
		port_free(p, 123);
	}

	/* One page: zeroed, aligned, with a plausible physical address. */
	va = NULL;
	pa = 0;
	ST_CHECK(st, port_pages_alloc(1, &va, &pa) == 0);
	if (va != NULL) {
		b = va;
		zero = true;
		for (i = 0; i < 4096; i++)
			zero = zero && (b[i] == 0);
		ST_CHECK(st, zero);
		ST_CHECK(st, ((uintptr_t)va & 4095) == 0);
		ST_CHECK(st, pa != 0 && (pa & 4095) == 0);
		ST_CHECK(st, pa < (1ULL << 46));
		memset(va, 0xC3, 4096);
		ST_CHECK(st, b[4095] == 0xC3);
		port_pages_free(va, pa, 1);
	}

	/* Several pages in one physically contiguous run, as a VMCB needs. */
	va = NULL;
	i = (size_t)port_pages_alloc(4, &va, &pa);
	if (i != 0)
		st_log(st, "memory: 4 contiguous pages refused, error %d "
		    "(see the kernel log)\n", (int)i);
	ST_CHECK(st, i == 0);
	if (va != NULL) {
		b = va;
		memset(va, 0x11, 4 * 4096);
		ST_CHECK(st, b[4 * 4096 - 1] == 0x11);
		ST_CHECK(st, (pa & 4095) == 0);
		port_pages_free(va, pa, 4);
	}
	st_log(st, "memory: single page pa %#llx\n", (unsigned long long)pa);

	/*
	 * The sizes the engine asks for per vCPU (VMCB 1, MSR bitmap 2, I/O
	 * bitmap 3 pages), over and over. Contiguity that only holds while
	 * memory is fresh after boot is not good enough.
	 */
	{
		unsigned int round, ok = 0, total = 0;
		size_t n;

		for (round = 0; round < 64; round++) {
			for (n = 1; n <= 4; n++) {
				total++;
				va = NULL;
				if (port_pages_alloc(n, &va, &pa) == 0) {
					ok++;
					memset(va, 0x77, n * 4096);
					port_pages_free(va, pa, n);
				}
			}
		}
		ST_CHECK(st, ok == total);
		st_log(st, "memory: %u of %u contiguous allocations ok\n", ok,
		    total);
	}
}

static void
st_locks(struct st *st)
{
	os_mtx_t m;
	os_rwl_t r;

	os_mtx_init(&m);
	ST_CHECK(st, !os_mtx_owned(&m));
	os_mtx_lock(&m);
	ST_CHECK(st, os_mtx_owned(&m));
	os_mtx_unlock(&m);
	ST_CHECK(st, !os_mtx_owned(&m));
	os_mtx_destroy(&m);

	os_rwl_init(&r);
	os_rwl_rlock(&r);
	ST_CHECK(st, !os_rwl_wheld(&r));
	os_rwl_unlock(&r);
	os_rwl_wlock(&r);
	ST_CHECK(st, os_rwl_wheld(&r));
	os_rwl_unlock(&r);
	ST_CHECK(st, !os_rwl_wheld(&r));
	os_rwl_destroy(&r);
}

static void
st_preempt(struct st *st)
{
	unsigned int cpu0, cpu1, i;
	volatile unsigned long spin = 0;

	ST_CHECK(st, !os_preempt_disabled());

	os_preempt_disable();
	ST_CHECK(st, os_preempt_disabled());
	cpu0 = os_curcpu_number();

	/* Nested, as the engine does around FPU set-up. */
	os_preempt_disable();
	ST_CHECK(st, os_preempt_disabled());
	os_preempt_enable();
	ST_CHECK(st, os_preempt_disabled());

	/* Burn some time: with preemption off the thread must not migrate. */
	for (i = 0; i < 20000000; i++)
		spin += i;
	cpu1 = os_curcpu_number();
	ST_CHECK(st, cpu0 == cpu1);

	os_preempt_enable();
	ST_CHECK(st, !os_preempt_disabled());
	st_log(st, "preempt: held on cpu %u\n", cpu0);
}

static volatile unsigned int st_ipi_count;
static volatile uint64_t st_ipi_mask;

static OS_IPI_FUNC(st_ipi_func)
{
	(void)arg;
	os_atomic_inc_uint(&st_ipi_count);
	(void)__sync_fetch_and_or(&st_ipi_mask, 1ULL << (cpu_number() & 63));
}

static void
st_ipi(struct st *st)
{
	const unsigned int ncpus = port_ncpus();
	uint64_t want;

	st_ipi_count = 0;
	st_ipi_mask = 0;
	os_ipi_broadcast(st_ipi_func, NULL);

	/* Once on every CPU, and all of them finished before it returned. */
	ST_CHECK(st, st_ipi_count == ncpus);
	want = (ncpus >= 64) ? ~0ULL : ((1ULL << ncpus) - 1);
	ST_CHECK(st, st_ipi_mask == want);

	st_ipi_count = 0;
	os_ipi_kickall();
	os_ipi_broadcast(st_ipi_func, NULL);
	ST_CHECK(st, st_ipi_count == ncpus);
	st_log(st, "ipi: %u cpus, mask %#llx\n", ncpus,
	    (unsigned long long)st_ipi_mask);
}

static void
st_fpu(struct st *st)
{
	const uint64_t pattern = 0x0123456789ABCDEFULL;
	const uint64_t junk = 0xFFFFFFFFFFFFFFFFULL;
	uint64_t after = 0;
	uint64_t dr7_before, dr7_after;

	ST_CHECK(st, port_fpu_mxcsr_mask != 0);

	/*
	 * What the engine does around a guest: park the host's FPU registers,
	 * let someone else scribble on them, bring them back.
	 */
	os_preempt_disable();
	__asm volatile ("movq %0,%%xmm0" : : "r" (pattern));
	x86_curthread_save_fpu();
	__asm volatile ("movq %0,%%xmm0" : : "r" (junk));
	x86_curthread_restore_fpu();
	__asm volatile ("movq %%xmm0,%0" : "=r" (after));
	os_preempt_enable();
	ST_CHECK(st, after == pattern);

	os_preempt_disable();
	dr7_before = x86_get_dr7();
	x86_curthread_save_dbregs();
	x86_set_dr7(0);
	x86_curthread_restore_dbregs();
	dr7_after = x86_get_dr7();
	os_preempt_enable();
	ST_CHECK(st, dr7_before == dr7_after);

	st_log(st, "fpu: xcr0 %#llx mxcsr mask %#x\n",
	    (unsigned long long)port_xsave_features, port_fpu_mxcsr_mask);
}

/* Guest memory, end to end, minus the guest. */
static void
st_guest_memory(struct st *st)
{
	const vsize_t size = 16 * 4096;
	const vaddr_t gpa = 0x100000;
	os_vmspace_t *vs;
	os_vmmap_t *gmap;
	os_vmobj_t *obj;
	vaddr_t kva = 0, g;
	paddr_t hpa, root;
	uint8_t *k;
	unsigned int i, ok;
	uint64_t gen0;
	int error;

	vs = os_vmspace_create(0, 1ULL << 40);
	ST_CHECK(st, vs != NULL);
	gmap = os_vmspace_get_vmmap(vs);
	root = os_vmspace_pdirpa(vs);
	ST_CHECK(st, root != 0 && (root & 4095) == 0);

	obj = os_vmobj_create(size);
	ST_CHECK(st, obj != NULL);
	if (obj == NULL)
		goto out;

	error = os_vmobj_map_kern(os_kernel_map, &kva, size, obj, 0, true,
	    false, true, PROT_READ | PROT_WRITE, PROT_READ | PROT_WRITE);
	ST_CHECK(st, error == 0 && kva != 0);
	if (error != 0)
		goto out_obj;
	k = (uint8_t *)(uintptr_t)kva;
	for (i = 0; i < size; i++)
		k[i] = (uint8_t)(i >> 12);

	g = gpa;
	error = os_vmobj_map_user(gmap, &g, size, obj, 0, false, true, true,
	    PROT_READ | PROT_WRITE | PROT_EXEC,
	    PROT_READ | PROT_WRITE | PROT_EXEC);
	ST_CHECK(st, error == 0);

	/*
	 * Every guest page must translate to a distinct, page-aligned physical
	 * address, and the tables must say nothing about its neighbours.
	 */
	ok = 0;
	for (i = 0; i < 16; i++) {
		if (port_vm_guest_lookup(vs, gpa + i * 4096ULL, &hpa) &&
		    hpa != 0 && (hpa & 4095) == 0)
			ok++;
	}
	ST_CHECK(st, ok == 16);
	ST_CHECK(st, !port_vm_guest_lookup(vs, gpa - 4096, &hpa));
	ST_CHECK(st, !port_vm_guest_lookup(vs, gpa + size, &hpa));

	/* A hole in the middle, and the generation moving on because of it. */
	gen0 = os_vmspace_gen(vs);
	os_vmobj_unmap(gmap, gpa + 4 * 4096, gpa + 8 * 4096, false);
	ST_CHECK(st, os_vmspace_gen(vs) != gen0);
	ST_CHECK(st, port_vm_guest_lookup(vs, gpa + 3 * 4096, &hpa));
	ST_CHECK(st, !port_vm_guest_lookup(vs, gpa + 4 * 4096, &hpa));
	ST_CHECK(st, !port_vm_guest_lookup(vs, gpa + 7 * 4096, &hpa));
	ST_CHECK(st, port_vm_guest_lookup(vs, gpa + 8 * 4096, &hpa));

	os_vmobj_unmap(gmap, gpa, gpa + size, false);
	ST_CHECK(st, !port_vm_guest_lookup(vs, gpa, &hpa));

	os_vmobj_unmap(os_kernel_map, kva, kva + size, true);
out_obj:
	os_vmobj_rel(obj);
out:
	os_vmspace_destroy(vs);
	st_log(st, "guest memory: table root pa %#llx\n",
	    (unsigned long long)root);
}

/*
 * A buffer big enough to be built from 2M runs. However many runs the system
 * gave, each must have gone into the guest's tables as one 2M page, the whole
 * buffer must translate, and taking a 4K page out must leave its neighbours
 * pointing where they did.
 */
static void
st_guest_large(struct st *st)
{
	const vsize_t large = 2ULL << 20;
	const vsize_t size = 4 * large + 16 * 4096;
	const vaddr_t gpa = 2 * large;
	const unsigned long nlarge0 = port_vm_nlarge;
	os_vmspace_t *vs;
	os_vmmap_t *gmap;
	os_vmobj_t *obj;
	vaddr_t kva = 0, g;
	paddr_t hpa, base, before;
	unsigned int got, i, ok, run;
	uint8_t *k;
	int error;

	vs = os_vmspace_create(0, 1ULL << 40);
	ST_CHECK(st, vs != NULL);
	if (vs == NULL)
		return;
	gmap = os_vmspace_get_vmmap(vs);

	memset(&nvmm_darwin_mem_stats, 0, sizeof(nvmm_darwin_mem_stats));
	obj = os_vmobj_create(size);
	ST_CHECK(st, obj != NULL);
	if (obj == NULL)
		goto out;
	got = nvmm_darwin_mem_stats.last_large;
	st_log(st, "guest memory: %u of %u 2M runs, found in %u us\n", got,
	    nvmm_darwin_mem_stats.last_wanted,
	    nvmm_darwin_mem_stats.last_usec);
	ST_CHECK(st, got <= 4);

	/* The kernel's view of a buffer made of parts is one piece. */
	error = os_vmobj_map_kern(os_kernel_map, &kva, size, obj, 0, true,
	    false, true, PROT_READ | PROT_WRITE, PROT_READ | PROT_WRITE);
	ST_CHECK(st, error == 0 && kva != 0);
	if (error != 0)
		goto out_obj;
	k = (uint8_t *)(uintptr_t)kva;
	ok = 1;
	for (i = 0; i < size; i += 4096)
		ok = ok && (k[i] == 0);
	ST_CHECK(st, ok);
	for (i = 0; i < size; i += 4096)
		k[i] = (uint8_t)(i >> 12) ^ 0x3C;
	ok = 1;
	for (i = 0; i < size; i += 4096)
		ok = ok && (k[i] == (uint8_t)((i >> 12) ^ 0x3C));
	ST_CHECK(st, ok);

	g = gpa;
	error = os_vmobj_map_user(gmap, &g, size, obj, 0, false, true, true,
	    PROT_READ | PROT_WRITE | PROT_EXEC,
	    PROT_READ | PROT_WRITE | PROT_EXEC);
	ST_CHECK(st, error == 0);
	if (error != 0)
		goto out_kern;

	/* The runs come first in the buffer, and each became a 2M page. */
	ST_CHECK(st, port_vm_nlarge - nlarge0 == got);
	for (run = 0; run < got; run++) {
		base = 0;
		ok = port_vm_guest_lookup(vs, gpa + run * large, &base) &&
		    base != 0 && (base & (large - 1)) == 0;
		for (i = 1; ok && i < 512; i++) {
			ok = port_vm_guest_lookup(vs,
			    gpa + run * large + i * 4096ULL, &hpa) &&
			    hpa == base + i * 4096ULL;
		}
		ST_CHECK(st, ok);
	}

	/* Every page translates, 2M or not. */
	ok = 0;
	for (i = 0; i < size / 4096; i++) {
		if (port_vm_guest_lookup(vs, gpa + i * 4096ULL, &hpa) &&
		    hpa != 0 && (hpa & 4095) == 0)
			ok++;
	}
	ST_CHECK(st, ok == size / 4096);
	ST_CHECK(st, !port_vm_guest_lookup(vs, gpa - 4096, &hpa));
	ST_CHECK(st, !port_vm_guest_lookup(vs, gpa + size, &hpa));

	/* One 4K page out of the first 2M: a split, if it was a 2M page. */
	before = 0;
	ST_CHECK(st, port_vm_guest_lookup(vs, gpa + 101 * 4096ULL, &before));
	os_vmobj_unmap(gmap, gpa + 100 * 4096ULL, gpa + 101 * 4096ULL, false);
	ST_CHECK(st, !port_vm_guest_lookup(vs, gpa + 100 * 4096ULL, &hpa));
	ST_CHECK(st, port_vm_guest_lookup(vs, gpa + 99 * 4096ULL, &hpa));
	ST_CHECK(st, port_vm_guest_lookup(vs, gpa + 101 * 4096ULL, &hpa) &&
	    hpa == before);

	os_vmobj_unmap(gmap, gpa, gpa + size, false);
	ST_CHECK(st, !port_vm_guest_lookup(vs, gpa, &hpa));
	ST_CHECK(st, !port_vm_guest_lookup(vs, gpa + size - 4096, &hpa));
out_kern:
	os_vmobj_unmap(os_kernel_map, kva, kva + size, true);
out_obj:
	os_vmobj_rel(obj);
out:
	os_vmspace_destroy(vs);
}

static void
st_misc(struct st *st)
{
	ST_CHECK(st, port_time() > 1600000000);
	ST_CHECK(st, port_curpid() > 0);
	ST_CHECK(st, port_ncpus() >= 1 && port_ncpus() <= OS_MAXCPUS);
	ST_CHECK(st, port_curcpu() < port_ncpus());
}

static void
selftest_run(struct nvmm_selftest_run *out)
{
	struct st st = { out, 0 };

	memset(out, 0, sizeof(*out));
	out->ncpus = port_ncpus();

	st_misc(&st);
	st_memory(&st);
	st_locks(&st);
	st_preempt(&st);
	st_ipi(&st);
	st_fpu(&st);
	st_guest_memory(&st);
	st_guest_large(&st);

	out->sleeps = nvmm_selftest_sleeps;
	out->wakes = nvmm_selftest_wakes;
	st_log(&st, "device: %u lookups so far\n", nvmm_selftest_lookups);
}

/* -------------------------------------------------------------------------- */
/* The device. One mapping per open, like one machine per owner. */

#define ST_MAXOPEN	16

struct st_open {
	bool used;
	int pid;
	os_vmobj_t *obj;
	vaddr_t uva;
	vaddr_t kva;
	vsize_t size;
};

static struct st_open st_opens[ST_MAXOPEN];
static os_mtx_t st_dev_lock;
static int st_major = -1;
static void *st_devnode;

static int
st_clone(dev_t dev __unused, int action)
{
	int i;

	if (action != DEVFS_CLONE_ALLOC)
		return 0;

	/*
	 * Only a suggestion; see the comment on the device in nvmm_darwin.c.
	 * Never reserve here and never return -1.
	 */
	for (i = 0; i < ST_MAXOPEN; i++) {
		if (!st_opens[i].used)
			break;
	}
	nvmm_selftest_lookups++;
	return i;
}

static void
st_drop_mapping(struct st_open *o)
{
	if (o->obj == NULL)
		return;
	if (o->kva != 0)
		os_vmobj_unmap(os_kernel_map, o->kva, o->kva + o->size, true);
	/*
	 * Deliberately not unmapping the user side here: this is the path a
	 * process takes when it exits without tidying up, and it is the
	 * close-time sweep that has to unwire the memory.
	 */
	port_vm_cleanup_pid(o->pid);
	os_vmobj_rel(o->obj);
	o->obj = NULL;
	o->kva = o->uva = 0;
	o->size = 0;
}

static int
st_open(dev_t dev, int flags __unused, int devtype __unused, proc_t p)
{
	const int unit = minor(dev);
	int error = 0;

	if (unit < 0 || unit > ST_MAXOPEN)
		return ENXIO;
	if (unit == ST_MAXOPEN)
		return EBUSY;

	os_mtx_lock(&st_dev_lock);
	if (st_opens[unit].used) {
		error = EBUSY;
	} else {
		memset(&st_opens[unit], 0, sizeof(st_opens[unit]));
		st_opens[unit].used = true;
		st_opens[unit].pid = proc_pid(p);
	}
	os_mtx_unlock(&st_dev_lock);
	return error;
}

static int
st_close(dev_t dev, int flags __unused, int devtype __unused,
    proc_t p __unused)
{
	const int unit = minor(dev);

	if (unit < 0 || unit >= ST_MAXOPEN || !st_opens[unit].used)
		return 0;
	st_drop_mapping(&st_opens[unit]);
	os_mtx_lock(&st_dev_lock);
	st_opens[unit].used = false;
	os_mtx_unlock(&st_dev_lock);
	return 0;
}

static int
st_do_map(struct st_open *o, struct nvmm_selftest_map *args)
{
	vaddr_t uva, kva = 0;
	uint8_t *k;
	uint64_t i;
	int error;

	if (o->obj != NULL)
		return EBUSY;
	if (args->size == 0 || (args->size & 4095) != 0 ||
	    args->size > (1ULL << 30))
		return EINVAL;
	if (args->fixed && (args->addr & 4095) != 0)
		return EINVAL;

	memset(&nvmm_darwin_mem_stats, 0, sizeof(nvmm_darwin_mem_stats));
	o->obj = os_vmobj_create(args->size);
	if (o->obj == NULL)
		return ENOMEM;
	args->large = nvmm_darwin_mem_stats.last_large;
	args->large_wanted = nvmm_darwin_mem_stats.last_wanted;
	args->large_usec = nvmm_darwin_mem_stats.last_usec;

	error = os_vmobj_map_kern(os_kernel_map, &kva, args->size, o->obj, 0,
	    true, false, true, PROT_READ | PROT_WRITE, PROT_READ | PROT_WRITE);
	if (error != 0)
		goto fail;

	uva = args->fixed ? args->addr : 0;
	error = os_vmobj_map_user(os_curproc_map, &uva, args->size, o->obj, 0,
	    false, args->fixed != 0, true, PROT_READ | PROT_WRITE,
	    PROT_READ | PROT_WRITE);
	if (error != 0) {
		os_vmobj_unmap(os_kernel_map, kva, kva + args->size, true);
		goto fail;
	}

	k = (uint8_t *)(uintptr_t)kva;
	for (i = 0; i < args->size; i++)
		k[i] = nvmm_selftest_pattern(args->seed, i);

	o->kva = kva;
	o->uva = uva;
	o->size = args->size;
	args->addr = uva;
	return 0;

fail:
	os_vmobj_rel(o->obj);
	o->obj = NULL;
	return error;
}

static int
st_do_verify(struct st_open *o, struct nvmm_selftest_verify *args)
{
	const uint8_t *k = (const uint8_t *)(uintptr_t)o->kva;
	uint64_t i;

	if (o->obj == NULL)
		return ENOENT;

	/* The process was asked to write the pattern back inverted. */
	args->ok = 1;
	args->first_bad = 0;
	for (i = 0; i < o->size; i++) {
		if (k[i] != (uint8_t)~nvmm_selftest_pattern(args->seed, i)) {
			args->ok = 0;
			args->first_bad = i;
			break;
		}
	}
	return 0;
}

static int
st_ioctl(dev_t dev, u_long cmd, caddr_t data, int fflag __unused,
    proc_t p __unused)
{
	const int unit = minor(dev);
	struct st_open *o;

	if (unit < 0 || unit >= ST_MAXOPEN || !st_opens[unit].used)
		return ENXIO;
	o = &st_opens[unit];

	switch (cmd) {
	case NVMM_SELFTEST_IOC_RUN:
		selftest_run((struct nvmm_selftest_run *)(void *)data);
		return 0;
	case NVMM_SELFTEST_IOC_MAP:
		return st_do_map(o, (struct nvmm_selftest_map *)(void *)data);
	case NVMM_SELFTEST_IOC_VERIFY:
		return st_do_verify(o,
		    (struct nvmm_selftest_verify *)(void *)data);
	case NVMM_SELFTEST_IOC_UNMAP:
		if (o->obj == NULL)
			return ENOENT;
		/* The tidy path: the process unmaps what it mapped. */
		os_vmobj_unmap(os_curproc_map, o->uva, o->uva + o->size, false);
		st_drop_mapping(o);
		return 0;
	default:
		return ENOTTY;
	}
}

static const struct cdevsw st_cdevsw = {
	.d_open = st_open,
	.d_close = st_close,
	.d_read = eno_rdwrt,
	.d_write = eno_rdwrt,
	.d_ioctl = st_ioctl,
	.d_stop = eno_stop,
	.d_reset = eno_reset,
	.d_ttys = NULL,
	.d_select = eno_select,
	.d_mmap = eno_mmap,
	.d_strategy = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type = 0,
};

int
nvmm_selftest_attach(void)
{
	os_mtx_init(&st_dev_lock);
	memset(st_opens, 0, sizeof(st_opens));

	st_major = cdevsw_add(-1, &st_cdevsw);
	if (st_major < 0) {
		os_mtx_destroy(&st_dev_lock);
		return ENXIO;
	}
	st_devnode = devfs_make_node_clone(makedev(st_major, 0), DEVFS_CHAR,
	    UID_ROOT, GID_WHEEL, 0600, st_clone, "nvmm-selftest");
	if (st_devnode == NULL) {
		(void)cdevsw_remove(st_major, &st_cdevsw);
		st_major = -1;
		os_mtx_destroy(&st_dev_lock);
		return ENXIO;
	}
	return 0;
}

int
nvmm_selftest_detach(void)
{
	int i;

	for (i = 0; i < ST_MAXOPEN; i++) {
		if (st_opens[i].used)
			return EBUSY;
	}
	devfs_remove(st_devnode);
	(void)cdevsw_remove(st_major, &st_cdevsw);
	st_devnode = NULL;
	st_major = -1;
	os_mtx_destroy(&st_dev_lock);
	return 0;
}
