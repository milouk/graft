/*
 * nvmm_darwin.c — the macOS kernel extension around NVMM.
 *
 * Three jobs:
 *   - the platform hooks from port/nvmm_port.h that are plain C
 *     (the IOKit-backed memory hooks are in nvmm_darwin_mem.cpp)
 *   - the /dev/nvmm character device
 *   - loading and unloading
 *
 * Only interfaces macOS exports to kernel extensions are used. The export
 * lists for macOS 15 were checked symbol by symbol; tools/check-kpi.sh repeats
 * that check against a built kext.
 *
 * STATUS: this file compiles and links. It has never been loaded.
 */

#include "nvmm.h"
#include "nvmm_internal.h"
#include "nvmm_ioctl.h"
#include "x86/nvmm_x86_internal.h"

#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/mach_types.h>
#include <mach/kmod.h>
#include <IOKit/IOLib.h>

#include "nvmm_darwin_mem.h"
#if defined(NVMM_DARWIN_SELFTEST)
#include "nvmm_darwin_selftest.h"
#endif

/* Exported by com.apple.kpi.unsupported on x86_64, but not declared in the SDK. */
extern int cpu_number(void);
extern unsigned int real_ncpus;
extern void mp_rendezvous_no_intrs(void (*action_func)(void *), void *arg);
extern boolean_t ml_set_interrupts_enabled(boolean_t enable);

static lck_grp_t *nvmm_lck_grp;

/* -------------------------------------------------------------------------- */
/* Memory. */

void *
port_malloc(size_t size)
{
	return IOMalloc(size);
}

void
port_free(void *ptr, size_t size)
{
	IOFree(ptr, size);
}

/* -------------------------------------------------------------------------- */
/* Locks. */

void
port_mtx_init(os_mtx_t *l)
{
	l->impl = lck_mtx_alloc_init(nvmm_lck_grp, LCK_ATTR_NULL);
	l->owner = NULL;
}

void
port_mtx_destroy(os_mtx_t *l)
{
	lck_mtx_free(l->impl, nvmm_lck_grp);
	l->impl = NULL;
}

void
port_mtx_lock(os_mtx_t *l)
{
	lck_mtx_lock(l->impl);
	l->owner = current_thread();
}

void
port_mtx_unlock(os_mtx_t *l)
{
	l->owner = NULL;
	lck_mtx_unlock(l->impl);
}

void
port_rwl_init(os_rwl_t *l)
{
	l->impl = lck_rw_alloc_init(nvmm_lck_grp, LCK_ATTR_NULL);
	l->wowner = NULL;
}

void
port_rwl_destroy(os_rwl_t *l)
{
	lck_rw_free(l->impl, nvmm_lck_grp);
	l->impl = NULL;
}

void
port_rwl_rlock(os_rwl_t *l)
{
	lck_rw_lock_shared(l->impl);
}

void
port_rwl_wlock(os_rwl_t *l)
{
	lck_rw_lock_exclusive(l->impl);
	l->wowner = current_thread();
}

void
port_rwl_unlock(os_rwl_t *l)
{
	/* Only the write holder can have set this, so the test is not racy. */
	if (l->wowner == current_thread()) {
		l->wowner = NULL;
		lck_rw_unlock_exclusive(l->impl);
	} else {
		lck_rw_unlock_shared(l->impl);
	}
}

void *
port_curthread(void)
{
	return current_thread();
}

/* -------------------------------------------------------------------------- */
/* CPUs. */

unsigned int
port_ncpus(void)
{
	return real_ncpus;
}

unsigned int
port_curcpu(void)
{
	return (unsigned int)cpu_number();
}

/*
 * Preemption.
 *
 * macOS does not export its preemption-disable primitive to extensions, but
 * holding a spin lock disables preemption. So there is one spin lock per CPU,
 * taken only by a thread running on that CPU. Interrupts are switched off
 * just long enough to learn which CPU that is and take its lock; after that
 * the thread cannot migrate. The lock is never contended.
 */
static struct {
	lck_spin_t *lock;
	void *owner;
	unsigned int depth;
} port_preempt[OS_MAXCPUS];

void
port_preempt_disable(void)
{
	const boolean_t intr = ml_set_interrupts_enabled(FALSE);
	const unsigned int cpu = (unsigned int)cpu_number();

	if (port_preempt[cpu].owner == current_thread()) {
		port_preempt[cpu].depth++;
	} else {
		lck_spin_lock(port_preempt[cpu].lock);
		port_preempt[cpu].owner = current_thread();
		port_preempt[cpu].depth = 1;
	}
	(void)ml_set_interrupts_enabled(intr);
}

void
port_preempt_enable(void)
{
	/* Preemption is off, so this thread is still on the CPU it locked. */
	const unsigned int cpu = (unsigned int)cpu_number();

	if (--port_preempt[cpu].depth == 0) {
		port_preempt[cpu].owner = NULL;
		lck_spin_unlock(port_preempt[cpu].lock);
	}
}

bool
port_preempt_disabled(void)
{
	return !preemption_enabled();
}

void
port_ipi_broadcast(void (*func)(void *), void *arg)
{
	mp_rendezvous_no_intrs(func, arg);
}

bool
port_return_needed(void)
{
	/*
	 * The vCPU loop holds preemption off, and macOS gives an extension no
	 * way to ask whether the scheduler or a signal is waiting. So go back
	 * to userland after every exit and let the kernel do its housekeeping
	 * on the way. It costs a system call per exit that the engine could
	 * have handled itself; correctness first.
	 */
	return true;
}

/* -------------------------------------------------------------------------- */
/* Misc. */

time_t
port_time(void)
{
	clock_sec_t secs;
	clock_usec_t usecs;

	clock_get_calendar_microtime(&secs, &usecs);
	return (time_t)secs;
}

int
port_curpid(void)
{
	return proc_selfpid();
}

int
port_copyin(const void *uaddr, void *kaddr, size_t len)
{
	return copyin((user_addr_t)(uintptr_t)uaddr, kaddr, len);
}

int
port_copyout(const void *kaddr, void *uaddr, size_t len)
{
	return copyout(kaddr, (user_addr_t)(uintptr_t)uaddr, len);
}

int
port_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void)vprintf(fmt, ap);
	va_end(ap);
	return 0;
}

/* Past this point "panic" means the kernel's, not the NVMM_PORT alias. */
#undef panic
extern void panic(const char *fmt, ...) __attribute__((__noreturn__));

void
port_panic(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	(void)vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	panic("nvmm: %s", buf);
}

/* -------------------------------------------------------------------------- */
/*
 * The device.
 *
 * NVMM ties machines to the file descriptor that created them, so that
 * closing it destroys them. A plain character device cannot tell one open
 * from another, so this is a cloning device: every open() gets a minor number
 * of its own, and close is delivered per minor.
 *
 * Two facts about devfs shape the clone function, both learned the hard way
 * on a real machine:
 *
 *   - It is called on every LOOKUP of the node, not only on open. An lstat()
 *     or an `ls /dev` calls it, and nothing is ever told that the minor it
 *     returned went unused. So it must not reserve anything.
 *
 *   - It must never return -1. On that path devfs_dntovn() returns without
 *     clearing DN_CREATE on the node, and every later lookup of it sleeps
 *     for ever, taking sudo and sshd down with it.
 *
 * So the clone function only suggests a minor that is free right now, open()
 * does the claiming, and "none free" is expressed as a minor that open()
 * always refuses.
 */

#define NVMM_MAXOPEN	64

static struct nvmm_owner *nvmm_owners[NVMM_MAXOPEN];
static lck_mtx_t *nvmm_dev_lock;
static int nvmm_major = -1;
static void *nvmm_devnode;

static int
darwin_nvmm_clone(dev_t dev __unused, int action)
{
	int i;

	if (action != DEVFS_CLONE_ALLOC)
		return 0;

	/*
	 * Called with the devfs lock held. A racy read is fine: this is only a
	 * suggestion, and open() checks again under nvmm_dev_lock.
	 */
	for (i = 0; i < NVMM_MAXOPEN; i++) {
		if (nvmm_owners[i] == NULL)
			break;
	}

	/* i == NVMM_MAXOPEN is the "all in use" minor; open() refuses it. */
	return i;
}

static int
darwin_nvmm_open(dev_t dev, int flags, int devtype __unused, proc_t p)
{
	const int unit = minor(dev);
	struct nvmm_owner *owner = NULL;
	int error = 0;

	if (unit < 0 || unit > NVMM_MAXOPEN)
		return ENXIO;
	if (unit == NVMM_MAXOPEN)
		return EBUSY;	/* every slot was taken when this was looked up */

	if (nvmm_impl == NULL) {
		error = ENXIO;
	} else if ((flags & (FREAD | FWRITE)) == FWRITE) {
		/* Write-only is how nvmmctl(8) asks for the global view. */
		if (proc_suser(p) != 0)
			error = EPERM;
		owner = &nvmm_root_owner;
	} else {
		owner = os_mem_alloc(sizeof(*owner));
		if (owner == NULL)
			error = ENOMEM;
		else
			owner->pid = proc_pid(p);
	}

	if (error != 0)
		return error;

	/* Claim the minor. Another open may have been suggested the same one. */
	lck_mtx_lock(nvmm_dev_lock);
	if (nvmm_owners[unit] != NULL)
		error = EBUSY;
	else
		nvmm_owners[unit] = owner;
	lck_mtx_unlock(nvmm_dev_lock);

	if (error != 0 && owner != &nvmm_root_owner)
		os_mem_free(owner, sizeof(*owner));
	return error;
}

static int
darwin_nvmm_close(dev_t dev, int flags __unused, int devtype __unused,
    proc_t p __unused)
{
	const int unit = minor(dev);
	struct nvmm_owner *owner;

	if (unit < 0 || unit >= NVMM_MAXOPEN)
		return ENXIO;

	lck_mtx_lock(nvmm_dev_lock);
	owner = nvmm_owners[unit];
	nvmm_owners[unit] = NULL;
	lck_mtx_unlock(nvmm_dev_lock);

	if (owner == NULL)
		return 0;

	nvmm_kill_machines(owner);
	if (owner != &nvmm_root_owner) {
		/* Guest RAM the process mapped stays wired until this runs. */
		port_vm_cleanup_pid(owner->pid);
		os_mem_free(owner, sizeof(*owner));
	}
	return 0;
}

static int
darwin_nvmm_ioctl(dev_t dev, u_long cmd, caddr_t data, int fflag __unused,
    proc_t p __unused)
{
	const int unit = minor(dev);
	struct nvmm_owner *owner;

	if (unit < 0 || unit >= NVMM_MAXOPEN)
		return ENXIO;
	owner = nvmm_owners[unit];
	if (owner == NULL)
		return ENXIO;

	return nvmm_ioctl(owner, cmd, data);
}

static const struct cdevsw nvmm_cdevsw = {
	.d_open = darwin_nvmm_open,
	.d_close = darwin_nvmm_close,
	.d_read = eno_rdwrt,
	.d_write = eno_rdwrt,
	.d_ioctl = darwin_nvmm_ioctl,
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

/* -------------------------------------------------------------------------- */
/* Sleep and wake: see nvmm_darwin_mem.cpp for the registration. */

void
nvmm_darwin_will_sleep(void)
{
#if defined(NVMM_DARWIN_SELFTEST)
	/* No engine in this build: just record that the notification came. */
	os_atomic_inc_uint(&nvmm_selftest_sleeps);
	return;
#endif
	/*
	 * A CPU comes back from sleep with SVM switched off and its host save
	 * area forgotten. nvmm_port_suspend() parks every vCPU loop first, so
	 * none of them can execute VMRUN in that state.
	 */
	if (os_atomic_load_uint(&nvmm_nmachines) > 0)
		os_printf("nvmm: sleeping with virtual machines running; "
		    "this path is untested\n");
	nvmm_port_suspend();
}

void
nvmm_darwin_did_wake(void)
{
#if defined(NVMM_DARWIN_SELFTEST)
	os_atomic_inc_uint(&nvmm_selftest_wakes);
	return;
#endif
	nvmm_port_resume();
}

/* -------------------------------------------------------------------------- */
/* Load and unload. */

static void
darwin_locks_fini(void)
{
	unsigned int i;

	for (i = 0; i < OS_MAXCPUS; i++) {
		if (port_preempt[i].lock != NULL) {
			lck_spin_free(port_preempt[i].lock, nvmm_lck_grp);
			port_preempt[i].lock = NULL;
		}
	}
	if (nvmm_dev_lock != NULL) {
		lck_mtx_free(nvmm_dev_lock, nvmm_lck_grp);
		nvmm_dev_lock = NULL;
	}
	if (nvmm_lck_grp != NULL) {
		lck_grp_free(nvmm_lck_grp);
		nvmm_lck_grp = NULL;
	}
}

kern_return_t nvmm_kext_start(kmod_info_t *ki, void *data);
kern_return_t nvmm_kext_stop(kmod_info_t *ki, void *data);

kern_return_t
nvmm_kext_start(kmod_info_t *ki __unused, void *data __unused)
{
	unsigned int i;
	int error;

	if (real_ncpus > OS_MAXCPUS) {
		printf("nvmm: more CPUs than this build supports\n");
		return KERN_FAILURE;
	}

	nvmm_lck_grp = lck_grp_alloc_init("nvmm", LCK_GRP_ATTR_NULL);
	nvmm_dev_lock = lck_mtx_alloc_init(nvmm_lck_grp, LCK_ATTR_NULL);
	for (i = 0; i < real_ncpus; i++)
		port_preempt[i].lock = lck_spin_alloc_init(nvmm_lck_grp,
		    LCK_ATTR_NULL);

	nvmm_darwin_mem_init();
	port_init();

	if (nvmm_ident() == NULL) {
		printf("nvmm: this CPU is not supported (AMD with SVM, nested "
		    "paging and next-RIP save is required)\n");
		goto fail_port;
	}

	error = port_x86_init();
	if (error != 0) {
		printf("nvmm: cannot set up FPU state storage (%d)\n", error);
		goto fail_port;
	}

	error = nvmm_init();
	if (error != 0) {
		printf("nvmm: initialisation failed (%d)\n", error);
		goto fail_x86;
	}

	nvmm_major = cdevsw_add(-1, &nvmm_cdevsw);
	if (nvmm_major < 0) {
		printf("nvmm: cannot register the device\n");
		goto fail_nvmm;
	}
	nvmm_devnode = devfs_make_node_clone(makedev(nvmm_major, 0),
	    DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, darwin_nvmm_clone, "nvmm");
	if (nvmm_devnode == NULL) {
		printf("nvmm: cannot create /dev/nvmm\n");
		goto fail_cdev;
	}

#if defined(NVMM_DARWIN_SELFTEST)
	/*
	 * Self-test build: the stand-in engine is behind /dev/nvmm, and the
	 * glue checks are behind /dev/nvmm-selftest.
	 */
	error = nvmm_selftest_attach();
	if (error != 0) {
		printf("nvmm: cannot create the self-test device (%d)\n",
		    error);
		devfs_remove(nvmm_devnode);
		nvmm_devnode = NULL;
		goto fail_cdev;
	}
#endif

	nvmm_darwin_power_register();

	printf("nvmm: attached, using backend %s\n", nvmm_impl->name);
	return KERN_SUCCESS;

fail_cdev:
	(void)cdevsw_remove(nvmm_major, &nvmm_cdevsw);
	nvmm_major = -1;
fail_nvmm:
	nvmm_fini();
fail_x86:
	port_x86_fini();
fail_port:
	port_fini();
	nvmm_darwin_mem_fini();
	darwin_locks_fini();
	return KERN_FAILURE;
}

kern_return_t
nvmm_kext_stop(kmod_info_t *ki __unused, void *data __unused)
{
	int i;

	if (os_atomic_load_uint(&nvmm_nmachines) > 0)
		return KERN_FAILURE;
	for (i = 0; i < NVMM_MAXOPEN; i++) {
		if (nvmm_owners[i] != NULL)
			return KERN_FAILURE;
	}
#if defined(NVMM_DARWIN_SELFTEST)
	if (nvmm_selftest_detach() != 0)
		return KERN_FAILURE;
#endif

	nvmm_darwin_power_unregister();

	devfs_remove(nvmm_devnode);
	(void)cdevsw_remove(nvmm_major, &nvmm_cdevsw);
	nvmm_devnode = NULL;
	nvmm_major = -1;

	nvmm_fini();
	port_x86_fini();
	port_fini();
	nvmm_darwin_mem_fini();
	darwin_locks_fini();

	printf("nvmm: detached\n");
	return KERN_SUCCESS;
}

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

#if defined(NVMM_DARWIN_SELFTEST)
KMOD_EXPLICIT_DECL(org.nvmm.driver.NVMMSelfTest, "0.1.0", _start, _stop)
#else
KMOD_EXPLICIT_DECL(org.nvmm.driver.NVMM, "0.1.0", _start, _stop)
#endif
__private_extern__ kmod_start_func_t *_realmain = nvmm_kext_start;
__private_extern__ kmod_stop_func_t *_antimain = nvmm_kext_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
