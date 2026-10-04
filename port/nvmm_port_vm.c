/*
 * nvmm_port_vm.c — NVMM's guest-memory interface without a BSD VM system.
 *
 * On BSD an NVMM machine owns a vmspace, and guest memory is a VM object
 * mapped both into the emulator process and into that vmspace; pages are
 * faulted in on demand. Here:
 *
 *   os_vmobj_t    a wired buffer (port_membuf) with a reference count
 *   os_vmspace_t  a nested page table plus the list of what is mapped in it
 *   mapping       eager: every page is entered into the nested page table at
 *                 map time, so a nested page fault always means "this address
 *                 is not guest RAM" and is passed to the emulator as MMIO
 *
 * The cost of the eager design is that guest RAM is fully wired while the
 * machine exists. The benefit is that no host page-fault path is involved.
 */

#include "nvmm.h"
#include "nvmm_internal.h"
#include "npt.h"

struct os_vmobj {
	volatile unsigned int refcnt;
	size_t size;
	struct port_membuf *buf;
};

/* One contiguous run of an object mapped somewhere. */
struct port_mapping {
	struct port_mapping *next;
	vaddr_t start;
	vsize_t size;
	os_vmobj_t *obj;
	voff_t off;
	/* Kernel and user mappings only. */
	void *cookie;
	int pid;
};

struct os_vmspace {
	struct npt npt;
	os_vmmap_t map;
	os_mtx_t lock;
	volatile uint64_t gen;
	vaddr_t min;
	vaddr_t max;
	struct port_mapping *maps;
};

os_vmmap_t port_kernel_map = { PORT_SPACE_KERNEL, NULL };
os_vmmap_t port_user_map = { PORT_SPACE_USER, NULL };
os_cpu_t port_cpus[OS_MAXCPUS];

/* Kernel and user mappings, so they can be found again at unmap time. */
static os_mtx_t port_maps_lock;
static struct port_mapping *port_maps[2];

/* PA -> VA for single pages handed out by os_pa_zalloc(). */
struct port_pa_ent {
	struct port_pa_ent *next;
	paddr_t pa;
	void *va;
};
static os_mtx_t port_pa_lock;
static struct port_pa_ent *port_pa_list;

void
port_init(void)
{
	unsigned int i;

	for (i = 0; i < OS_MAXCPUS; i++)
		port_cpus[i].idx = i;
	port_mtx_init(&port_maps_lock);
	port_mtx_init(&port_pa_lock);
	port_maps[PORT_SPACE_KERNEL] = NULL;
	port_maps[PORT_SPACE_USER] = NULL;
	port_pa_list = NULL;
}

void
port_fini(void)
{
	port_mtx_destroy(&port_maps_lock);
	port_mtx_destroy(&port_pa_lock);
}

/* -------------------------------------------------------------------------- */

static int
npt_page_alloc(void *ctx __unused, void **va, uint64_t *pa)
{
	return port_pages_alloc(1, va, pa);
}

static void
npt_page_free(void *ctx __unused, void *va, uint64_t pa)
{
	port_pages_free(va, pa, 1);
}

static OS_IPI_FUNC(port_ipi_noop)
{
	(void)arg;
}

/*
 * Interrupt every CPU. A CPU that is running a guest takes a #VMEXIT, and its
 * vCPU loop then notices that the address space generation moved on.
 */
void
os_ipi_kickall(void)
{
	port_ipi_broadcast(port_ipi_noop, NULL);
}

os_vmspace_t *
os_vmspace_create(vaddr_t vmin, vaddr_t vmax)
{
	const struct npt_ops ops = { npt_page_alloc, npt_page_free, NULL };
	os_vmspace_t *vs;

	vs = port_zalloc(sizeof(*vs));
	if (vs == NULL)
		port_panic("nvmm: out of memory creating a guest space");
	if (npt_init(&vs->npt, &ops) != 0)
		port_panic("nvmm: out of memory creating a guest page table");

	vs->map.kind = PORT_SPACE_GUEST;
	vs->map.vs = vs;
	vs->min = vmin;
	vs->max = vmax;
	vs->gen = 1;
	vs->maps = NULL;
	port_mtx_init(&vs->lock);
	return vs;
}

void
os_vmspace_destroy(os_vmspace_t *vs)
{
	struct port_mapping *m, *next;

	for (m = vs->maps; m != NULL; m = next) {
		next = m->next;
		os_vmobj_rel(m->obj);
		port_free(m, sizeof(*m));
	}
	npt_destroy(&vs->npt);
	port_mtx_destroy(&vs->lock);
	port_free(vs, sizeof(*vs));
}

int
os_vmspace_fault(os_vmspace_t *vs __unused, vaddr_t va __unused,
    int prot __unused)
{
	/*
	 * Guest RAM is entered into the nested page table when it is mapped,
	 * so there is never anything to fault in. Reporting failure makes the
	 * core hand the access to the emulator as a memory exit.
	 */
	return EFAULT;
}

os_vmmap_t *
os_vmspace_get_vmmap(os_vmspace_t *vs)
{
	return &vs->map;
}

paddr_t
os_vmspace_pdirpa(os_vmspace_t *vs)
{
	return npt_root_pa(&vs->npt);
}

bool
port_vm_guest_lookup(os_vmspace_t *vs, vaddr_t gpa, paddr_t *hpa)
{
	bool found;

	port_mtx_lock(&vs->lock);
	found = npt_lookup(&vs->npt, gpa, hpa, NULL);
	port_mtx_unlock(&vs->lock);
	return found;
}

uint64_t
os_vmspace_gen(os_vmspace_t *vs)
{
	return __atomic_load_n(&vs->gen, __ATOMIC_RELAXED);
}

/* -------------------------------------------------------------------------- */

os_vmobj_t *
os_vmobj_create(voff_t size)
{
	os_vmobj_t *obj;

	obj = port_zalloc(sizeof(*obj));
	if (obj == NULL)
		return NULL;
	obj->buf = port_membuf_create((size_t)size);
	if (obj->buf == NULL) {
		port_free(obj, sizeof(*obj));
		return NULL;
	}
	obj->size = (size_t)size;
	obj->refcnt = 1;
	return obj;
}

void
os_vmobj_ref(os_vmobj_t *obj)
{
	os_atomic_inc_uint(&obj->refcnt);
}

void
os_vmobj_rel(os_vmobj_t *obj)
{
	if (__sync_sub_and_fetch(&obj->refcnt, 1) == 0) {
		port_membuf_destroy(obj->buf);
		port_free(obj, sizeof(*obj));
	}
}

static int
port_prot_to_npt(int prot)
{
	int nprot = 0;

	if (prot & PROT_READ)
		nprot |= NPT_PROT_READ;
	if (prot & PROT_WRITE)
		nprot |= NPT_PROT_WRITE;
	if (prot & PROT_EXEC)
		nprot |= NPT_PROT_EXEC;
	return nprot;
}

/*
 * Remove [start, end) from a guest space. A mapping that is only partly
 * covered is trimmed or split, so each surviving piece still holds its own
 * reference on the object behind it. Called with vs->lock held.
 */
static int
port_guest_unmap_locked(os_vmspace_t *vs, vaddr_t start, vaddr_t end)
{
	struct port_mapping **mp, *m, *tail;

	mp = &vs->maps;
	while ((m = *mp) != NULL) {
		const vaddr_t mstart = m->start;
		const vaddr_t mend = m->start + m->size;

		if (mend <= start || mstart >= end) {
			mp = &m->next;
			continue;
		}

		if (start <= mstart && end >= mend) {
			/* Entirely covered. */
			*mp = m->next;
			os_vmobj_rel(m->obj);
			port_free(m, sizeof(*m));
			continue;
		}

		if (start > mstart && end < mend) {
			/* A hole in the middle: keep both ends. */
			tail = port_zalloc(sizeof(*tail));
			if (tail == NULL)
				return ENOMEM;
			tail->start = end;
			tail->size = mend - end;
			tail->obj = m->obj;
			tail->off = m->off + (end - mstart);
			os_vmobj_ref(tail->obj);
			tail->next = m->next;
			m->size = start - mstart;
			m->next = tail;
			mp = &tail->next;
			continue;
		}

		if (start <= mstart) {
			/* The front is cut off. */
			m->off += end - mstart;
			m->size = mend - end;
			m->start = end;
		} else {
			/* The back is cut off. */
			m->size = start - mstart;
		}
		mp = &m->next;
	}

	if (npt_unmap(&vs->npt, start, end - start) != 0) {
		/*
		 * Stale translations may be cached on any CPU. Move the
		 * generation on, then force every running vCPU out of the
		 * guest so it sees the change before it runs again.
		 */
		(void)__sync_fetch_and_add(&vs->gen, 1);
	}
	return 0;
}

/* 2M pages entered since load. Diagnostic only. */
unsigned long port_vm_nlarge;

/*
 * Can the 2M of guest memory at 'gpa' be one 2M page? Only if the guest
 * address and the host address are both 2M aligned and the host pages behind
 * it are one contiguous run. Whether they are is up to the platform's buffer
 * allocator; this just takes the chance when it is there.
 */
static bool
port_guest_large_ok(os_vmobj_t *obj, vaddr_t gpa, voff_t off, vsize_t left,
    uint64_t hpa)
{
	vsize_t i;

	if (left < NPT_LARGE_SIZE || (gpa & (NPT_LARGE_SIZE - 1)) != 0 ||
	    (hpa & (NPT_LARGE_SIZE - 1)) != 0)
		return false;
	for (i = NVMM_PAGE_SIZE; i < NPT_LARGE_SIZE; i += NVMM_PAGE_SIZE) {
		if (port_membuf_pa(obj->buf, (size_t)(off + i)) != hpa + i)
			return false;
	}
	return true;
}

static int
port_guest_map(os_vmspace_t *vs, vaddr_t gpa, vsize_t size, os_vmobj_t *obj,
    voff_t off, int prot)
{
	struct port_mapping *m;
	const int nprot = port_prot_to_npt(prot);
	vsize_t done, step;
	int error;

	if ((gpa & NVMM_PAGE_MASK) != 0 || (size & NVMM_PAGE_MASK) != 0 ||
	    (off & NVMM_PAGE_MASK) != 0 || size == 0)
		return EINVAL;
	if (off + size > obj->size || off + size < off)
		return EINVAL;
	if (gpa < vs->min || gpa + size > vs->max || gpa + size < gpa)
		return EINVAL;
	if ((nprot & NPT_PROT_READ) == 0)
		return EINVAL;

	m = port_zalloc(sizeof(*m));
	if (m == NULL)
		return ENOMEM;

	port_mtx_lock(&vs->lock);

	/* A fixed mapping replaces whatever was there. */
	error = port_guest_unmap_locked(vs, gpa, gpa + size);
	if (error != 0) {
		port_mtx_unlock(&vs->lock);
		port_free(m, sizeof(*m));
		return error;
	}

	for (done = 0; done < size; done += step) {
		const uint64_t hpa = port_membuf_pa(obj->buf,
		    (size_t)(off + done));

		if (port_guest_large_ok(obj, gpa + done, off + done,
		    size - done, hpa)) {
			step = NPT_LARGE_SIZE;
			error = npt_map_large(&vs->npt, gpa + done, hpa, nprot);
			if (error == 0)
				(void)__sync_fetch_and_add(&port_vm_nlarge, 1);
		} else {
			step = NVMM_PAGE_SIZE;
			error = npt_map(&vs->npt, gpa + done, hpa, nprot);
		}
		if (error != 0) {
			(void)npt_unmap(&vs->npt, gpa, done);
			(void)__sync_fetch_and_add(&vs->gen, 1);
			port_mtx_unlock(&vs->lock);
			port_free(m, sizeof(*m));
			os_ipi_kickall();
			return ENOMEM;
		}
	}

	m->start = gpa;
	m->size = size;
	m->obj = obj;
	m->off = off;
	os_vmobj_ref(obj);
	m->next = vs->maps;
	vs->maps = m;

	port_mtx_unlock(&vs->lock);
	os_ipi_kickall();
	return 0;
}

static int
port_task_map(int space, vaddr_t *addr, vsize_t size, os_vmobj_t *obj,
    voff_t off, bool fixed, int prot)
{
	struct port_mapping *m;
	uintptr_t where = *addr;
	void *cookie = NULL;
	int error;

	if (size == 0 || off + size > obj->size || off + size < off)
		return EINVAL;

	m = port_zalloc(sizeof(*m));
	if (m == NULL)
		return ENOMEM;

	error = port_membuf_map(obj->buf, space, (size_t)off, size, fixed,
	    prot, &where, &cookie);
	if (error != 0) {
		port_free(m, sizeof(*m));
		return error;
	}

	m->start = where;
	m->size = size;
	m->obj = obj;
	m->off = off;
	m->cookie = cookie;
	m->pid = (space == PORT_SPACE_USER) ? port_curpid() : 0;
	os_vmobj_ref(obj);

	port_mtx_lock(&port_maps_lock);
	m->next = port_maps[space];
	port_maps[space] = m;
	port_mtx_unlock(&port_maps_lock);

	*addr = where;
	return 0;
}

int
os_vmobj_map_user(os_vmmap_t *map, vaddr_t *addr, vsize_t size,
    os_vmobj_t *obj, voff_t off, bool wired __unused, bool fixed,
    bool shared __unused, int prot, int maxprot __unused)
{
	if (map->kind == PORT_SPACE_GUEST)
		return port_guest_map(map->vs, *addr, size, obj, off, prot);
	return port_task_map(map->kind, addr, size, obj, off, fixed, prot);
}

int
os_vmobj_map_kern(os_vmmap_t *map, vaddr_t *addr, vsize_t size,
    os_vmobj_t *obj, voff_t off, bool wired, bool fixed, bool shared,
    int prot, int maxprot)
{
	return os_vmobj_map_user(map, addr, size, obj, off, wired, fixed,
	    shared, prot, maxprot);
}

void
os_vmobj_unmap(os_vmmap_t *map, vaddr_t start, vaddr_t end,
    bool wired __unused)
{
	struct port_mapping **mp, *m;
	const int pid = (map->kind == PORT_SPACE_USER) ? port_curpid() : 0;

	if (map->kind == PORT_SPACE_GUEST) {
		os_vmspace_t *vs = map->vs;

		port_mtx_lock(&vs->lock);
		(void)port_guest_unmap_locked(vs, start, end);
		port_mtx_unlock(&vs->lock);
		os_ipi_kickall();
		return;
	}

	/*
	 * Kernel and user mappings are only ever removed whole, which is how
	 * the core uses them.
	 */
	port_mtx_lock(&port_maps_lock);
	mp = &port_maps[map->kind];
	while ((m = *mp) != NULL) {
		if (m->pid == pid && m->start >= start &&
		    m->start + m->size <= end) {
			*mp = m->next;
			port_mtx_unlock(&port_maps_lock);
			port_membuf_unmap(m->cookie);
			os_vmobj_rel(m->obj);
			port_free(m, sizeof(*m));
			port_mtx_lock(&port_maps_lock);
			mp = &port_maps[map->kind];
			continue;
		}
		mp = &m->next;
	}
	port_mtx_unlock(&port_maps_lock);
}

void
port_vm_cleanup_pid(int pid)
{
	struct port_mapping **mp, *m;

	port_mtx_lock(&port_maps_lock);
	mp = &port_maps[PORT_SPACE_USER];
	while ((m = *mp) != NULL) {
		if (m->pid == pid) {
			*mp = m->next;
			port_mtx_unlock(&port_maps_lock);
			port_membuf_unmap(m->cookie);
			os_vmobj_rel(m->obj);
			port_free(m, sizeof(*m));
			port_mtx_lock(&port_maps_lock);
			mp = &port_maps[PORT_SPACE_USER];
			continue;
		}
		mp = &m->next;
	}
	port_mtx_unlock(&port_maps_lock);
}

/* -------------------------------------------------------------------------- */

void *
os_pagemem_zalloc(size_t size)
{
	const size_t npages = roundup(size, NVMM_PAGE_SIZE) / NVMM_PAGE_SIZE;
	void *va;
	uint64_t pa;

	if (port_pages_alloc(npages, &va, &pa) != 0)
		return NULL;

	/* The core frees this by address only, so remember the pair. */
	{
		struct port_pa_ent *e = port_malloc(sizeof(*e));

		if (e == NULL) {
			port_pages_free(va, pa, npages);
			return NULL;
		}
		e->pa = pa;
		e->va = va;
		port_mtx_lock(&port_pa_lock);
		e->next = port_pa_list;
		port_pa_list = e;
		port_mtx_unlock(&port_pa_lock);
	}
	return va;
}

void
os_pagemem_free(void *ptr, size_t size)
{
	const size_t npages = roundup(size, NVMM_PAGE_SIZE) / NVMM_PAGE_SIZE;
	struct port_pa_ent **ep, *e;

	port_mtx_lock(&port_pa_lock);
	for (ep = &port_pa_list; (e = *ep) != NULL; ep = &e->next) {
		if (e->va == ptr) {
			*ep = e->next;
			break;
		}
	}
	port_mtx_unlock(&port_pa_lock);

	OS_ASSERT(e != NULL);
	port_pages_free(e->va, e->pa, npages);
	port_free(e, sizeof(*e));
}

paddr_t
os_pa_zalloc(void)
{
	struct port_pa_ent *e;

	e = port_malloc(sizeof(*e));
	if (e == NULL)
		port_panic("nvmm: out of memory");
	if (port_pages_alloc(1, &e->va, &e->pa) != 0)
		port_panic("nvmm: out of memory");

	port_mtx_lock(&port_pa_lock);
	e->next = port_pa_list;
	port_pa_list = e;
	port_mtx_unlock(&port_pa_lock);

	return e->pa;
}

void
os_pa_free(paddr_t pa)
{
	struct port_pa_ent **ep, *e;

	port_mtx_lock(&port_pa_lock);
	for (ep = &port_pa_list; (e = *ep) != NULL; ep = &e->next) {
		if (e->pa == pa) {
			*ep = e->next;
			break;
		}
	}
	port_mtx_unlock(&port_pa_lock);

	OS_ASSERT(e != NULL);
	port_pages_free(e->va, e->pa, 1);
	port_free(e, sizeof(*e));
}

int
os_contigpa_zalloc(paddr_t *pa, vaddr_t *va, size_t npages)
{
	void *ptr;
	uint64_t phys;

	if (port_pages_alloc(npages, &ptr, &phys) != 0)
		return ENOMEM;
	*va = (vaddr_t)ptr;
	*pa = phys;
	return 0;
}

void
os_contigpa_free(paddr_t pa, vaddr_t va, size_t npages)
{
	port_pages_free((void *)va, pa, npages);
}

time_t
os_time(void)
{
	return port_time();
}
