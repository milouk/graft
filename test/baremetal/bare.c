/*
 * bare.c — the "operating system" under the bare-metal test kernel.
 *
 * It implements the platform hooks from port/nvmm_port.h with the simplest
 * thing that is correct on one CPU with interrupts off: memory is identity
 * mapped, so a physical address is its own pointer, and nothing ever blocks.
 *
 * Every allocation is counted so the tests can assert that the code under
 * test gives back everything it takes.
 */

#include "nvmm.h"
#include "nvmm_internal.h"
#include "bare.h"

/* -------------------------------------------------------------------------- */
/* Port I/O, serial console, exit. */

static inline void
outb(uint16_t port, uint8_t val)
{
	__asm volatile ("outb %0,%1" : : "a" (val), "Nd" (port));
}

static inline uint8_t
inb(uint16_t port)
{
	uint8_t val;

	__asm volatile ("inb %1,%0" : "=a" (val) : "Nd" (port));
	return val;
}

#define COM1	0x3F8

static void
serial_init(void)
{
	outb(COM1 + 1, 0x00);	/* no interrupts */
	outb(COM1 + 3, 0x80);	/* DLAB */
	outb(COM1 + 0, 0x01);	/* 115200 */
	outb(COM1 + 1, 0x00);
	outb(COM1 + 3, 0x03);	/* 8n1 */
	outb(COM1 + 2, 0x00);	/* no FIFO */
}

static void
serial_putc(char c)
{
	while ((inb(COM1 + 5) & 0x20) == 0)
		continue;
	outb(COM1, (uint8_t)c);
}

void
bare_exit(int code)
{
	/*
	 * isa-debug-exit: QEMU exits with (value << 1) | 1, so writing 0 gives
	 * exit status 1 and writing 1 gives 3. The runner maps those back.
	 */
	outb(0xF4, (uint8_t)(code == 0 ? 0 : 1));
	for (;;)
		__asm volatile ("cli; hlt");
}

/* -------------------------------------------------------------------------- */
/* libc bits. */

void *
memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

void *
memmove(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else {
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	return dst;
}

void *
memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;

	while (n--)
		*d++ = (uint8_t)c;
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;

	for (; n--; x++, y++) {
		if (*x != *y)
			return *x - *y;
	}
	return 0;
}

size_t
strlen(const char *s)
{
	size_t n = 0;

	while (s[n] != '\0')
		n++;
	return n;
}

static void
put_num(uint64_t v, unsigned int base, int width, char pad, bool neg)
{
	char buf[24];
	int i = 0;

	do {
		const unsigned int d = (unsigned int)(v % base);

		buf[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		v /= base;
	} while (v != 0);
	if (neg)
		buf[i++] = '-';
	while (i < width)
		buf[i++] = pad;
	while (i > 0)
		serial_putc(buf[--i]);
}

static void
bare_vprintf(const char *fmt, va_list ap)
{
	for (; *fmt != '\0'; fmt++) {
		int lng = 0, width = 0;
		char pad = ' ';

		if (*fmt != '%') {
			if (*fmt == '\n')
				serial_putc('\r');
			serial_putc(*fmt);
			continue;
		}
		fmt++;
		if (*fmt == '#')
			fmt++;
		if (*fmt == '0') {
			pad = '0';
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		while (*fmt == 'l' || *fmt == 'z' || *fmt == 'j') {
			lng++;
			fmt++;
		}

		switch (*fmt) {
		case 's': {
			const char *s = va_arg(ap, const char *);

			if (s == NULL)
				s = "(null)";
			while (*s != '\0')
				serial_putc(*s++);
			break;
		}
		case 'c':
			serial_putc((char)va_arg(ap, int));
			break;
		case 'd':
		case 'i': {
			int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);

			put_num((uint64_t)(v < 0 ? -v : v), 10, width, pad,
			    v < 0);
			break;
		}
		case 'u':
			put_num(lng ? va_arg(ap, uint64_t) :
			    va_arg(ap, unsigned int), 10, width, pad, false);
			break;
		case 'x':
		case 'X':
			put_num(lng ? va_arg(ap, uint64_t) :
			    va_arg(ap, unsigned int), 16, width, pad, false);
			break;
		case 'p':
			serial_putc('0');
			serial_putc('x');
			put_num((uint64_t)va_arg(ap, void *), 16, 0, ' ',
			    false);
			break;
		case '%':
			serial_putc('%');
			break;
		default:
			serial_putc('?');
			break;
		}
	}
}

int
port_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	bare_vprintf(fmt, ap);
	va_end(ap);
	return 0;
}

void
port_panic(const char *fmt, ...)
{
	va_list ap;

	port_printf("\nPANIC: ");
	va_start(ap, fmt);
	bare_vprintf(fmt, ap);
	va_end(ap);
	port_printf("\n");
	bare_exit(1);
	for (;;)
		continue;
}

void
trap_handler(uint64_t vector, uint64_t error, uint64_t rip, uint64_t cr2)
{
	port_printf("\nTRAP: vector %lu error %#lx rip %#lx cr2 %#lx\n",
	    vector, error, rip, cr2);
	bare_exit(1);
}

/* -------------------------------------------------------------------------- */
/* Descriptor tables. */

struct tss64 {
	uint32_t rsvd0;
	uint64_t rsp[3];
	uint64_t rsvd1;
	uint64_t ist[7];
	uint64_t rsvd2;
	uint16_t rsvd3;
	uint16_t iomap;
} __attribute__((__packed__));

struct idt_gate {
	uint16_t off_lo;
	uint16_t sel;
	uint8_t ist;
	uint8_t type;
	uint16_t off_mid;
	uint32_t off_hi;
	uint32_t rsvd;
} __attribute__((__packed__));

extern uint64_t boot_gdt[];
extern const uint64_t trap_stubs[32];
static struct tss64 bare_tss;
static struct idt_gate bare_idt[32];

static void
tables_init(void)
{
	const uint64_t base = (uint64_t)&bare_tss;
	const uint64_t limit = sizeof(bare_tss) - 1;
	struct {
		uint16_t limit;
		uint64_t base;
	} __attribute__((__packed__)) idtr;
	unsigned int i;

	/* TSS descriptor: type 9 (available 64-bit TSS), present. */
	boot_gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) |
	    (0x89ULL << 40) | (((limit >> 16) & 0xF) << 48) |
	    (((base >> 24) & 0xFF) << 56);
	boot_gdt[6] = base >> 32;
	bare_tss.iomap = sizeof(bare_tss);
	__asm volatile ("ltr %0" : : "r" ((uint16_t)0x28));

	for (i = 0; i < 32; i++) {
		const uint64_t h = trap_stubs[i];

		bare_idt[i].off_lo = (uint16_t)h;
		bare_idt[i].sel = 0x08;
		bare_idt[i].ist = 0;
		bare_idt[i].type = 0x8E;	/* interrupt gate, present */
		bare_idt[i].off_mid = (uint16_t)(h >> 16);
		bare_idt[i].off_hi = (uint32_t)(h >> 32);
	}
	idtr.limit = sizeof(bare_idt) - 1;
	idtr.base = (uint64_t)bare_idt;
	__asm volatile ("lidt %0" : : "m" (idtr));

	/* Mask both PICs; the tests never take a device interrupt. */
	outb(0x21, 0xFF);
	outb(0xA1, 0xFF);
}

/* -------------------------------------------------------------------------- */
/* Memory. */

extern char __heap_start[];
static uintptr_t heap_cur, heap_end;
static void *page_freelist;

struct bare_stats bare_stats;

void
bare_init(uint64_t ram_bytes)
{
	serial_init();
	tables_init();
	heap_cur = (uintptr_t)__heap_start;
	heap_end = (uintptr_t)ram_bytes;
	page_freelist = NULL;
	memset(&bare_stats, 0, sizeof(bare_stats));
}

static void *
bump(size_t size, size_t align)
{
	uintptr_t p = (heap_cur + align - 1) & ~(align - 1);

	if (p + size > heap_end || p + size < p)
		return NULL;
	heap_cur = p + size;
	return (void *)p;
}

/*
 * A header in front of every allocation records its size, so port_free() can
 * check that the caller passed the size it allocated with. NVMM's interface
 * requires that, and a kernel allocator on the real host will rely on it.
 */
struct bare_hdr {
	size_t size;
	uint64_t magic;
};
#define BARE_MAGIC	0x4E564D4D62617265ULL

void *
port_malloc(size_t size)
{
	struct bare_hdr *h;

	h = bump(sizeof(*h) + size, 16);
	if (h == NULL)
		return NULL;
	h->size = size;
	h->magic = BARE_MAGIC;
	bare_stats.malloc_live++;
	bare_stats.malloc_bytes += size;
	return h + 1;
}

void
port_free(void *ptr, size_t size)
{
	struct bare_hdr *h = (struct bare_hdr *)ptr - 1;

	if (h->magic != BARE_MAGIC)
		port_panic("port_free: bad or double free of %p", ptr);
	if (h->size != size)
		port_panic("port_free: %p allocated with %lu, freed with %lu",
		    ptr, (uint64_t)h->size, (uint64_t)size);
	h->magic = 0;
	bare_stats.malloc_live--;
	bare_stats.malloc_bytes -= size;
}

int
port_pages_alloc(size_t npages, void **va, uint64_t *pa)
{
	void *p;

	if (npages == 1 && page_freelist != NULL) {
		p = page_freelist;
		page_freelist = *(void **)p;
	} else {
		p = bump(npages * 4096, 4096);
		if (p == NULL)
			return ENOMEM;
	}
	memset(p, 0, npages * 4096);
	bare_stats.pages_live += npages;
	*va = p;
	*pa = (uint64_t)p;
	return 0;
}

void
port_pages_free(void *va, uint64_t pa, size_t npages)
{
	if ((uint64_t)va != pa)
		port_panic("port_pages_free: va %p does not match pa %#lx",
		    va, pa);
	if (bare_stats.pages_live < npages)
		port_panic("port_pages_free: more pages freed than allocated");
	bare_stats.pages_live -= npages;
	if (npages == 1) {
		/* Poison, so a use-after-free reads as garbage, not zeroes. */
		memset(va, 0xA5, 4096);
		*(void **)va = page_freelist;
		page_freelist = va;
	}
}

/* -------------------------------------------------------------------------- */
/* Memory buffers. */

struct port_membuf {
	void *va;
	size_t size;
	size_t npages;
};

#define BARE_MAX_ALIASES	16
static struct {
	uintptr_t hva;
	void *ptr;
	size_t size;
	bool used;
} bare_aliases[BARE_MAX_ALIASES];

struct port_membuf *
port_membuf_create(size_t size)
{
	struct port_membuf *buf;
	uint64_t pa;

	buf = port_malloc(sizeof(*buf));
	if (buf == NULL)
		return NULL;
	buf->size = size;
	buf->npages = (size + 4095) / 4096;
	if (port_pages_alloc(buf->npages, &buf->va, &pa) != 0) {
		port_free(buf, sizeof(*buf));
		return NULL;
	}
	bare_stats.membufs_live++;
	return buf;
}

void
port_membuf_destroy(struct port_membuf *buf)
{
	port_pages_free(buf->va, (uint64_t)buf->va, buf->npages);
	port_free(buf, sizeof(*buf));
	bare_stats.membufs_live--;
}

uint64_t
port_membuf_pa(struct port_membuf *buf, size_t off)
{
	if (off >= buf->npages * 4096)
		port_panic("port_membuf_pa: offset %#lx out of range",
		    (uint64_t)off);
	return (uint64_t)buf->va + off;
}

int
port_membuf_map(struct port_membuf *buf, int space, size_t off, size_t size,
    bool fixed, int prot __unused, uintptr_t *addr, void **cookie)
{
	unsigned int i;

	if (off + size > buf->npages * 4096)
		return EINVAL;

	if (!fixed) {
		/* Everything is one address space: hand out the buffer. */
		*addr = (uintptr_t)buf->va + off;
		*cookie = &bare_aliases[BARE_MAX_ALIASES - 1].used;
		bare_stats.maps_live++;
		return 0;
	}

	/*
	 * A fixed mapping asks for the buffer to appear at an address the
	 * caller chose. There is no page-table trickery here, so record the
	 * alias; the test reaches the memory through bare_hva_ptr().
	 */
	if (space != PORT_SPACE_USER)
		return EINVAL;
	for (i = 0; i < BARE_MAX_ALIASES - 1; i++) {
		if (!bare_aliases[i].used) {
			bare_aliases[i].used = true;
			bare_aliases[i].hva = *addr;
			bare_aliases[i].ptr = (char *)buf->va + off;
			bare_aliases[i].size = size;
			*cookie = &bare_aliases[i];
			bare_stats.maps_live++;
			return 0;
		}
	}
	return ENOMEM;
}

void
port_membuf_unmap(void *cookie)
{
	unsigned int i;

	for (i = 0; i < BARE_MAX_ALIASES - 1; i++) {
		if (cookie == &bare_aliases[i]) {
			bare_aliases[i].used = false;
			break;
		}
	}
	bare_stats.maps_live--;
}

void *
bare_hva_ptr(uintptr_t hva)
{
	unsigned int i;

	for (i = 0; i < BARE_MAX_ALIASES - 1; i++) {
		if (bare_aliases[i].used && hva >= bare_aliases[i].hva &&
		    hva < bare_aliases[i].hva + bare_aliases[i].size)
			return (char *)bare_aliases[i].ptr +
			    (hva - bare_aliases[i].hva);
	}
	return NULL;
}

/* -------------------------------------------------------------------------- */
/* Locks, CPUs, misc. One CPU, nothing sleeps: locks only track ownership. */

#define BARE_THREAD	((void *)0x1)

void
port_mtx_init(os_mtx_t *l)
{
	l->impl = BARE_THREAD;
	l->owner = NULL;
	bare_stats.locks_live++;
}

void
port_mtx_destroy(os_mtx_t *l)
{
	if (l->owner != NULL)
		port_panic("mutex destroyed while held");
	l->impl = NULL;
	bare_stats.locks_live--;
}

void
port_mtx_lock(os_mtx_t *l)
{
	if (l->impl != BARE_THREAD)
		port_panic("mutex used before init or after destroy");
	if (l->owner != NULL)
		port_panic("mutex locked recursively");
	l->owner = BARE_THREAD;
}

void
port_mtx_unlock(os_mtx_t *l)
{
	if (l->owner == NULL)
		port_panic("mutex unlocked while not held");
	l->owner = NULL;
}

void
port_rwl_init(os_rwl_t *l)
{
	l->impl = NULL;		/* reader count */
	l->wowner = NULL;
	bare_stats.locks_live++;
}

void
port_rwl_destroy(os_rwl_t *l)
{
	if (l->wowner != NULL || l->impl != NULL)
		port_panic("rwlock destroyed while held");
	bare_stats.locks_live--;
}

void
port_rwl_rlock(os_rwl_t *l)
{
	if (l->wowner != NULL)
		port_panic("rwlock read-locked while write-locked");
	l->impl = (void *)((uintptr_t)l->impl + 1);
}

void
port_rwl_wlock(os_rwl_t *l)
{
	if (l->wowner != NULL || l->impl != NULL)
		port_panic("rwlock write-locked while held");
	l->wowner = BARE_THREAD;
}

void
port_rwl_unlock(os_rwl_t *l)
{
	if (l->wowner != NULL) {
		l->wowner = NULL;
	} else if (l->impl != NULL) {
		l->impl = (void *)((uintptr_t)l->impl - 1);
	} else {
		port_panic("rwlock unlocked while not held");
	}
}

void *
port_curthread(void)
{
	return BARE_THREAD;
}

unsigned int
port_ncpus(void)
{
	return 1;
}

unsigned int
port_curcpu(void)
{
	return 0;
}

static int preempt_level;

void
port_preempt_disable(void)
{
	preempt_level++;
}

void
port_preempt_enable(void)
{
	if (preempt_level == 0)
		port_panic("preemption enabled more often than disabled");
	preempt_level--;
}

bool
port_preempt_disabled(void)
{
	return preempt_level > 0;
}

void
port_ipi_broadcast(void (*func)(void *), void *arg)
{
	bare_stats.ipis++;
	func(arg);
}

bool bare_return_needed;

bool
port_return_needed(void)
{
	return bare_return_needed;
}

time_t
port_time(void)
{
	return 0;
}

int
port_curpid(void)
{
	return 1;
}

int
port_copyin(const void *uaddr, void *kaddr, size_t len)
{
	memcpy(kaddr, uaddr, len);
	return 0;
}

int
port_copyout(const void *kaddr, void *uaddr, size_t len)
{
	memcpy(uaddr, kaddr, len);
	return 0;
}
