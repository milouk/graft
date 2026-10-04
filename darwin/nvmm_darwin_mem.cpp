/*
 * nvmm_darwin_mem.cpp — the memory hooks of port/nvmm_port.h, on IOKit.
 *
 * IOKit's memory descriptors are the supported way for an extension to get
 * wired memory whose physical addresses it may know, and to map that memory
 * into a process. Nothing here depends on the layout of a private kernel
 * structure, which is what broke earlier hypervisor ports to macOS.
 *
 * STATUS: compiles and links. Never loaded.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/locks.h>
#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/mman.h>

#include "nvmm_darwin_mem.h"

extern "C" {

#define NVMM_PAGE	4096ULL

/* Mirrors port/nvmm_port.h; that header is C and drags in the whole engine. */
#define PORT_SPACE_KERNEL	0
#define PORT_SPACE_USER		1

struct port_membuf;
int	port_pages_alloc(size_t npages, void **va, uint64_t *pa);
void	port_pages_free(void *va, uint64_t pa, size_t npages);
struct port_membuf *port_membuf_create(size_t size);
void	port_membuf_destroy(struct port_membuf *buf);
uint64_t port_membuf_pa(struct port_membuf *buf, size_t off);
int	port_membuf_map(struct port_membuf *buf, int space, size_t off,
	    size_t size, bool fixed, int prot, uintptr_t *addr, void **cookie);
void	port_membuf_unmap(void *cookie);

} /* extern "C" */

/* -------------------------------------------------------------------------- */
/*
 * Page runs. The caller frees by address, so the descriptor behind each run
 * is remembered here.
 */

struct page_run {
	struct page_run *next;
	void *va;
	IOBufferMemoryDescriptor *desc;
};

static lck_grp_t *mem_lck_grp;
static lck_mtx_t *mem_lock;
static struct page_run *page_runs;

void
nvmm_darwin_mem_init(void)
{
	mem_lck_grp = lck_grp_alloc_init("nvmm-mem", LCK_GRP_ATTR_NULL);
	mem_lock = lck_mtx_alloc_init(mem_lck_grp, LCK_ATTR_NULL);
	page_runs = NULL;
}

void
nvmm_darwin_mem_fini(void)
{
	if (mem_lock != NULL) {
		lck_mtx_free(mem_lock, mem_lck_grp);
		mem_lock = NULL;
	}
	if (mem_lck_grp != NULL) {
		lck_grp_free(mem_lck_grp);
		mem_lck_grp = NULL;
	}
}

int
port_pages_alloc(size_t npages, void **va, uint64_t *pa)
{
	const size_t size = npages * NVMM_PAGE;
	IOBufferMemoryDescriptor *desc;
	struct page_run *run;
	IOByteCount seglen = 0;
	addr64_t phys;

	run = (struct page_run *)IOMalloc(sizeof(*run));
	if (run == NULL)
		return ENOMEM;

	/* Any physical address, page aligned, one contiguous run. */
	desc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous, size,
	    0xFFFFFFFFFFFFF000ULL);
	if (desc == NULL) {
		printf("nvmm: no %zu contiguous page(s) available\n", npages);
		IOFree(run, sizeof(*run));
		return ENOMEM;
	}
	if (desc->prepare() != kIOReturnSuccess) {
		printf("nvmm: cannot wire %zu page(s)\n", npages);
		desc->release();
		IOFree(run, sizeof(*run));
		return ENOMEM;
	}

	phys = desc->getPhysicalSegment(0, &seglen, kIOMemoryMapperNone);
	if (phys == 0 || seglen < size) {
		printf("nvmm: %zu page(s) came back in a %llu-byte run at "
		    "%#llx, wanted %zu bytes\n", npages,
		    (unsigned long long)seglen, (unsigned long long)phys, size);
		desc->complete();
		desc->release();
		IOFree(run, sizeof(*run));
		return ENOMEM;
	}

	run->va = desc->getBytesNoCopy();
	run->desc = desc;
	bzero(run->va, size);

	lck_mtx_lock(mem_lock);
	run->next = page_runs;
	page_runs = run;
	lck_mtx_unlock(mem_lock);

	*va = run->va;
	*pa = phys;
	return 0;
}

void
port_pages_free(void *va, uint64_t pa __unused, size_t npages __unused)
{
	struct page_run **rp, *run;

	lck_mtx_lock(mem_lock);
	for (rp = &page_runs; (run = *rp) != NULL; rp = &run->next) {
		if (run->va == va) {
			*rp = run->next;
			break;
		}
	}
	lck_mtx_unlock(mem_lock);

	if (run == NULL)
		panic("nvmm: freeing pages that were never allocated");

	run->desc->complete();
	run->desc->release();
	IOFree(run, sizeof(*run));
}

/* -------------------------------------------------------------------------- */
/* Memory buffers: guest RAM and the shared communication pages. */

struct port_membuf {
	IOBufferMemoryDescriptor *desc;
	size_t size;
};

struct port_membuf *
port_membuf_create(size_t size)
{
	const size_t rounded = (size + NVMM_PAGE - 1) & ~(NVMM_PAGE - 1);
	struct port_membuf *buf;

	buf = (struct port_membuf *)IOMalloc(sizeof(*buf));
	if (buf == NULL)
		return NULL;

	/*
	 * Kernel-allocated and not pageable, so the pages stay put for as long
	 * as the descriptor exists. kIOMemoryKernelUserShared is what allows
	 * it to be mapped into the emulator as well.
	 */
	buf->desc = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared, rounded, NVMM_PAGE);
	if (buf->desc == NULL) {
		IOFree(buf, sizeof(*buf));
		return NULL;
	}
	if (buf->desc->prepare() != kIOReturnSuccess) {
		buf->desc->release();
		IOFree(buf, sizeof(*buf));
		return NULL;
	}

	bzero(buf->desc->getBytesNoCopy(), rounded);
	buf->size = rounded;
	return buf;
}

void
port_membuf_destroy(struct port_membuf *buf)
{
	buf->desc->complete();
	buf->desc->release();
	IOFree(buf, sizeof(*buf));
}

uint64_t
port_membuf_pa(struct port_membuf *buf, size_t off)
{
	IOByteCount seglen = 0;
	addr64_t phys;

	phys = buf->desc->getPhysicalSegment(off, &seglen, kIOMemoryMapperNone);
	if (phys == 0)
		panic("nvmm: guest memory has no physical page at %#zx", off);
	return phys;
}

int
port_membuf_map(struct port_membuf *buf, int space, size_t off, size_t size,
    bool fixed, int prot, uintptr_t *addr, void **cookie)
{
	IOOptionBits options;
	IOMemoryMap *map;

	if (off + size > buf->size || off + size < off)
		return EINVAL;

	if (space == PORT_SPACE_KERNEL) {
		/* The buffer already lives in the kernel's address space. */
		if (fixed)
			return EINVAL;
		*addr = (uintptr_t)buf->desc->getBytesNoCopy() + off;
		*cookie = NULL;
		return 0;
	}

	options = fixed ? kIOMapOverwrite : kIOMapAnywhere;
	if ((prot & PROT_WRITE) == 0)
		options |= kIOMapReadOnly;

	map = buf->desc->createMappingInTask(current_task(),
	    fixed ? (mach_vm_address_t)*addr : 0, options, off, size);
	if (map == NULL)
		return ENOMEM;

	*addr = (uintptr_t)map->getAddress();
	*cookie = map;
	return 0;
}

void
port_membuf_unmap(void *cookie)
{
	IOMemoryMap *map = (IOMemoryMap *)cookie;

	/* Releasing the last reference to a map removes the mapping. */
	if (map != NULL)
		map->release();
}

/* -------------------------------------------------------------------------- */
/* Sleep and wake. */

static IONotifier *power_notifier;

static IOReturn
nvmm_power_handler(void *target __unused, void *refCon __unused,
    UInt32 messageType, IOService *provider __unused,
    void *messageArgument __unused, vm_size_t argSize __unused)
{
	switch (messageType) {
	case kIOMessageSystemWillSleep:
		nvmm_darwin_will_sleep();
		break;
	case kIOMessageSystemHasPoweredOn:
		nvmm_darwin_did_wake();
		break;
	default:
		break;
	}
	return kIOReturnSuccess;
}

void
nvmm_darwin_power_register(void)
{
	power_notifier = registerPrioritySleepWakeInterest(nvmm_power_handler,
	    NULL, NULL);
}

void
nvmm_darwin_power_unregister(void)
{
	if (power_notifier != NULL) {
		power_notifier->remove();
		power_notifier = NULL;
	}
}
