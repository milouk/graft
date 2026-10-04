/*
 * nvmm_darwin_mem.cpp — the memory hooks of port/nvmm_port.h, on IOKit.
 *
 * IOKit's memory descriptors are the supported way for an extension to get
 * wired memory whose physical addresses it may know, and to map that memory
 * into a process. Nothing here depends on the layout of a private kernel
 * structure, which is what broke earlier hypervisor ports to macOS.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOMultiMemoryDescriptor.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/locks.h>
#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/mman.h>

#include "nvmm_darwin_mem.h"

extern "C" {

#define NVMM_PAGE	4096ULL
#define NVMM_LARGE	(2ULL << 20)

/*
 * Stop asking for 2M runs for one buffer once this much time has gone into
 * them. Finding contiguous memory on a machine that has been up for a while
 * can be slow, and a guest that starts with 4K pages beats one that starts
 * late.
 */
#define NVMM_LARGE_BUDGET_NS	(2000ULL * 1000 * 1000)

struct nvmm_darwin_mem_stats nvmm_darwin_mem_stats;

/* Mirrors port/nvmm_port.h; that header is C and drags in the whole engine. */
#define PORT_SPACE_KERNEL	0
#define PORT_SPACE_USER		1

struct port_membuf;
int	port_pages_alloc(size_t npages, void **va, uint64_t *pa);
void	port_pages_free(void *va, uint64_t pa, size_t npages);
struct port_membuf *port_membuf_create(size_t size);
struct port_membuf *port_membuf_borrow(uintptr_t uva, size_t size);
int	port_membuf_populate(struct port_membuf *buf, size_t off, size_t len);
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

	/*
	 * Any physical address, page aligned, one contiguous run.
	 *
	 * The flag has to be the HOST one. On a machine with an I/O mapper
	 * (VT-d), kIOMemoryPhysicallyContiguous only promises a run that is
	 * contiguous as a device sees it through the mapper, and the pages
	 * behind it can be anywhere. The CPU reads a VMCB and its bitmaps by
	 * real physical address, so that is the contiguity that matters. A
	 * 2013 MacBook Pro showed the difference: four "contiguous" pages came
	 * back as runs of one or two.
	 */
	desc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryHostPhysicallyContiguous |
	    kIOMemoryMapperNone, size, 0xFFFFFFFFFFFFF000ULL);
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

/*
 * A small buffer is one IOBufferMemoryDescriptor, and 'kva' is its address.
 *
 * A buffer of 2M or more is built from parts: as many 2M runs of host-
 * contiguous, 2M-aligned memory as the system will give, so that the guest
 * can be mapped with 2M pages, and one ordinary buffer for whatever is left.
 * An IOMultiMemoryDescriptor strings the parts together so that the whole
 * can still be mapped into a process, or into the kernel, in one piece.
 */
struct port_membuf {
	IOMemoryDescriptor *desc;	/* what gets mapped and looked up */
	void *kva;			/* NULL if built from parts */
	IOMemoryDescriptor **parts;
	unsigned int nparts;
	size_t size;
	/*
	 * A borrowed buffer has none of the above. It is 'size' bytes of
	 * 'task' at 'uva', and 'chunks' holds, for each NVMM_LARGE-sized
	 * piece that has been pinned, the descriptor that pins it.
	 */
	bool borrowed;
	task_t task;
	uintptr_t uva;
	IOMemoryDescriptor **chunks;
	size_t nchunks;
};

static uint64_t
mem_now_ns(void)
{
	uint64_t abs, ns;

	clock_get_uptime(&abs);
	absolutetime_to_nanoseconds(abs, &ns);
	return ns;
}

/*
 * Kernel-allocated and not pageable, so the pages stay put for as long as
 * the descriptor exists. kIOMemoryKernelUserShared is what allows it to be
 * mapped into the emulator as well. No device ever does DMA to guest RAM
 * through this descriptor, so skip the I/O mapper rather than have it build
 * mappings for gigabytes of memory.
 */
static IOBufferMemoryDescriptor *
mem_plain(size_t size)
{
	IOBufferMemoryDescriptor *d;

	d = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared | kIOMemoryMapperNone,
	    size, NVMM_PAGE);
	if (d != NULL)
		bzero(d->getBytesNoCopy(), size);
	return d;
}

/* One 2M run: contiguous for the CPU and aligned to 2M, or nothing. */
static IOBufferMemoryDescriptor *
mem_large(void)
{
	IOBufferMemoryDescriptor *d;
	IOByteCount seglen = 0;
	addr64_t phys;

	d = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryHostPhysicallyContiguous |
	    kIOMemoryKernelUserShared | kIOMemoryMapperNone, NVMM_LARGE,
	    ~(NVMM_LARGE - 1));
	if (d == NULL)
		return NULL;

	/* Trust nothing: a run that is not what was asked for is no use. */
	if (d->prepare() != kIOReturnSuccess) {
		d->release();
		return NULL;
	}
	phys = d->getPhysicalSegment(0, &seglen, kIOMemoryMapperNone);
	d->complete();
	if (phys == 0 || (phys & (NVMM_LARGE - 1)) != 0 || seglen < NVMM_LARGE) {
		d->release();
		return NULL;
	}

	bzero(d->getBytesNoCopy(), NVMM_LARGE);
	return d;
}

static void
mem_parts_free(IOMemoryDescriptor **parts, unsigned int n, unsigned int cap)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		parts[i]->release();
	IOFree(parts, cap * sizeof(*parts));
}

/* Returns false if the buffer should be built the plain way instead. */
static bool
mem_build_parts(struct port_membuf *buf, size_t size)
{
	const unsigned int cap = (unsigned int)(size / NVMM_LARGE) + 1;
	const uint64_t t0 = mem_now_ns();
	IOMemoryDescriptor **parts;
	IOMultiMemoryDescriptor *multi;
	unsigned int n = 0, nlarge = 0;
	size_t done = 0;

	parts = (IOMemoryDescriptor **)IOMalloc(cap * sizeof(*parts));
	if (parts == NULL)
		return false;

	while (size - done >= NVMM_LARGE) {
		IOBufferMemoryDescriptor *d;

		if (mem_now_ns() - t0 > NVMM_LARGE_BUDGET_NS)
			break;
		d = mem_large();
		if (d == NULL)
			break;
		parts[n++] = d;
		nlarge++;
		done += NVMM_LARGE;
	}

	nvmm_darwin_mem_stats.last_large = nlarge;
	nvmm_darwin_mem_stats.last_wanted = (unsigned int)(size / NVMM_LARGE);
	nvmm_darwin_mem_stats.last_usec =
	    (unsigned int)((mem_now_ns() - t0) / 1000);
	if (size >= (64ULL << 20)) {
		printf("nvmm: %zu MB of guest memory: %u of %u 2M runs, "
		    "%u ms\n", size >> 20, nlarge,
		    nvmm_darwin_mem_stats.last_wanted,
		    nvmm_darwin_mem_stats.last_usec / 1000);
	}

	if (nlarge == 0) {
		IOFree(parts, cap * sizeof(*parts));
		return false;
	}

	if (done < size) {
		IOBufferMemoryDescriptor *rest = mem_plain(size - done);

		if (rest == NULL) {
			mem_parts_free(parts, n, cap);
			return false;
		}
		parts[n++] = rest;
	}

	multi = IOMultiMemoryDescriptor::withDescriptors(parts, n,
	    kIODirectionInOut, false);
	if (multi == NULL) {
		mem_parts_free(parts, n, cap);
		return false;
	}
	/* Wires every part. */
	if (multi->prepare() != kIOReturnSuccess) {
		multi->release();
		mem_parts_free(parts, n, cap);
		return false;
	}

	buf->desc = multi;
	buf->kva = NULL;
	buf->parts = parts;
	buf->nparts = n;
	return true;
}

struct port_membuf *
port_membuf_create(size_t size)
{
	const size_t rounded = (size + NVMM_PAGE - 1) & ~(NVMM_PAGE - 1);
	IOBufferMemoryDescriptor *plain;
	struct port_membuf *buf;

	buf = (struct port_membuf *)IOMalloc(sizeof(*buf));
	if (buf == NULL)
		return NULL;
	bzero(buf, sizeof(*buf));
	buf->size = rounded;

	if (rounded >= NVMM_LARGE && mem_build_parts(buf, rounded))
		return buf;

	plain = mem_plain(rounded);
	if (plain == NULL) {
		printf("nvmm: cannot allocate %zu bytes of wired memory\n",
		    rounded);
		IOFree(buf, sizeof(*buf));
		return NULL;
	}
	if (plain->prepare() != kIOReturnSuccess) {
		printf("nvmm: cannot wire %zu bytes of memory\n", rounded);
		plain->release();
		IOFree(buf, sizeof(*buf));
		return NULL;
	}
	buf->desc = plain;
	buf->kva = plain->getBytesNoCopy();
	return buf;
}

/*
 * The process's own memory, pinned piece by piece as the guest touches it.
 *
 * Pinning a range of a process's memory and learning its physical addresses
 * is what every driver that does I/O straight into a user buffer does, and
 * IOKit's way of doing it is a descriptor for the range and prepare(). The
 * pages stay put until complete(), even if the process unmaps them or dies.
 */
struct port_membuf *
port_membuf_borrow(uintptr_t uva, size_t size)
{
	struct port_membuf *buf;

	if ((uva & (NVMM_PAGE - 1)) != 0 || (size & (NVMM_PAGE - 1)) != 0 ||
	    size == 0)
		return NULL;
	buf = (struct port_membuf *)IOMalloc(sizeof(*buf));
	if (buf == NULL)
		return NULL;
	bzero(buf, sizeof(*buf));
	buf->nchunks = (size + NVMM_LARGE - 1) / NVMM_LARGE;
	buf->chunks = (IOMemoryDescriptor **)IOMalloc(buf->nchunks *
	    sizeof(*buf->chunks));
	if (buf->chunks == NULL) {
		IOFree(buf, sizeof(*buf));
		return NULL;
	}
	bzero(buf->chunks, buf->nchunks * sizeof(*buf->chunks));
	buf->borrowed = true;
	buf->task = current_task();
	buf->uva = uva;
	buf->size = size;
	return buf;
}

/* The caller serialises this for any one buffer. */
int
port_membuf_populate(struct port_membuf *buf, size_t off, size_t len)
{
	size_t c;

	if (!buf->borrowed)
		return 0;
	if (len == 0 || off + len > buf->size || off + len < off)
		return EINVAL;
	/* It is this process's memory only while this process is asking. */
	if (current_task() != buf->task)
		return EFAULT;

	for (c = off / NVMM_LARGE; c <= (off + len - 1) / NVMM_LARGE; c++) {
		const size_t coff = c * NVMM_LARGE;
		const size_t clen = (buf->size - coff < NVMM_LARGE) ?
		    buf->size - coff : NVMM_LARGE;
		IOMemoryDescriptor *d;

		if (buf->chunks[c] != NULL)
			continue;
		d = IOMemoryDescriptor::withAddressRange(buf->uva + coff, clen,
		    kIODirectionInOut | kIOMemoryMapperNone, buf->task);
		if (d == NULL)
			return ENOMEM;
		if (d->prepare() != kIOReturnSuccess) {
			d->release();
			printf("nvmm: cannot pin %zu KB of guest memory\n",
			    clen >> 10);
			return ENOMEM;
		}
		buf->chunks[c] = d;
		nvmm_darwin_mem_stats.pinned_chunks++;
	}
	return 0;
}

void
port_membuf_destroy(struct port_membuf *buf)
{
	if (buf->borrowed) {
		size_t c;

		for (c = 0; c < buf->nchunks; c++) {
			if (buf->chunks[c] != NULL) {
				buf->chunks[c]->complete();
				buf->chunks[c]->release();
				nvmm_darwin_mem_stats.pinned_chunks--;
			}
		}
		IOFree(buf->chunks, buf->nchunks * sizeof(*buf->chunks));
		IOFree(buf, sizeof(*buf));
		return;
	}
	buf->desc->complete();
	buf->desc->release();
	if (buf->parts != NULL) {
		mem_parts_free(buf->parts, buf->nparts,
		    (unsigned int)(buf->size / NVMM_LARGE) + 1);
	}
	IOFree(buf, sizeof(*buf));
}

uint64_t
port_membuf_pa(struct port_membuf *buf, size_t off)
{
	IOByteCount seglen = 0;
	addr64_t phys;

	if (buf->borrowed) {
		const size_t c = off / NVMM_LARGE;

		if (c >= buf->nchunks || buf->chunks[c] == NULL)
			panic("nvmm: guest memory at %#zx is not pinned", off);
		phys = buf->chunks[c]->getPhysicalSegment(off - c * NVMM_LARGE,
		    &seglen, kIOMemoryMapperNone);
	} else {
		phys = buf->desc->getPhysicalSegment(off, &seglen,
		    kIOMemoryMapperNone);
	}
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
	/* Borrowed memory is already where the process wants it. */
	if (buf->borrowed)
		return ENOTSUP;

	if (space == PORT_SPACE_KERNEL) {
		if (fixed)
			return EINVAL;
		if (buf->kva != NULL) {
			/* Already in the kernel's address space. */
			*addr = (uintptr_t)buf->kva + off;
			*cookie = NULL;
			return 0;
		}
		/* The parts are scattered; map them side by side. */
		map = buf->desc->createMappingInTask(kernel_task, 0,
		    kIOMapAnywhere, off, size);
		if (map == NULL)
			return ENOMEM;
		*addr = (uintptr_t)map->getAddress();
		*cookie = map;
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
