/*
 * nvmm-run.c — a small virtual machine monitor on libnvmm, in place of QEMU.
 *
 * It boots a Linux kernel directly, with no firmware, and gives it the least
 * a kernel needs: a serial console, the legacy interrupt controller, the
 * legacy timer and a clock. One virtual CPU. The kernel is told not to look
 * for anything else (no ACPI, no PCI, no local APIC), which is what keeps
 * this small: NVMM leaves every device to userland, and the local APIC is by
 * far the largest of them.
 *
 * A disk, if given, is a virtio block device: the kernel finds it through
 * its command line, which is how virtio's memory-mapped transport is used
 * on machines with no firmware tables to describe it.
 *
 * A network card, if asked for (-n), is a virtio one on macOS's own vmnet:
 * the guest gets an address by DHCP and reaches the outside through the
 * host, with nothing to configure. vmnet needs root.
 *
 * Usage (needs /dev/nvmm):
 *   nvmm-run -k vmlinuz [-i initramfs] [-d disk.img] [-n] [-m megabytes]
 *            [-a "extra cmdline"]
 *
 * The console is this terminal. Ctrl-A then x quits.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <dispatch/dispatch.h>
#include <vmnet/vmnet.h>

#include <nvmm.h>

/* -------------------------------------------------------------------------- */
/* Guest memory layout. */

#define GDT_GPA		0x00000500ULL
#define BOOT_PARAMS_GPA	0x00007000ULL
#define PML4_GPA	0x00009000ULL	/* + PDPT + 4 PDs: 6 pages */
#define CMDLINE_GPA	0x00020000ULL
#define CMDLINE_MAX	2048
#define LOWMEM_END	0x0009FC00ULL
#define HIGHMEM_START	0x00100000ULL
#define MEM_MAX_MB	3072		/* stay below the device hole */

static struct nvmm_machine mach;
static struct nvmm_vcpu vcpu;
static uint8_t *ram;
static uint64_t ram_size;
static pthread_t vcpu_thread;
static volatile bool running = true;
static int exit_code;
static bool verbose;

static void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "\r\nnvmm-run: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\r\n");
	va_end(ap);
	exit(1);
}

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* -------------------------------------------------------------------------- */
/*
 * The interrupt controller: two 8259s, the slave on the master's line 2.
 * Edge-triggered: a device raises a line, and that sets a request bit until
 * the guest takes the interrupt.
 */

struct pic {
	uint8_t irr, isr, imr;
	uint8_t base;		/* vector of line 0 */
	uint8_t init_step;	/* 0 = running, else which ICW comes next */
	bool icw4;
	bool read_isr;
};

static struct pic pics[2] = {
	{ .imr = 0xFF, .base = 0x08 }, { .imr = 0xFF, .base = 0x70 },
};

static void
irq_raise(int line)
{
	if (line < 8)
		pics[0].irr |= (uint8_t)(1u << line);
	else
		pics[1].irr |= (uint8_t)(1u << (line - 8));
}

/* The line one 8259 would deliver now, or -1. Lower numbers win. */
static int
pic_line(const struct pic *p)
{
	const uint8_t req = p->irr & (uint8_t)~p->imr;
	int i;

	for (i = 0; i < 8; i++) {
		if (p->isr & (1u << i))
			return -1;	/* same or higher priority in service */
		if (req & (1u << i))
			return i;
	}
	return -1;
}

static bool
pic_pending(void)
{
	if (pic_line(&pics[1]) >= 0)
		pics[0].irr |= 1u << 2;
	else
		pics[0].irr &= (uint8_t)~(1u << 2);
	return pic_line(&pics[0]) >= 0;
}

/* The guest takes the interrupt: returns its vector. */
static uint8_t
pic_ack(void)
{
	int line = pic_line(&pics[0]);

	if (line < 0)
		return pics[0].base + 7;	/* spurious */
	pics[0].isr |= (uint8_t)(1u << line);
	pics[0].irr &= (uint8_t)~(1u << line);
	if (line != 2)
		return pics[0].base + (uint8_t)line;

	line = pic_line(&pics[1]);
	if (line < 0)
		return pics[1].base + 7;
	pics[1].isr |= (uint8_t)(1u << line);
	pics[1].irr &= (uint8_t)~(1u << line);
	return pics[1].base + (uint8_t)line;
}

static void
pic_write(struct pic *p, int a0, uint8_t val)
{
	int i;

	if (a0 == 0) {
		if (val & 0x10) {			/* ICW1: initialise */
			p->init_step = 2;
			p->icw4 = (val & 0x01) != 0;
			p->imr = 0;
			p->isr = 0;
			p->irr = 0;
			p->read_isr = false;
		} else if (val & 0x08) {		/* OCW3 */
			if (val & 0x02)
				p->read_isr = (val & 0x01) != 0;
		} else {				/* OCW2 */
			if ((val & 0xE0) == 0x20) {	/* non-specific EOI */
				for (i = 0; i < 8; i++) {
					if (p->isr & (1u << i)) {
						p->isr &= (uint8_t)~(1u << i);
						break;
					}
				}
			} else if ((val & 0xE0) == 0x60) { /* specific EOI */
				p->isr &= (uint8_t)~(1u << (val & 7));
			}
		}
		return;
	}

	switch (p->init_step) {
	case 0:
		p->imr = val;				/* OCW1 */
		break;
	case 2:
		p->base = val & 0xF8;			/* ICW2 */
		p->init_step = 3;
		break;
	case 3:
		p->init_step = p->icw4 ? 4 : 0;		/* ICW3 */
		break;
	case 4:
		p->init_step = 0;			/* ICW4 */
		break;
	}
}

static uint8_t
pic_read(const struct pic *p, int a0)
{
	if (a0 == 0)
		return p->read_isr ? p->isr : p->irr;
	return p->imr;
}

/* -------------------------------------------------------------------------- */
/*
 * The timer: an 8254. Counters are not counted down; their value is worked
 * out from the host clock whenever the guest looks. Channel 0 drives line 0.
 */

#define PIT_HZ	1193182ULL

struct pit_chan {
	uint16_t reload;
	uint8_t mode;
	uint8_t access;		/* 1 = low byte, 2 = high byte, 3 = both */
	bool write_hi, read_hi;
	bool latched;
	uint16_t latch;
	uint8_t write_lo;
	uint64_t start_ns;	/* when the count was loaded */
	bool armed;
	bool gate;
	uint64_t next_irq_ns;	/* channel 0 only */
};

static struct pit_chan pit[3];
static uint8_t port61;

static uint64_t
pit_period_ns(const struct pit_chan *c)
{
	const uint64_t n = c->reload ? c->reload : 65536;

	return n * 1000000000ULL / PIT_HZ;
}

static uint64_t
pit_ticks(const struct pit_chan *c)
{
	return (now_ns() - c->start_ns) * PIT_HZ / 1000000000ULL;
}

static uint16_t
pit_count(const struct pit_chan *c)
{
	const uint64_t n = c->reload ? c->reload : 65536;
	const uint64_t t = pit_ticks(c);

	if (!c->armed)
		return c->reload;
	if (c->mode == 2 || c->mode == 3)
		return (uint16_t)(n - (t % n));
	return (uint16_t)(t >= n ? 0 : n - t);		/* one-shot */
}

/* The output pin, as the speaker port shows it for channel 2. */
static bool
pit_out(const struct pit_chan *c)
{
	const uint64_t n = c->reload ? c->reload : 65536;
	const uint64_t t = pit_ticks(c);

	if (!c->armed)
		return false;
	if (c->mode == 3)
		return (t % n) < n / 2;
	if (c->mode == 2)
		return (t % n) != n - 1;
	return t >= n;
}

static void
pit_load(int ch, uint16_t val)
{
	struct pit_chan *c = &pit[ch];

	c->reload = val;
	c->start_ns = now_ns();
	c->armed = true;
	if (ch == 0)
		c->next_irq_ns = c->start_ns + pit_period_ns(c);
}

static void
pit_write(int port, uint8_t val)
{
	struct pit_chan *c;
	int ch;

	if (port == 3) {
		ch = val >> 6;
		if (ch == 3)
			return;				/* read-back: unused */
		c = &pit[ch];
		if ((val & 0x30) == 0) {		/* latch the count */
			c->latch = pit_count(c);
			c->latched = true;
			c->read_hi = false;
			return;
		}
		c->access = (val >> 4) & 3;
		c->mode = (val >> 1) & 7;
		if (c->mode > 5)
			c->mode &= 3;
		c->write_hi = false;
		c->read_hi = false;
		c->latched = false;
		c->armed = false;
		return;
	}

	c = &pit[port];
	switch (c->access) {
	case 1:
		pit_load(port, val);
		break;
	case 2:
		pit_load(port, (uint16_t)(val << 8));
		break;
	default:
		if (!c->write_hi) {
			c->write_lo = val;
			c->write_hi = true;
		} else {
			c->write_hi = false;
			pit_load(port, (uint16_t)(c->write_lo | (val << 8)));
		}
		break;
	}
}

static uint8_t
pit_read(int port)
{
	struct pit_chan *c;
	uint16_t v;

	if (port == 3)
		return 0;
	c = &pit[port];
	v = c->latched ? c->latch : pit_count(c);

	switch (c->access) {
	case 1:
		c->latched = false;
		return (uint8_t)v;
	case 2:
		c->latched = false;
		return (uint8_t)(v >> 8);
	default:
		if (!c->read_hi) {
			c->read_hi = true;
			return (uint8_t)v;
		}
		c->read_hi = false;
		c->latched = false;
		return (uint8_t)(v >> 8);
	}
}

/* Raise line 0 if channel 0 is due. Returns ns until it next is, or 0. */
static uint64_t
pit_poll(void)
{
	struct pit_chan *c = &pit[0];
	uint64_t now;

	if (!c->armed || c->next_irq_ns == 0)
		return 0;
	now = now_ns();
	if (now >= c->next_irq_ns) {
		irq_raise(0);
		if (c->mode == 2 || c->mode == 3) {
			const uint64_t per = pit_period_ns(c);

			/* Missed periods are dropped, not replayed. */
			c->next_irq_ns += per * ((now - c->next_irq_ns) / per + 1);
		} else {
			c->next_irq_ns = 0;
			return 0;
		}
	}
	return c->next_irq_ns - now;
}

/* -------------------------------------------------------------------------- */
/* The serial port: a 16550A at 0x3F8 on line 4, wired to this terminal. */

#define UART_IRQ	4

static struct {
	uint8_t ier, lcr, mcr, scr, fcr, dll, dlm;
	bool thre_pending;
	uint8_t rx[256];
	unsigned int rx_head, rx_tail;
} uart;

static struct termios saved_termios;
static bool termios_saved;
static bool escape_seen;

static bool
uart_rx_ready(void)
{
	return uart.rx_head != uart.rx_tail;
}

static void
uart_update_irq(void)
{
	if (((uart.ier & 0x01) && uart_rx_ready()) ||
	    ((uart.ier & 0x02) && uart.thre_pending))
		irq_raise(UART_IRQ);
}

static void
uart_write(int reg, uint8_t val)
{
	const bool dlab = (uart.lcr & 0x80) != 0;

	switch (reg) {
	case 0:
		if (dlab) {
			uart.dll = val;
			break;
		}
		if (uart.mcr & 0x10) {			/* loopback */
			uart.rx[uart.rx_head++ % sizeof(uart.rx)] = val;
		} else {
			(void)write(STDOUT_FILENO, &val, 1);
		}
		uart.thre_pending = true;
		break;
	case 1:
		if (dlab) {
			uart.dlm = val;
			break;
		}
		uart.ier = val & 0x0F;
		if (uart.ier & 0x02)
			uart.thre_pending = true;
		break;
	case 2:
		uart.fcr = val;
		if (val & 0x02)
			uart.rx_head = uart.rx_tail = 0;
		break;
	case 3:
		uart.lcr = val;
		break;
	case 4:
		uart.mcr = val;
		break;
	case 7:
		uart.scr = val;
		break;
	}
	uart_update_irq();
}

static uint8_t
uart_read(int reg)
{
	const bool dlab = (uart.lcr & 0x80) != 0;
	uint8_t v = 0;

	switch (reg) {
	case 0:
		if (dlab)
			return uart.dll;
		if (uart_rx_ready())
			v = uart.rx[uart.rx_tail++ % sizeof(uart.rx)];
		break;
	case 1:
		return dlab ? uart.dlm : uart.ier;
	case 2:
		v = (uart.fcr & 0x01) ? 0xC0 : 0x00;
		if ((uart.ier & 0x01) && uart_rx_ready()) {
			v |= 0x04;
		} else if ((uart.ier & 0x02) && uart.thre_pending) {
			v |= 0x02;
			uart.thre_pending = false;
		} else {
			v |= 0x01;			/* nothing pending */
		}
		break;
	case 3:
		return uart.lcr;
	case 4:
		return uart.mcr;
	case 5:
		return (uint8_t)(0x60 | (uart_rx_ready() ? 0x01 : 0x00));
	case 6:
		if (uart.mcr & 0x10) {
			return (uint8_t)(((uart.mcr & 0x02) ? 0x10 : 0) |
			    ((uart.mcr & 0x01) ? 0x20 : 0) |
			    ((uart.mcr & 0x04) ? 0x40 : 0) |
			    ((uart.mcr & 0x08) ? 0x80 : 0));
		}
		return 0xB0;				/* DCD, DSR, CTS */
	case 7:
		return uart.scr;
	}
	uart_update_irq();
	return v;
}

static void
console_restore(void)
{
	if (termios_saved)
		tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
}

static void
console_init(void)
{
	struct termios t;

	if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
		termios_saved = true;
		atexit(console_restore);
		t = saved_termios;
		cfmakeraw(&t);
		t.c_oflag |= OPOST | ONLCR;
		tcsetattr(STDIN_FILENO, TCSANOW, &t);
	}
	(void)fcntl(STDIN_FILENO, F_SETFL,
	    fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
}

/* Move what has been typed into the receive buffer. */
static void
console_poll(void)
{
	uint8_t buf[64];
	ssize_t n, i;

	if ((uart.rx_head - uart.rx_tail) > sizeof(uart.rx) - sizeof(buf))
		return;
	n = read(STDIN_FILENO, buf, sizeof(buf));
	for (i = 0; i < n; i++) {
		if (escape_seen) {
			escape_seen = false;
			if (buf[i] == 'x') {
				running = false;
				return;
			}
			if (buf[i] != 0x01)
				continue;
		} else if (buf[i] == 0x01) {
			escape_seen = true;
			continue;
		}
		uart.rx[uart.rx_head++ % sizeof(uart.rx)] = buf[i];
	}
	if (n > 0)
		uart_update_irq();
}

/* -------------------------------------------------------------------------- */
/* The clock: just enough of the CMOS real-time clock to give the date. */

static uint8_t cmos_index;

static uint8_t
bcd(int v)
{
	return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static uint8_t
cmos_read(void)
{
	const time_t t = time(NULL);
	struct tm tm;

	gmtime_r(&t, &tm);
	switch (cmos_index & 0x7F) {
	case 0x00: return bcd(tm.tm_sec);
	case 0x02: return bcd(tm.tm_min);
	case 0x04: return bcd(tm.tm_hour);
	case 0x06: return bcd(tm.tm_wday + 1);
	case 0x07: return bcd(tm.tm_mday);
	case 0x08: return bcd(tm.tm_mon + 1);
	case 0x09: return bcd(tm.tm_year % 100);
	case 0x32: return bcd((tm.tm_year + 1900) / 100);
	case 0x0A: return 0x26;		/* no update in progress */
	case 0x0B: return 0x02;		/* 24-hour, BCD */
	case 0x0D: return 0x80;		/* battery good */
	default:   return 0x00;
	}
}


/* -------------------------------------------------------------------------- */
/*
 * Virtio devices on the memory-mapped transport (virtio 1.x, "version 2"),
 * with split virtqueues. The guest's memory is ours too, so a queue is just
 * read where it lies.
 */

#define VIRTIO_BASE	0xD0000000ULL
#define VIRTIO_STRIDE	0x1000ULL
#define VIRTIO_MAXDEV	4
#define VQ_MAX		256
#define VQ_IOV_MAX	(VQ_MAX + 2)

#define VIRTIO_F_VERSION_1	(1ULL << 32)
#define VIRTIO_ID_NET		1
#define VIRTIO_ID_BLOCK		2

struct virtq {
	uint32_t num;
	bool ready;
	uint64_t desc, avail, used;
	uint16_t last_avail;
};

struct virtio_dev {
	uint32_t id;
	int irq;
	uint64_t features, driver_features;
	uint32_t features_sel, driver_sel, queue_sel;
	uint32_t status, isr;
	struct virtq vq[2];
	uint8_t config[64];
	void (*notify)(struct virtio_dev *, unsigned int);
	int fd;				/* block: the image */
	interface_ref iface;		/* network: the vmnet interface */
	size_t max_packet;
};

static struct virtio_dev vdevs[VIRTIO_MAXDEV];
static unsigned int nvdevs;

/* Guest memory, bounds checked. NULL if any of it is not RAM. */
static void *
gpa_ptr(uint64_t gpa, uint64_t len)
{
	if (gpa >= ram_size || len > ram_size - gpa)
		return NULL;
	return ram + gpa;
}

struct vq_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};
#define VQ_DESC_F_NEXT	1
#define VQ_DESC_F_WRITE	2

/*
 * Take the next request off a queue: its buffers in 'iov', the first 'nout'
 * of them for the device to read, the rest to write. Returns the number of
 * buffers, 0 if the queue is empty, -1 if the guest handed over nonsense.
 */
static int
vq_pop(struct virtq *q, struct iovec *iov, unsigned int *nout, uint16_t *head)
{
	const uint16_t *avail = gpa_ptr(q->avail, 4 + 2 * (uint64_t)q->num);
	const struct vq_desc *desc = gpa_ptr(q->desc, 16 * (uint64_t)q->num);
	unsigned int n = 0;
	uint16_t i;

	if (!q->ready || avail == NULL || desc == NULL)
		return -1;
	if (q->last_avail == avail[1])
		return 0;

	*head = i = avail[2 + (q->last_avail % q->num)];
	*nout = 0;
	for (;;) {
		if (i >= q->num || n == VQ_IOV_MAX)
			return -1;
		iov[n].iov_base = gpa_ptr(desc[i].addr, desc[i].len);
		iov[n].iov_len = desc[i].len;
		if (iov[n].iov_base == NULL)
			return -1;
		if ((desc[i].flags & VQ_DESC_F_WRITE) == 0) {
			if (*nout != n)
				return -1;	/* readable after writable */
			(*nout)++;
		}
		n++;
		if ((desc[i].flags & VQ_DESC_F_NEXT) == 0)
			break;
		i = desc[i].next;
	}
	q->last_avail++;
	return (int)n;
}

/* Hand a request back, 'len' bytes of it written by the device. */
static void
vq_push(struct virtio_dev *d, struct virtq *q, uint16_t head, uint32_t len)
{
	uint16_t *used = gpa_ptr(q->used, 4 + 8 * (uint64_t)q->num);
	const uint16_t *avail = gpa_ptr(q->avail, 4);
	uint32_t *elem;

	if (used == NULL || avail == NULL)
		return;
	elem = (uint32_t *)(void *)(used + 2) + 2 * (used[1] % q->num);
	elem[0] = head;
	elem[1] = len;
	__sync_synchronize();
	used[1]++;

	d->isr |= 1;
	if ((avail[0] & 1) == 0)		/* guest wants interrupts */
		irq_raise(d->irq);
}

static void
virtio_reset(struct virtio_dev *d)
{
	memset(d->vq, 0, sizeof(d->vq));
	d->status = 0;
	d->isr = 0;
	d->driver_features = 0;
	d->features_sel = d->driver_sel = d->queue_sel = 0;
}

static uint32_t
virtio_read(struct virtio_dev *d, uint64_t off, size_t size)
{
	const struct virtq *q = &d->vq[d->queue_sel & 1];
	uint32_t v = 0;

	if (off >= 0x100) {
		if (off - 0x100 + size <= sizeof(d->config))
			memcpy(&v, d->config + (off - 0x100), size);
		return v;
	}
	switch (off) {
	case 0x000: return 0x74726976;		/* "virt" */
	case 0x004: return 2;
	case 0x008: return d->id;
	case 0x00C: return 0x4D4D564E;		/* "NVMM" */
	case 0x010:
		return (uint32_t)(d->features >> (d->features_sel ? 32 : 0));
	case 0x034: return d->queue_sel < 2 ? VQ_MAX : 0;
	case 0x044: return q->ready;
	case 0x060: return d->isr;
	case 0x070: return d->status;
	case 0x0FC: return 0;
	default:    return 0;
	}
}

static void
virtio_write(struct virtio_dev *d, uint64_t off, uint32_t v)
{
	struct virtq *q = &d->vq[d->queue_sel & 1];

	switch (off) {
	case 0x014: d->features_sel = v; break;
	case 0x020:
		if (d->driver_sel)
			d->driver_features = (d->driver_features &
			    0xFFFFFFFFULL) | ((uint64_t)v << 32);
		else
			d->driver_features = (d->driver_features &
			    ~0xFFFFFFFFULL) | v;
		break;
	case 0x024: d->driver_sel = v; break;
	case 0x030: d->queue_sel = v; break;
	case 0x038: q->num = (v <= VQ_MAX) ? v : 0; break;
	case 0x044: q->ready = (v & 1) && q->num != 0; break;
	case 0x050:
		if (v < 2 && d->vq[v].ready)
			d->notify(d, v);
		break;
	case 0x064: d->isr &= ~v; break;
	case 0x070:
		if (v == 0)
			virtio_reset(d);
		else
			d->status = v;
		break;
	case 0x080: q->desc = (q->desc & ~0xFFFFFFFFULL) | v; break;
	case 0x084: q->desc = (q->desc & 0xFFFFFFFFULL) | ((uint64_t)v << 32); break;
	case 0x090: q->avail = (q->avail & ~0xFFFFFFFFULL) | v; break;
	case 0x094: q->avail = (q->avail & 0xFFFFFFFFULL) | ((uint64_t)v << 32); break;
	case 0x0A0: q->used = (q->used & ~0xFFFFFFFFULL) | v; break;
	case 0x0A4: q->used = (q->used & 0xFFFFFFFFULL) | ((uint64_t)v << 32); break;
	}
}

/* ---- the block device ---- */

#define VIRTIO_BLK_F_SEG_MAX	(1ULL << 2)
#define VIRTIO_BLK_F_FLUSH	(1ULL << 9)
#define BLK_SEG_MAX		126

/*
 * Read or write a run of buffers at an offset. macOS has preadv() only from
 * version 11, so it is done one buffer at a time. Returns bytes moved, or -1.
 */
static ssize_t
blk_rw(int fd, bool wr, const struct iovec *iov, int n, off_t off)
{
	ssize_t total = 0, r;
	int i;

	for (i = 0; i < n; i++) {
		size_t done = 0;

		while (done < iov[i].iov_len) {
			char *p = (char *)iov[i].iov_base + done;
			const size_t left = iov[i].iov_len - done;

			r = wr ? pwrite(fd, p, left, off) :
			    pread(fd, p, left, off);
			if (r < 0 && errno == EINTR)
				continue;
			if (r <= 0)
				return -1;	/* error, or past the end */
			done += (size_t)r;
			off += r;
			total += r;
		}
	}
	return total;
}

static void
blk_notify(struct virtio_dev *d, unsigned int qi)
{
	static struct iovec iov[VQ_IOV_MAX];
	struct virtq *q = &d->vq[qi];
	unsigned int nout;
	uint16_t head;
	int n;

	while ((n = vq_pop(q, iov, &nout, &head)) > 0) {
		struct { uint32_t type, rsvd; uint64_t sector; } hdr;
		uint8_t *status;
		uint32_t written = 1;
		ssize_t r = 0;

		/* A header to read, a status byte to write, data between. */
		if (nout < 1 || iov[0].iov_len != sizeof(hdr) ||
		    (unsigned int)n == nout || iov[n - 1].iov_len != 1) {
			vq_push(d, q, head, 0);
			continue;
		}
		memcpy(&hdr, iov[0].iov_base, sizeof(hdr));
		status = iov[n - 1].iov_base;

		switch (hdr.type) {
		case 0:					/* read */
			r = blk_rw(d->fd, false, iov + nout, n - 1 - (int)nout,
			    (off_t)(hdr.sector * 512));
			if (r >= 0)
				written += (uint32_t)r;
			break;
		case 1:					/* write */
			r = blk_rw(d->fd, true, iov + 1, (int)nout - 1,
			    (off_t)(hdr.sector * 512));
			break;
		case 4:					/* flush */
			r = fsync(d->fd);
			break;
		case 8:					/* identify */
			if ((unsigned int)n - nout == 2 &&
			    iov[nout].iov_len >= 20) {
				memset(iov[nout].iov_base, 0, 20);
				memcpy(iov[nout].iov_base, "nvmm-run", 8);
				written += 20;
			}
			break;
		default:
			r = -2;
			break;
		}
		*status = (r == -2) ? 2 : (r < 0) ? 1 : 0;
		vq_push(d, q, head, written);
	}
	if (n < 0)
		fprintf(stderr, "[virtio-blk: bad request from the guest]\r\n");
}

/* Returns the kernel command line fragment that announces the device. */
static void
blk_add(const char *path, char *cmdline, size_t cmdlen)
{
	struct virtio_dev *d = &vdevs[nvdevs];
	struct stat st;
	uint64_t sectors;
	uint32_t seg_max = BLK_SEG_MAX;

	d->fd = open(path, O_RDWR);
	if (d->fd == -1 || fstat(d->fd, &st) == -1)
		die("%s: %s", path, strerror(errno));
	sectors = (uint64_t)st.st_size / 512;

	d->id = VIRTIO_ID_BLOCK;
	d->irq = 5 + (int)nvdevs;
	d->features = VIRTIO_F_VERSION_1 | VIRTIO_BLK_F_SEG_MAX |
	    VIRTIO_BLK_F_FLUSH;
	d->notify = blk_notify;
	memcpy(d->config, &sectors, 8);
	memcpy(d->config + 12, &seg_max, 4);

	snprintf(cmdline + strlen(cmdline), cmdlen - strlen(cmdline),
	    " virtio_mmio.device=4K@%#llx:%d",
	    (unsigned long long)(VIRTIO_BASE + nvdevs * VIRTIO_STRIDE), d->irq);
	nvdevs++;
}

/* ---- the network card ---- */

#define VIRTIO_NET_F_MAC	(1ULL << 5)
#define NET_HDR_LEN		12	/* struct virtio_net_hdr_v1 */
#define NET_RXQ			0
#define NET_TXQ			1

static struct virtio_dev *netdev;
static volatile bool net_rx_ready;

/* Guest to host: each request is a header and one frame. */
static void
net_tx(struct virtio_dev *d)
{
	static struct iovec iov[VQ_IOV_MAX];
	static uint8_t frame[65536];
	struct virtq *q = &d->vq[NET_TXQ];
	unsigned int nout;
	uint16_t head;
	int n, i;

	while ((n = vq_pop(q, iov, &nout, &head)) > 0) {
		struct vmpktdesc pkt;
		struct iovec out;
		size_t len = 0, skip = NET_HDR_LEN;
		int count = 1;

		for (i = 0; i < (int)nout; i++) {
			const uint8_t *p = iov[i].iov_base;
			size_t l = iov[i].iov_len;

			if (skip >= l) {
				skip -= l;
				continue;
			}
			p += skip;
			l -= skip;
			skip = 0;
			if (len + l > sizeof(frame))
				break;
			memcpy(frame + len, p, l);
			len += l;
		}
		if (len > 0 && len <= d->max_packet) {
			out.iov_base = frame;
			out.iov_len = len;
			memset(&pkt, 0, sizeof(pkt));
			pkt.vm_pkt_size = len;
			pkt.vm_pkt_iov = &out;
			pkt.vm_pkt_iovcnt = 1;
			(void)vmnet_write(d->iface, &pkt, &count);
		}
		vq_push(d, q, head, 0);
	}
}

/* Host to guest: as many waiting frames as the guest has buffers for. */
static void
net_rx(struct virtio_dev *d)
{
	static struct iovec iov[VQ_IOV_MAX];
	static uint8_t frame[65536];
	struct virtq *q = &d->vq[NET_RXQ];

	if (!q->ready)
		return;
	net_rx_ready = false;

	for (;;) {
		struct vmpktdesc pkt;
		struct iovec in;
		unsigned int nout;
		uint16_t head, last = q->last_avail;
		size_t len, done = 0;
		int count = 1, n, i;

		/* Only read a frame once there is somewhere to put it. */
		n = vq_pop(q, iov, &nout, &head);
		if (n <= 0)
			return;

		in.iov_base = frame + NET_HDR_LEN;
		in.iov_len = d->max_packet;
		memset(&pkt, 0, sizeof(pkt));
		pkt.vm_pkt_size = d->max_packet;
		pkt.vm_pkt_iov = &in;
		pkt.vm_pkt_iovcnt = 1;
		if (vmnet_read(d->iface, &pkt, &count) != VMNET_SUCCESS ||
		    count < 1) {
			q->last_avail = last;	/* nothing: put it back */
			return;
		}

		memset(frame, 0, NET_HDR_LEN);
		frame[10] = 1;			/* num_buffers = 1 */
		len = NET_HDR_LEN + pkt.vm_pkt_size;
		for (i = (int)nout; i < n && done < len; i++) {
			size_t l = iov[i].iov_len;

			if (l > len - done)
				l = len - done;
			memcpy(iov[i].iov_base, frame + done, l);
			done += l;
		}
		vq_push(d, q, head, (uint32_t)done);
	}
}

static void
net_notify(struct virtio_dev *d, unsigned int qi)
{
	if (qi == NET_TXQ)
		net_tx(d);
	else
		net_rx(d);
}

static void
net_add(char *cmdline, size_t cmdlen)
{
	struct virtio_dev *d = &vdevs[nvdevs];
	dispatch_queue_t q = dispatch_queue_create("nvmm-run.net", NULL);
	dispatch_semaphore_t sem = dispatch_semaphore_create(0);
	xpc_object_t desc = xpc_dictionary_create(NULL, NULL, 0);
	__block vmnet_return_t status = VMNET_FAILURE;
	static uint8_t mac[6];		/* a block cannot capture an array */
	__block uint64_t maxpkt = 0;

	xpc_dictionary_set_uint64(desc, vmnet_operation_mode_key,
	    VMNET_SHARED_MODE);
	d->iface = vmnet_start_interface(desc, q,
	    ^(vmnet_return_t st, xpc_object_t params) {
		status = st;
		if (st == VMNET_SUCCESS && params != NULL) {
			const char *m = xpc_dictionary_get_string(params,
			    vmnet_mac_address_key);
			unsigned int b[6];

			if (m != NULL && sscanf(m, "%x:%x:%x:%x:%x:%x", &b[0],
			    &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
				for (int i = 0; i < 6; i++)
					mac[i] = (uint8_t)b[i];
			}
			maxpkt = xpc_dictionary_get_uint64(params,
			    vmnet_max_packet_size_key);
		}
		dispatch_semaphore_signal(sem);
	});
	if (d->iface != NULL)
		dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
	if (d->iface == NULL || status != VMNET_SUCCESS)
		die("cannot start a vmnet interface (status %d); the network "
		    "needs root", (int)status);

	/* Frames arrive on vmnet's own thread; the vCPU thread moves them. */
	vmnet_interface_set_event_callback(d->iface,
	    VMNET_INTERFACE_PACKETS_AVAILABLE, q,
	    ^(interface_event_t ev, xpc_object_t params) {
		(void)ev;
		(void)params;
		net_rx_ready = true;
		pthread_kill(vcpu_thread, SIGUSR1);
	});

	d->id = VIRTIO_ID_NET;
	d->irq = 5 + (int)nvdevs;
	d->features = VIRTIO_F_VERSION_1 | VIRTIO_NET_F_MAC;
	d->notify = net_notify;
	d->max_packet = (maxpkt != 0 && maxpkt <= 65536 - NET_HDR_LEN) ?
	    (size_t)maxpkt : 1514;
	memcpy(d->config, mac, 6);
	netdev = d;

	snprintf(cmdline + strlen(cmdline), cmdlen - strlen(cmdline),
	    " virtio_mmio.device=4K@%#llx:%d",
	    (unsigned long long)(VIRTIO_BASE + nvdevs * VIRTIO_STRIDE), d->irq);
	nvdevs++;
}

/* -------------------------------------------------------------------------- */
/* I/O ports. */

static void
io_callback(struct nvmm_io *io)
{
	const uint16_t port = io->port;
	uint32_t val = 0xFFFFFFFF;
	uint8_t out = io->data[0];

	if (port >= 0x3F8 && port <= 0x3FF) {
		if (io->in)
			val = uart_read(port - 0x3F8);
		else
			uart_write(port - 0x3F8, out);
	} else if (port == 0x20 || port == 0x21) {
		if (io->in)
			val = pic_read(&pics[0], port & 1);
		else
			pic_write(&pics[0], port & 1, out);
	} else if (port == 0xA0 || port == 0xA1) {
		if (io->in)
			val = pic_read(&pics[1], port & 1);
		else
			pic_write(&pics[1], port & 1, out);
	} else if (port >= 0x40 && port <= 0x43) {
		if (io->in)
			val = pit_read(port - 0x40);
		else
			pit_write(port - 0x40, out);
	} else if (port == 0x61) {
		if (io->in) {
			val = (uint8_t)((port61 & 0x0F) |
			    (pit_out(&pit[2]) ? 0x20 : 0));
		} else {
			/* Raising the gate restarts channel 2. */
			if ((out & 1) && !(port61 & 1) && pit[2].access != 0)
				pit_load(2, pit[2].reload);
			port61 = out;
		}
	} else if (port == 0x70) {
		if (!io->in)
			cmos_index = out;
	} else if (port == 0x71) {
		if (io->in)
			val = cmos_read();
	} else if (port == 0x64) {
		/* The keyboard controller's reset line is how Linux reboots. */
		if (!io->in && out == 0xFE) {
			running = false;
			exit_code = 0;
		}
	} else if (port == 0x4D0 || port == 0x4D1) {
		if (io->in)
			val = 0;
	} else if (verbose && port != 0x80 && port != 0xCF8 &&
	    !(port >= 0xCFC && port <= 0xCFF) && port != 0x60) {
		fprintf(stderr, "[io %s port %#x size %zu]\r\n",
		    io->in ? "in" : "out", port, io->size);
	}

	if (io->in)
		memcpy(io->data, &val, io->size);
}

/* Memory the guest touches that is not RAM: the virtio devices. */
static void
mem_callback(struct nvmm_mem *mem)
{
	const uint64_t idx = (mem->gpa - VIRTIO_BASE) / VIRTIO_STRIDE;
	uint32_t v = 0;

	if (mem->gpa >= VIRTIO_BASE && idx < nvdevs && mem->size <= 4) {
		const uint64_t off = (mem->gpa - VIRTIO_BASE) % VIRTIO_STRIDE;

		if (mem->write) {
			memcpy(&v, mem->data, mem->size);
			virtio_write(&vdevs[idx], off, v);
		} else {
			v = virtio_read(&vdevs[idx], off, mem->size);
			memcpy(mem->data, &v, mem->size);
		}
		return;
	}

	if (!mem->write)
		memset(mem->data, 0xFF, mem->size);
	if (verbose)
		fprintf(stderr, "[mem %s gpa %#llx size %zu]\r\n",
		    mem->write ? "write" : "read",
		    (unsigned long long)mem->gpa, mem->size);
}

/* -------------------------------------------------------------------------- */
/* Loading Linux: the 64-bit boot protocol, Documentation/x86/boot.rst. */

static void *
read_file(const char *path, size_t *size)
{
	struct stat st;
	void *buf;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1 || fstat(fd, &st) == -1)
		die("%s: %s", path, strerror(errno));
	buf = malloc((size_t)st.st_size);
	if (buf == NULL || read(fd, buf, (size_t)st.st_size) != st.st_size)
		die("%s: cannot read", path);
	close(fd);
	*size = (size_t)st.st_size;
	return buf;
}

#define rd16(p, o)	(*(uint16_t *)(void *)((uint8_t *)(p) + (o)))
#define rd32(p, o)	(*(uint32_t *)(void *)((uint8_t *)(p) + (o)))
#define rd64(p, o)	(*(uint64_t *)(void *)((uint8_t *)(p) + (o)))

static void
e820_add(uint8_t *bp, uint64_t addr, uint64_t size, uint32_t type)
{
	uint8_t *e = bp + 0x2D0 + bp[0x1E8] * 20;

	memcpy(e, &addr, 8);
	memcpy(e + 8, &size, 8);
	memcpy(e + 16, &type, 4);
	bp[0x1E8]++;
}

/* Returns the address execution starts at. */
static uint64_t
load_linux(const char *kpath, const char *ipath, const char *cmdline)
{
	uint8_t *bp = ram + BOOT_PARAMS_GPA;
	size_t ksize, isize = 0, setup_size, hdr_end;
	uint8_t *kernel, *initrd = NULL;
	uint64_t load, initrd_gpa = 0, init_size;
	unsigned int setup_sects;

	kernel = read_file(kpath, &ksize);
	if (ksize < 0x1000 || rd16(kernel, 0x1FE) != 0xAA55 ||
	    rd32(kernel, 0x202) != 0x53726448)
		die("%s is not a Linux bzImage", kpath);
	if (rd16(kernel, 0x206) < 0x020C)
		die("kernel boot protocol %#x is too old", rd16(kernel, 0x206));
	if ((rd16(kernel, 0x236) & 0x01) == 0)
		die("kernel has no 64-bit entry point");

	setup_sects = kernel[0x1F1] ? kernel[0x1F1] : 4;
	setup_size = (setup_sects + 1) * 512;
	load = rd64(kernel, 0x258);		/* the address it prefers */
	init_size = rd32(kernel, 0x260);
	if (load < HIGHMEM_START || load + init_size > ram_size)
		die("kernel wants %llu MB at %#llx; guest has %llu MB",
		    (unsigned long long)(init_size >> 20),
		    (unsigned long long)load,
		    (unsigned long long)(ram_size >> 20));
	memcpy(ram + load, kernel + setup_size, ksize - setup_size);

	/* The "zero page": the kernel's own header, then what we fill in. */
	memset(bp, 0, 4096);
	hdr_end = 0x202 + kernel[0x201];
	memcpy(bp + 0x1F1, kernel + 0x1F1, hdr_end - 0x1F1);
	bp[0x210] = 0xFF;			/* loader: none of the known */
	rd16(bp, 0x1FA) = 0xFFFF;		/* video mode: as it is */

	if (strlen(cmdline) >= CMDLINE_MAX)
		die("kernel command line too long");
	strcpy((char *)ram + CMDLINE_GPA, cmdline);
	rd32(bp, 0x228) = (uint32_t)CMDLINE_GPA;

	if (ipath != NULL) {
		initrd = read_file(ipath, &isize);
		initrd_gpa = (ram_size - isize) & ~0xFFFULL;
		if (initrd_gpa < load + init_size)
			die("initramfs does not fit in guest memory");
		memcpy(ram + initrd_gpa, initrd, isize);
		rd32(bp, 0x218) = (uint32_t)initrd_gpa;
		rd32(bp, 0x21C) = (uint32_t)isize;
		free(initrd);
	}

	e820_add(bp, 0, LOWMEM_END, 1);
	e820_add(bp, LOWMEM_END, HIGHMEM_START - LOWMEM_END, 2);
	e820_add(bp, HIGHMEM_START, ram_size - HIGHMEM_START, 1);

	free(kernel);
	return load + 0x200;			/* startup_64 */
}

/* Long mode with the first 4G mapped one to one, as the protocol asks. */
static void
setup_cpu(uint64_t entry)
{
	struct nvmm_x64_state *st = vcpu.state;
	uint64_t *pml4 = (uint64_t *)(void *)(ram + PML4_GPA);
	uint64_t *pdpt = pml4 + 512;
	uint64_t *pd = pdpt + 512;
	uint64_t *gdt = (uint64_t *)(void *)(ram + GDT_GPA);
	struct nvmm_x64_state_seg seg;
	unsigned int i;

	memset(pml4, 0, 6 * 4096);
	pml4[0] = (PML4_GPA + 0x1000) | 0x03;
	for (i = 0; i < 4; i++)
		pdpt[i] = (PML4_GPA + 0x2000 + i * 0x1000ULL) | 0x03;
	for (i = 0; i < 4 * 512; i++)
		pd[i] = ((uint64_t)i << 21) | 0x83;	/* 2M pages */

	gdt[0] = 0;
	gdt[1] = 0;
	gdt[2] = 0x00AF9B000000FFFFULL;		/* 0x10: 64-bit code */
	gdt[3] = 0x00CF93000000FFFFULL;		/* 0x18: data */

	if (nvmm_vcpu_getstate(&mach, &vcpu, NVMM_X64_STATE_ALL) == -1)
		die("getstate: %s", strerror(errno));

	memset(&seg, 0, sizeof(seg));
	seg.selector = 0x10;
	seg.attrib.type = 0xB;
	seg.attrib.s = 1;
	seg.attrib.p = 1;
	seg.attrib.l = 1;
	seg.attrib.g = 1;
	seg.limit = 0xFFFFFFFF;
	st->segs[NVMM_X64_SEG_CS] = seg;

	seg.selector = 0x18;
	seg.attrib.type = 0x3;
	seg.attrib.l = 0;
	seg.attrib.def = 1;
	st->segs[NVMM_X64_SEG_DS] = seg;
	st->segs[NVMM_X64_SEG_ES] = seg;
	st->segs[NVMM_X64_SEG_SS] = seg;
	st->segs[NVMM_X64_SEG_FS] = seg;
	st->segs[NVMM_X64_SEG_GS] = seg;

	st->segs[NVMM_X64_SEG_GDT].base = GDT_GPA;
	st->segs[NVMM_X64_SEG_GDT].limit = 4 * 8 - 1;
	st->segs[NVMM_X64_SEG_IDT].base = 0;
	st->segs[NVMM_X64_SEG_IDT].limit = 0;

	memset(&seg, 0, sizeof(seg));
	seg.attrib.type = 0xB;			/* busy 64-bit TSS */
	seg.attrib.p = 1;
	seg.limit = 0x67;
	st->segs[NVMM_X64_SEG_TR] = seg;

	st->crs[NVMM_X64_CR_CR0] = 0x80050033;	/* PG WP NE ET MP PE */
	st->crs[NVMM_X64_CR_CR3] = PML4_GPA;
	st->crs[NVMM_X64_CR_CR4] = 0x20;	/* PAE */
	st->msrs[NVMM_X64_MSR_EFER] = 0x500;	/* LMA LME */

	memset(st->gprs, 0, sizeof(st->gprs));
	st->gprs[NVMM_X64_GPR_RIP] = entry;
	st->gprs[NVMM_X64_GPR_RSI] = BOOT_PARAMS_GPA;
	st->gprs[NVMM_X64_GPR_RFLAGS] = 0x2;

	if (nvmm_vcpu_setstate(&mach, &vcpu, NVMM_X64_STATE_SEGS |
	    NVMM_X64_STATE_GPRS | NVMM_X64_STATE_CRS |
	    NVMM_X64_STATE_MSRS) == -1)
		die("setstate: %s", strerror(errno));
}

/* -------------------------------------------------------------------------- */
/* Running. */

static void
kick_handler(int sig)
{
	(void)sig;
}

/*
 * The vCPU only returns from the kernel when the guest needs something or
 * the host interrupts it. A guest that is busy computing does neither often
 * enough for a 1 kHz timer, so poke the vCPU thread at that rate: the signal
 * interrupts the CPU it is on, which is all it takes.
 */
static void *
kicker(void *arg)
{
	(void)arg;
	while (running) {
		usleep(1000);
		pthread_kill(vcpu_thread, SIGUSR1);
	}
	return NULL;
}

static void
set_gpr(int reg, uint64_t val)
{
	if (nvmm_vcpu_getstate(&mach, &vcpu, NVMM_X64_STATE_GPRS) == -1)
		die("getstate: %s", strerror(errno));
	vcpu.state->gprs[reg] = val;
	if (nvmm_vcpu_setstate(&mach, &vcpu, NVMM_X64_STATE_GPRS) == -1)
		die("setstate: %s", strerror(errno));
}

static void
dump_and_die(const char *why)
{
	const struct nvmm_x64_state *st = vcpu.state;

	(void)nvmm_vcpu_getstate(&mach, &vcpu, NVMM_X64_STATE_ALL);
	die("%s: rip %#llx rsp %#llx cr0 %#llx cr2 %#llx cr3 %#llx cr4 %#llx "
	    "efer %#llx", why,
	    (unsigned long long)st->gprs[NVMM_X64_GPR_RIP],
	    (unsigned long long)st->gprs[NVMM_X64_GPR_RSP],
	    (unsigned long long)st->crs[NVMM_X64_CR_CR0],
	    (unsigned long long)st->crs[NVMM_X64_CR_CR2],
	    (unsigned long long)st->crs[NVMM_X64_CR_CR3],
	    (unsigned long long)st->crs[NVMM_X64_CR_CR4],
	    (unsigned long long)st->msrs[NVMM_X64_MSR_EFER]);
}

/* Hand the guest its next interrupt, or ask to be told when it can take it. */
static void
deliver_interrupts(void)
{
	const struct nvmm_vcpu_exit *exit = vcpu.exit;

	if (!pic_pending())
		return;

	if ((exit->exitstate.rflags & 0x200) && !exit->exitstate.int_shadow &&
	    !exit->exitstate.evt_pending) {
		vcpu.event->type = NVMM_VCPU_EVENT_INTR;
		vcpu.event->vector = pic_ack();
		if (nvmm_vcpu_inject(&mach, &vcpu) == -1)
			die("inject: %s", strerror(errno));
		return;
	}

	if (!exit->exitstate.int_window_exiting) {
		if (nvmm_vcpu_getstate(&mach, &vcpu, NVMM_X64_STATE_INTR) == -1)
			die("getstate: %s", strerror(errno));
		vcpu.state->intr.int_window_exiting = 1;
		if (nvmm_vcpu_setstate(&mach, &vcpu, NVMM_X64_STATE_INTR) == -1)
			die("setstate: %s", strerror(errno));
	}
}

/* The guest is idle: wait for the timer or the keyboard. */
static void
idle(void)
{
	struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
	uint64_t ns;

	for (;;) {
		console_poll();
		ns = pit_poll();
		if (net_rx_ready)
			net_rx(netdev);
		if (pic_pending() || !running)
			return;
		if ((vcpu.exit->exitstate.rflags & 0x200) == 0) {
			/* Halted with interrupts off: nothing can wake it. */
			fprintf(stderr, "\r\nnvmm-run: the guest halted\r\n");
			running = false;
			return;
		}
		if (ns == 0 || ns > 50000000)
			ns = 50000000;
		(void)poll(&pfd, 1, (int)(ns / 1000000) + 1);
	}
}

static void
run(void)
{
	struct nvmm_vcpu_exit *exit = vcpu.exit;
	unsigned long nexits = 0;

	/* Before the first run there is no exit state: interrupts are off. */
	memset(&exit->exitstate, 0, sizeof(exit->exitstate));

	while (running) {
		console_poll();
		(void)pit_poll();
		if (net_rx_ready)
			net_rx(netdev);
		deliver_interrupts();

		if (nvmm_vcpu_run(&mach, &vcpu) == -1) {
			if (errno == EINTR)
				continue;
			die("vcpu run: %s", strerror(errno));
		}
		nexits++;

		switch (exit->reason) {
		case NVMM_VCPU_EXIT_NONE:
		case NVMM_VCPU_EXIT_INT_READY:
			break;
		case NVMM_VCPU_EXIT_IO:
			if (nvmm_assist_io(&mach, &vcpu) == -1)
				dump_and_die("I/O emulation failed");
			break;
		case NVMM_VCPU_EXIT_MEMORY:
			if (nvmm_assist_mem(&mach, &vcpu) == -1)
				dump_and_die("memory access emulation failed");
			break;
		case NVMM_VCPU_EXIT_HALTED:
			idle();
			break;
		case NVMM_VCPU_EXIT_RDMSR:
			if (verbose)
				fprintf(stderr, "[rdmsr %#x]\r\n",
				    exit->u.rdmsr.msr);
			(void)nvmm_vcpu_getstate(&mach, &vcpu,
			    NVMM_X64_STATE_GPRS);
			vcpu.state->gprs[NVMM_X64_GPR_RAX] = 0;
			vcpu.state->gprs[NVMM_X64_GPR_RDX] = 0;
			vcpu.state->gprs[NVMM_X64_GPR_RIP] = exit->u.rdmsr.npc;
			(void)nvmm_vcpu_setstate(&mach, &vcpu,
			    NVMM_X64_STATE_GPRS);
			break;
		case NVMM_VCPU_EXIT_WRMSR:
			if (verbose)
				fprintf(stderr, "[wrmsr %#x = %#llx]\r\n",
				    exit->u.wrmsr.msr,
				    (unsigned long long)exit->u.wrmsr.val);
			set_gpr(NVMM_X64_GPR_RIP, exit->u.wrmsr.npc);
			break;
		case NVMM_VCPU_EXIT_MONITOR:
		case NVMM_VCPU_EXIT_MWAIT:
			set_gpr(NVMM_X64_GPR_RIP, exit->u.insn.npc);
			break;
		case NVMM_VCPU_EXIT_SHUTDOWN:
			dump_and_die("the guest triple-faulted");
			break;
		default:
			fprintf(stderr, "\r\nnvmm-run: exit reason %#llx "
			    "(hw %#llx)\r\n",
			    (unsigned long long)exit->reason,
			    (unsigned long long)exit->u.inv.hwcode);
			dump_and_die("unexpected exit");
		}
	}

	if (verbose)
		fprintf(stderr, "\r\n[%lu returns from the kernel]\r\n", nexits);
}

static void
usage(void)
{
	fprintf(stderr, "usage: nvmm-run -k vmlinuz [-i initramfs] "
	    "[-d disk.img] [-n] [-m megabytes] [-a \"extra cmdline\"] [-v]\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	const char *kpath = NULL, *ipath = NULL, *dpath = NULL, *extra = "";
	struct nvmm_assist_callbacks cbs = { io_callback, mem_callback };
	struct nvmm_vcpu_conf_cpuid cpuid;
	struct sigaction sa;
	char cmdline[CMDLINE_MAX];
	uint64_t tsc_hz = 0, entry;
	size_t len = sizeof(tsc_hz);
	pthread_t kick;
	long mb = 512;
	bool want_net = false;
	int ch;

	while ((ch = getopt(argc, argv, "k:i:d:nm:a:v")) != -1) {
		switch (ch) {
		case 'k': kpath = optarg; break;
		case 'i': ipath = optarg; break;
		case 'd': dpath = optarg; break;
		case 'n': want_net = true; break;
		case 'm': mb = atol(optarg); break;
		case 'a': extra = optarg; break;
		case 'v': verbose = true; break;
		default: usage();
		}
	}
	if (kpath == NULL || mb < 64 || mb > MEM_MAX_MB)
		usage();
	ram_size = (uint64_t)mb << 20;

	if (nvmm_init() == -1)
		die("cannot open /dev/nvmm: %s", strerror(errno));
	if (nvmm_machine_create(&mach) == -1)
		die("machine create: %s", strerror(errno));
	if (nvmm_vcpu_create(&mach, 0, &vcpu) == -1)
		die("vcpu create: %s", strerror(errno));
	if (nvmm_vcpu_configure(&mach, &vcpu, NVMM_VCPU_CONF_CALLBACKS,
	    &cbs) == -1)
		die("vcpu configure: %s", strerror(errno));

	/* No local APIC: hide it, so the kernel does not go looking. */
	memset(&cpuid, 0, sizeof(cpuid));
	cpuid.mask = 1;
	cpuid.leaf = 0x00000001;
	cpuid.u.mask.del.edx = 1u << 9;				/* APIC */
	cpuid.u.mask.del.ecx = (1u << 21) | (1u << 24);	/* x2APIC, deadline */
	if (nvmm_vcpu_configure(&mach, &vcpu, NVMM_VCPU_CONF_CPUID,
	    &cpuid) == -1)
		die("cpuid configure: %s", strerror(errno));

	ram = mmap(NULL, ram_size, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (ram == MAP_FAILED)
		die("mmap: %s", strerror(errno));
	if (nvmm_hva_map(&mach, (uintptr_t)ram, ram_size) == -1)
		die("cannot get %ld MB of guest memory: %s", mb,
		    strerror(errno));
	if (nvmm_gpa_map(&mach, (uintptr_t)ram, 0, ram_size,
	    PROT_READ | PROT_WRITE | PROT_EXEC) == -1)
		die("gpa map: %s", strerror(errno));

	/*
	 * With no ACPI and no APIC the kernel has only the old timer to
	 * measure the CPU's clock against. Tell it instead, and tell it to
	 * trust the answer.
	 */
	(void)sysctlbyname("machdep.tsc.frequency", &tsc_hz, &len, NULL, 0);
	snprintf(cmdline, sizeof(cmdline),
	    "console=ttyS0 earlyprintk=serial,ttyS0 nolapic noapic acpi=off "
	    "pci=off reboot=k panic=-1 tsc=reliable clocksource=tsc "
	    "i8042.noaux i8042.nokbd");
	if (tsc_hz != 0) {
		snprintf(cmdline + strlen(cmdline),
		    sizeof(cmdline) - strlen(cmdline), " tsc_early_khz=%llu",
		    (unsigned long long)(tsc_hz / 1000));
	}
	/* The vmnet callback signals this thread; it must be known first. */
	vcpu_thread = pthread_self();
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = kick_handler;
	sigaction(SIGUSR1, &sa, NULL);

	if (dpath != NULL)
		blk_add(dpath, cmdline, sizeof(cmdline));
	if (want_net)
		net_add(cmdline, sizeof(cmdline));
	snprintf(cmdline + strlen(cmdline), sizeof(cmdline) - strlen(cmdline),
	    " %s", extra);

	entry = load_linux(kpath, ipath, cmdline);
	setup_cpu(entry);

	console_init();
	if (pthread_create(&kick, NULL, kicker, NULL) != 0)
		die("cannot start the timer thread");

	run();

	running = false;
	pthread_join(kick, NULL);
	(void)nvmm_vcpu_destroy(&mach, &vcpu);
	(void)nvmm_machine_destroy(&mach);
	fprintf(stderr, "\r\n");
	return exit_code;
}
