/*
 * nvmm_port.h — the OS layer for hosts without a BSD virtual-memory system.
 *
 * NetBSD and DragonFly give NVMM a vmspace whose pmap doubles as the guest's
 * nested page table. Nothing equivalent is available to a macOS kernel
 * extension, so this layer builds the nested page tables itself (npt.c) and
 * implements NVMM's os_* interface on top of a small set of platform hooks.
 *
 * Two platforms implement the hooks:
 *   darwin/         the macOS kernel extension
 *   test/baremetal  a freestanding kernel used to exercise the engine in an
 *                   emulator, with no macOS involved
 *
 * Everything in port/ is shared between them, so the code that decides how a
 * guest's memory is laid out is the same code in both.
 */

#ifndef _NVMM_PORT_H_
#define _NVMM_PORT_H_

#if defined(__APPLE__)
#include "nvmm_darwin.h"
#else
#include "os_bare.h"
#endif

/* -------------------------------------------------------------------------- */
/* Types. */

/*
 * 64-bit by definition rather than uintptr_t: the core passes guest-physical
 * addresses (uint64_t) where a vaddr_t is expected, and on macOS those are
 * different types even though they are the same size.
 */
typedef uint64_t	vaddr_t;
typedef uint64_t	voff_t;
typedef size_t		vsize_t;
typedef uint64_t	paddr_t;

#ifndef PAGE_SIZE
#define PAGE_SIZE	4096
#endif
#define NVMM_PAGE_SIZE	4096ULL
#define NVMM_PAGE_MASK	(NVMM_PAGE_SIZE - 1)

/* -------------------------------------------------------------------------- */
/* Compiler helpers the core expects from <sys/cdefs.h>. */

#ifndef __predict_true
#define __predict_true(x)	__builtin_expect(!!(x), 1)
#endif
#ifndef __predict_false
#define __predict_false(x)	__builtin_expect(!!(x), 0)
#endif
#ifndef __unused
#define __unused		__attribute__((__unused__))
#endif
#ifndef __packed
#define __packed		__attribute__((__packed__))
#endif
#ifndef __aligned
#define __aligned(x)		__attribute__((__aligned__(x)))
#endif
#ifndef __cacheline_aligned
#define __cacheline_aligned	__attribute__((__aligned__(64)))
#endif
#ifndef __read_mostly
#define __read_mostly
#endif
#ifndef __diagused
#define __diagused		__attribute__((__unused__))
#endif
#ifndef __arraycount
#define __arraycount(a)		(sizeof(a) / sizeof((a)[0]))
#endif
#ifndef __insn_barrier
#define __insn_barrier()	__asm __volatile("":::"memory")
#endif
#ifndef roundup
#define roundup(x, y)		((((x) + ((y) - 1)) / (y)) * (y))
#endif
#ifndef uimin
#define uimin(a, b)		((unsigned int)(a) < (unsigned int)(b) ? \
				    (unsigned int)(a) : (unsigned int)(b))
#endif

/* __BIT, __BITS, __SHIFTIN and __SHIFTOUT come from compat/sys/bitops.h. */
#include <sys/bitops.h>

#ifndef ilog2
#define ilog2(x)		(63 - __builtin_clzll((unsigned long long)(x)))
#endif

/* -------------------------------------------------------------------------- */
/* Platform hooks. Each platform implements every function in this section. */

/* General-purpose kernel memory. */
void *	port_malloc(size_t size);
void	port_free(void *ptr, size_t size);

/* Physically contiguous, wired, zeroed pages, with both addresses known. */
int	port_pages_alloc(size_t npages, void **va, uint64_t *pa);
void	port_pages_free(void *va, uint64_t pa, size_t npages);

/*
 * A buffer of wired, zeroed memory that can be mapped into the kernel, into
 * the calling process, and (page by page, through port_membuf_pa) into a guest.
 */
struct port_membuf;
struct port_membuf *port_membuf_create(size_t size);
void	port_membuf_destroy(struct port_membuf *buf);
uint64_t port_membuf_pa(struct port_membuf *buf, size_t off);

#define PORT_SPACE_KERNEL	0
#define PORT_SPACE_USER		1
#define PORT_SPACE_GUEST	2

/*
 * Map [off, off+size) of the buffer into the kernel or the calling process.
 * If 'fixed', *addr is where it must go, replacing whatever is there.
 * '*cookie' is handed back to port_membuf_unmap.
 */
int	port_membuf_map(struct port_membuf *buf, int space, size_t off,
	    size_t size, bool fixed, int prot, uintptr_t *addr, void **cookie);
void	port_membuf_unmap(void *cookie);

/* Locks. The owner field lets the core ask "do I hold this?". */
typedef struct {
	void *impl;
	void *owner;
} os_mtx_t;
typedef struct {
	void *impl;
	void *wowner;
} os_rwl_t;

void	port_mtx_init(os_mtx_t *);
void	port_mtx_destroy(os_mtx_t *);
void	port_mtx_lock(os_mtx_t *);
void	port_mtx_unlock(os_mtx_t *);
void	port_rwl_init(os_rwl_t *);
void	port_rwl_destroy(os_rwl_t *);
void	port_rwl_rlock(os_rwl_t *);
void	port_rwl_wlock(os_rwl_t *);
void	port_rwl_unlock(os_rwl_t *);
void *	port_curthread(void);

/* CPUs. */
unsigned int port_ncpus(void);
unsigned int port_curcpu(void);
void	port_preempt_disable(void);
void	port_preempt_enable(void);
bool	port_preempt_disabled(void);
/* Run func(arg) on every CPU, including this one, and wait for all of them. */
void	port_ipi_broadcast(void (*func)(void *), void *arg);
/*
 * True when the host knows of a reason for the vCPU loop to return to userland
 * right now. A host that cannot tell returns false; the engine then applies
 * the rules described at NVMM_PORT_EXIT_BUDGET.
 */
bool	port_return_needed(void);

/* Misc. */
time_t	port_time(void);
int	port_curpid(void);
int	port_copyin(const void *uaddr, void *kaddr, size_t len);
int	port_copyout(const void *kaddr, void *uaddr, size_t len);
void	port_panic(const char *fmt, ...) __attribute__((__noreturn__));
int	port_printf(const char *fmt, ...);

/* -------------------------------------------------------------------------- */
/* NVMM's os_* interface, expressed with the hooks above. */

#define os_mtx_init(l)		port_mtx_init(l)
#define os_mtx_destroy(l)	port_mtx_destroy(l)
#define os_mtx_lock(l)		port_mtx_lock(l)
#define os_mtx_unlock(l)	port_mtx_unlock(l)
#define os_mtx_owned(l)		((l)->owner == port_curthread())

#define os_rwl_init(l)		port_rwl_init(l)
#define os_rwl_destroy(l)	port_rwl_destroy(l)
#define os_rwl_rlock(l)		port_rwl_rlock(l)
#define os_rwl_wlock(l)		port_rwl_wlock(l)
#define os_rwl_unlock(l)	port_rwl_unlock(l)
#define os_rwl_wheld(l)		((l)->wowner == port_curthread())

#define os_mem_alloc(size)	port_malloc(size)
#define os_mem_zalloc(size)	port_zalloc(size)
#define os_mem_free(ptr, size)	port_free(ptr, size)

static inline void *
port_zalloc(size_t size)
{
	void *p = port_malloc(size);

	if (p != NULL)
		memset(p, 0, size);
	return p;
}

#define os_printf		port_printf
#define panic			port_panic

#define os_atomic_inc_uint(x)	((void)__sync_fetch_and_add((x), 1))
#define os_atomic_dec_uint(x)	((void)__sync_fetch_and_sub((x), 1))
#define os_atomic_load_uint(x)	__atomic_load_n((x), __ATOMIC_RELAXED)
#define os_atomic_inc_64(x)	((void)__sync_fetch_and_add((x), 1))

#define OS_ASSERT(cond)							\
	do {								\
		if (__predict_false(!(cond)))				\
			port_panic("nvmm: assertion failed: %s (%s:%d)",\
			    #cond, __FILE__, __LINE__);			\
	} while (0)

#define os_copy_from_user	port_copyin
#define os_copy_to_user		port_copyout

/* CPUs. */
#define OS_MAXCPUS		256
typedef struct os_cpu {
	unsigned int idx;
} os_cpu_t;
extern os_cpu_t port_cpus[OS_MAXCPUS];

#define OS_CPU_FOREACH(cpu)						\
	for (unsigned int _ci = 0;					\
	    _ci < port_ncpus() && ((cpu) = &port_cpus[_ci]) != NULL;	\
	    _ci++)
#define os_cpu_number(cpu)	((cpu)->idx)
#define os_curcpu()		(&port_cpus[port_curcpu()])
#define os_curcpu_number()	port_curcpu()

#define os_preempt_disable()	port_preempt_disable()
#define os_preempt_enable()	port_preempt_enable()
#define os_preempt_disabled()	port_preempt_disabled()

#define OS_IPI_FUNC(func)	void func(void *arg)
#define os_ipi_broadcast(f, a)	port_ipi_broadcast((f), (a))
void	os_ipi_kickall(void);

/*
 * The vCPU loop runs with preemption disabled from start to finish, and it
 * records which host CPU it last ran on, so there is nothing to bind.
 */
#define curlwp_bind()		((int)0)
#define curlwp_bindx(bound)	((void)(bound))

static inline bool
os_return_needed(void)
{
	return port_return_needed();
}

/*
 * How many guest exits the engine may handle back to back before it returns
 * to userland regardless. See the end of the vCPU loop in nvmm_x86_svm.c.
 */
#define NVMM_PORT_EXIT_BUDGET	32

/* -------------------------------------------------------------------------- */
/* Guest memory (nvmm_port_vm.c). */

typedef struct os_vmobj os_vmobj_t;
typedef struct os_vmspace os_vmspace_t;
typedef struct os_vmmap {
	int kind;		/* PORT_SPACE_* */
	os_vmspace_t *vs;	/* PORT_SPACE_GUEST only */
} os_vmmap_t;

extern os_vmmap_t port_kernel_map;
extern os_vmmap_t port_user_map;
#define os_kernel_map		(&port_kernel_map)
#define os_curproc_map		(&port_user_map)

os_vmspace_t *	os_vmspace_create(vaddr_t, vaddr_t);
void		os_vmspace_destroy(os_vmspace_t *);
int		os_vmspace_fault(os_vmspace_t *, vaddr_t, int);
os_vmmap_t *	os_vmspace_get_vmmap(os_vmspace_t *);
/* Physical address of the nested page table root (nCR3). */
paddr_t		os_vmspace_pdirpa(os_vmspace_t *);
/* Bumped whenever a guest mapping is removed; vCPUs flush when it changes. */
uint64_t	os_vmspace_gen(os_vmspace_t *);

os_vmobj_t *	os_vmobj_create(voff_t);
void		os_vmobj_ref(os_vmobj_t *);
void		os_vmobj_rel(os_vmobj_t *);

int		os_vmobj_map_user(os_vmmap_t *, vaddr_t *, vsize_t,
		    os_vmobj_t *, voff_t, bool, bool, bool, int, int);
int		os_vmobj_map_kern(os_vmmap_t *, vaddr_t *, vsize_t,
		    os_vmobj_t *, voff_t, bool, bool, bool, int, int);
void		os_vmobj_unmap(os_vmmap_t *, vaddr_t, vaddr_t, bool);

/* Translate a guest-physical address by walking the space's nested page table. */
extern unsigned long port_vm_nlarge;
bool		port_vm_guest_lookup(os_vmspace_t *, vaddr_t, paddr_t *);

/* Drop every user mapping a process created, when it closes the device. */
void		port_vm_cleanup_pid(int pid);

void *		os_pagemem_zalloc(size_t);
void		os_pagemem_free(void *, size_t);

paddr_t		os_pa_zalloc(void);
void		os_pa_free(paddr_t);

int		os_contigpa_zalloc(paddr_t *, vaddr_t *, size_t);
void		os_contigpa_free(paddr_t, vaddr_t, size_t);

time_t		os_time(void);

/*
 * Turn hardware virtualization off and on again on every CPU, for the host's
 * sleep and wake. Implemented by the engine.
 */
void		nvmm_port_suspend(void);
void		nvmm_port_resume(void);

/* One-time setup and teardown of this layer's own state. */
void		port_init(void);
void		port_fini(void);

#endif /* _NVMM_PORT_H_ */
