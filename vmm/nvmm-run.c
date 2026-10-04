/*
 * nvmm-run.c — a small virtual machine monitor on libnvmm, in place of QEMU.
 *
 * It boots a Linux kernel directly, with no firmware, and gives it the least
 * a kernel needs: a serial console, the legacy interrupt controller, the
 * legacy timer and a clock. The kernel is told not to look for anything else
 * (no ACPI, no PCI), which is what keeps this small.
 *
 * With one virtual CPU, the default, there is no local APIC either: NVMM
 * leaves every device to userland, and that is by far the largest of them.
 * With more (-c), each CPU gets a local APIC and the machine an I/O APIC,
 * described to the kernel by an MP table, and each CPU runs on a thread.
 *
 * A disk, if given, is a virtio block device: the kernel finds it through
 * its command line, which is how virtio's memory-mapped transport is used
 * on machines with no firmware tables to describe it.
 *
 * A network card, if asked for, is a virtio one. Its frames go either to
 * macOS's own vmnet (-n vmnet, needs root) or over a socket to a helper
 * that does the networking in userland (-n /path/to/socket; see the comment
 * on the socket backend below). Either way the guest gets its address by
 * DHCP and a route out, with nothing to configure.
 *
 * Usage (needs /dev/nvmm):
 *   nvmm-run -k vmlinuz [-i initramfs] [-d disk.img] [-n vmnet|socket]
 *            [-c cpus] [-m megabytes] [-a "extra cmdline"]
 *
 * The console is this terminal. Ctrl-A then x quits.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
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
static uint8_t *ram;
static uint64_t ram_size;

/*
 * One of these per virtual CPU. Everything in this file that is not marked
 * otherwise is protected by 'big': a CPU's thread holds it whenever it is
 * not inside the guest, and so do the timer and network threads when they
 * touch a device.
 */
#define MAX_CPUS	16

struct lapic {
	uint32_t tpr, ldr, dfr, svr, esr, icr_lo, icr_hi;
	uint32_t lvt[6];	/* timer, thermal, perf, LINT0, LINT1, error */
	uint32_t isr[8], tmr[8], irr[8];
	uint32_t divide, init_count;
	uint64_t deadline_ns;	/* 0: the timer is not running */
	uint64_t base_msr;
};

struct cpu {
	unsigned int id;
	struct nvmm_vcpu vcpu;
	pthread_t thread;
	pthread_cond_t cond;
	struct lapic apic;
	bool in_guest;		/* inside nvmm_vcpu_run() */
	bool stopped;		/* halted with interrupts off */
	bool wait_sipi;		/* an AP that has not been started */
	bool sipi_pending;
	bool nmi_pending;
	uint8_t sipi_vector;
	unsigned long nruns;
};

static struct cpu cpus[MAX_CPUS];
static unsigned int ncpus = 1;
static bool apic_mode;		/* more than one CPU */
static pthread_mutex_t big = PTHREAD_MUTEX_INITIALIZER;

static void cpu_wake(struct cpu *);
static void ioapic_set_irq(int);
static bool timer_irq_waiting(void);
static void clock_rearm(uint64_t);
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

/*
 * Everything a guest waits on goes through this program, so anything here
 * that takes long freezes the guest for as long. Report it when it happens:
 * the guest only knows that it lost time, not where.
 */
#define STALL_NS	200000000ULL		/* 200 ms */

static void
stall_check(uint64_t since, const char *what, unsigned long detail)
{
	const uint64_t now = now_ns();
	const uint64_t took = (now > since) ? now - since : 0;

	if (took >= STALL_NS)
		fprintf(stderr, "\r\n[nvmm-run: %llu ms in %s %#lx]\r\n",
		    (unsigned long long)(took / 1000000), what, detail);
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

/*
 * A device raises its line. The line goes to the 8259s and, when there is
 * one, to the I/O APIC; the guest decides which of the two it listens to.
 */
static void
irq_raise(int line)
{
	if (line < 8)
		pics[0].irr |= (uint8_t)(1u << line);
	else
		pics[1].irr |= (uint8_t)(1u << (line - 8));
	if (apic_mode)
		ioapic_set_irq(line);
	cpu_wake(&cpus[0]);	/* the 8259s interrupt the first CPU */
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
	if (ch == 0) {
		c->next_irq_ns = c->start_ns + pit_period_ns(c);
		clock_rearm(c->next_irq_ns);
	}
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
		/*
		 * An interrupt request is one bit: a tick raised while the
		 * last is still waiting to be taken would vanish into it.
		 * Hold it back instead; it is made up below.
		 */
		if ((c->mode == 2 || c->mode == 3) && timer_irq_waiting())
			return 1;
		irq_raise(0);
		if (c->mode == 2 || c->mode == 3) {
			const uint64_t per = pit_period_ns(c);

			/*
			 * One period per call, so that a tick that came late
			 * is made up for by the next coming early: the guest
			 * measures other clocks against this one by counting
			 * its interrupts. More than a few periods of debt is
			 * written off, though: paid back in a burst, it would
			 * make this clock run fast instead.
			 */
			c->next_irq_ns += per;
			if (now > c->next_irq_ns + 4 * per)
				c->next_irq_ns = now + per;
			if (c->next_irq_ns <= now)
				return 1;
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
static bool console_eof;
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
	if (n == 0)
		console_eof = true;	/* nothing more will ever come */
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
	pthread_t worker;		/* block: the thread that does the I/O */
	unsigned long resets;		/* times the guest has reset the device */
	size_t max_packet;		/* network: largest frame */
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
	d->resets++;
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

/*
 * Requests are served by a thread of the device's own, which lets go of
 * 'big' while it reads or writes. Done on the CPU's thread with the lock
 * held, one slow write to the host's disk stops every CPU of the guest for
 * as long as it takes: seen as a 22-second stall under load.
 */
static pthread_cond_t blk_cond = PTHREAD_COND_INITIALIZER;

static void
blk_notify(struct virtio_dev *d, unsigned int qi)
{
	(void)d;
	(void)qi;
	pthread_cond_broadcast(&blk_cond);
}

static void *
blk_worker(void *arg)
{
	static struct iovec iov[VIRTIO_MAXDEV][VQ_IOV_MAX];
	struct virtio_dev *d = arg;
	struct iovec *v = iov[d - vdevs];
	struct virtq *q = &d->vq[0];
	unsigned long resets;
	unsigned int nout;
	uint16_t head;
	int n;

	pthread_mutex_lock(&big);
	while (running) {
		struct { uint32_t type, rsvd; uint64_t sector; } hdr;
		uint8_t *status;
		uint32_t written = 1;
		ssize_t r = 0;

		n = q->ready ? vq_pop(q, v, &nout, &head) : 0;
		if (n < 0) {
			fprintf(stderr,
			    "[virtio-blk: bad request from the guest]\r\n");
			n = 0;
		}
		if (n == 0) {
			struct timespec ts;
			struct timeval tv;

			/* Woken by a notify; the timeout is for shutdown. */
			gettimeofday(&tv, NULL);
			ts.tv_sec = tv.tv_sec + 1;
			ts.tv_nsec = tv.tv_usec * 1000;
			(void)pthread_cond_timedwait(&blk_cond, &big, &ts);
			continue;
		}

		/* A header to read, a status byte to write, data between. */
		if (nout < 1 || v[0].iov_len != sizeof(hdr) ||
		    (unsigned int)n == nout || v[n - 1].iov_len != 1) {
			vq_push(d, q, head, 0);
			continue;
		}
		memcpy(&hdr, v[0].iov_base, sizeof(hdr));
		status = v[n - 1].iov_base;

		/* The buffers are the guest's own memory: no lock needed. */
		resets = d->resets;
		pthread_mutex_unlock(&big);
		switch (hdr.type) {
		case 0:					/* read */
			r = blk_rw(d->fd, false, v + nout, n - 1 - (int)nout,
			    (off_t)(hdr.sector * 512));
			if (r >= 0)
				written += (uint32_t)r;
			break;
		case 1:					/* write */
			r = blk_rw(d->fd, true, v + 1, (int)nout - 1,
			    (off_t)(hdr.sector * 512));
			break;
		case 4:					/* flush */
			r = fsync(d->fd);
			break;
		case 8:					/* identify */
			if ((unsigned int)n - nout == 2 &&
			    v[nout].iov_len >= 20) {
				memset(v[nout].iov_base, 0, 20);
				memcpy(v[nout].iov_base, "nvmm-run", 8);
				written += 20;
			}
			break;
		default:
			r = -2;
			break;
		}
		*status = (r == -2) ? 2 : (r < 0) ? 1 : 0;
		pthread_mutex_lock(&big);

		/* Unless the guest reset the device meanwhile. */
		if (q->ready && d->resets == resets)
			vq_push(d, q, head, written);
	}
	pthread_mutex_unlock(&big);
	return NULL;
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

	if (pthread_create(&d->worker, NULL, blk_worker, d) != 0)
		die("cannot start the disk thread");
}

/* ---- the network card ---- */

#define VIRTIO_NET_F_MAC	(1ULL << 5)
#define NET_HDR_LEN		12	/* struct virtio_net_hdr_v1 */
#define NET_RXQ			0
#define NET_TXQ			1
#define NET_FRAME_MAX		65536

/*
 * What carries the card's frames. Two are provided: macOS's vmnet, and a
 * stream socket to a helper process that does the networking in userland.
 * 'recv' must not block: it returns 0 when nothing is waiting.
 */
struct net_backend {
	void (*send)(const uint8_t *, size_t);
	size_t (*recv)(uint8_t *, size_t);
};

static struct virtio_dev *netdev;
static struct net_backend net_be;

/* Guest to host: each request is a header and one frame. */
static void
net_tx(struct virtio_dev *d)
{
	static struct iovec iov[VQ_IOV_MAX];
	static uint8_t frame[NET_FRAME_MAX];
	struct virtq *q = &d->vq[NET_TXQ];
	unsigned int nout;
	uint16_t head;
	int n, i;

	while ((n = vq_pop(q, iov, &nout, &head)) > 0) {
		size_t len = 0, skip = NET_HDR_LEN;

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
		if (len > 0 && len <= d->max_packet && net_be.send != NULL)
			net_be.send(frame, len);
		vq_push(d, q, head, 0);
	}
}

/* Host to guest: as many waiting frames as the guest has buffers for. */
static void
net_rx(struct virtio_dev *d)
{
	static struct iovec iov[VQ_IOV_MAX];
	static uint8_t frame[NET_HDR_LEN + NET_FRAME_MAX];
	struct virtq *q = &d->vq[NET_RXQ];

	if (!q->ready || net_be.recv == NULL)
		return;

	for (;;) {
		unsigned int nout;
		uint16_t head, last = q->last_avail;
		size_t len, done = 0;
		int n, i;

		/* Only take a frame once there is somewhere to put it. */
		n = vq_pop(q, iov, &nout, &head);
		if (n <= 0)
			return;
		len = net_be.recv(frame + NET_HDR_LEN, d->max_packet);
		if (len == 0) {
			q->last_avail = last;	/* nothing: put it back */
			return;
		}

		memset(frame, 0, NET_HDR_LEN);
		frame[10] = 1;			/* num_buffers = 1 */
		len += NET_HDR_LEN;
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

/* The device itself, behind whatever carries its frames. */
static void
net_finish(struct virtio_dev *d, const uint8_t *mac, char *cmdline,
    size_t cmdlen)
{
	d->id = VIRTIO_ID_NET;
	d->irq = 5 + (int)nvdevs;
	d->features = VIRTIO_F_VERSION_1 | VIRTIO_NET_F_MAC;
	d->notify = net_notify;
	memcpy(d->config, mac, 6);
	netdev = d;

	snprintf(cmdline + strlen(cmdline), cmdlen - strlen(cmdline),
	    " virtio_mmio.device=4K@%#llx:%d",
	    (unsigned long long)(VIRTIO_BASE + nvdevs * VIRTIO_STRIDE), d->irq);
	nvdevs++;
}

/* A new frame is waiting. Called on the backend's own thread. */
static void
net_wake(void)
{
	uint64_t t0 = now_ns();

	pthread_mutex_lock(&big);
	stall_check(t0, "the network thread waiting for the lock", 0);
	t0 = now_ns();
	net_rx(netdev);
	stall_check(t0, "network receive", 0);
	pthread_mutex_unlock(&big);
}

/* -- backend: nothing. A card with the cable unplugged, for testing. -- */

static const uint8_t net_local_mac[6] = { 0x5A, 0x94, 0xEF, 0xE4, 0x0C, 0xEE };

static void
net_add_unplugged(char *cmdline, size_t cmdlen)
{
	struct virtio_dev *d = &vdevs[nvdevs];

	d->max_packet = 1514;
	net_finish(d, net_local_mac, cmdline, cmdlen);
}

/*
 * -- backend: a stream socket --
 *
 * Each frame is preceded by its length as a 32-bit big-endian number, the
 * framing QEMU's socket network devices use, so this talks to anything
 * written for those: gvproxy (-listen-qemu), for one, which gives the guest
 * DHCP, DNS and a route out with no privileges on the host.
 *
 * A thread reads frames into a ring; the vCPU thread empties it.
 */

#define SOCK_RING	256
#define SOCK_FRAME	2048

static int sock_fd = -1;
static pthread_mutex_t sock_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint16_t len; uint8_t data[SOCK_FRAME]; } sock_ring[SOCK_RING];
static unsigned int sock_head, sock_tail;

static bool
sock_read_all(void *buf, size_t len)
{
	size_t done = 0;
	ssize_t r;

	while (done < len) {
		r = read(sock_fd, (char *)buf + done, len - done);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
		done += (size_t)r;
	}
	return true;
}

static void *
sock_reader(void *arg)
{
	static uint8_t discard[NET_FRAME_MAX];
	uint8_t hdr[4];
	uint32_t len;

	(void)arg;
	for (;;) {
		if (!sock_read_all(hdr, 4))
			break;
		len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
		    ((uint32_t)hdr[2] << 8) | hdr[3];
		if (len > sizeof(discard))
			break;				/* out of step */

		pthread_mutex_lock(&sock_lock);
		if (len <= SOCK_FRAME && sock_head - sock_tail < SOCK_RING) {
			const unsigned int slot = sock_head % SOCK_RING;

			pthread_mutex_unlock(&sock_lock);
			/* Only this thread advances the head. */
			if (!sock_read_all(sock_ring[slot].data, len))
				break;
			sock_ring[slot].len = (uint16_t)len;
			pthread_mutex_lock(&sock_lock);
			sock_head++;
			pthread_mutex_unlock(&sock_lock);
			net_wake();
		} else {
			pthread_mutex_unlock(&sock_lock);
			if (!sock_read_all(discard, len))	/* full: drop */
				break;
		}
	}
	fprintf(stderr, "\r\nnvmm-run: the network helper went away\r\n");
	return NULL;
}

static size_t
sock_recv(uint8_t *buf, size_t max)
{
	size_t len = 0;

	pthread_mutex_lock(&sock_lock);
	if (sock_head != sock_tail) {
		const unsigned int slot = sock_tail % SOCK_RING;

		len = sock_ring[slot].len;
		if (len > max)
			len = 0;			/* cannot be: drop it */
		else
			memcpy(buf, sock_ring[slot].data, len);
		sock_tail++;
	}
	pthread_mutex_unlock(&sock_lock);
	return len;
}

static void
sock_send(const uint8_t *buf, size_t len)
{
	const uint8_t hdr[4] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16),
	    (uint8_t)(len >> 8), (uint8_t)len };
	struct iovec iov[2] = {
		{ .iov_base = (void *)(uintptr_t)hdr, .iov_len = 4 },
		{ .iov_base = (void *)(uintptr_t)buf, .iov_len = len },
	};
	size_t done = 0, total = 4 + len;
	ssize_t r;

	/* A frame must go out whole, or the stream loses its framing. */
	while (done < total) {
		r = writev(sock_fd, iov, 2);
		if (r < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (r <= 0)
			return;
		done += (size_t)r;
		if (done >= total)
			break;
		if ((size_t)r >= iov[0].iov_len) {
			r -= (ssize_t)iov[0].iov_len;
			iov[0].iov_len = 0;
			iov[1].iov_base = (char *)iov[1].iov_base + r;
			iov[1].iov_len -= (size_t)r;
		} else {
			iov[0].iov_base = (char *)iov[0].iov_base + r;
			iov[0].iov_len -= (size_t)r;
		}
	}
}

static void
net_add_socket(const char *path, char *cmdline, size_t cmdlen)
{
	struct virtio_dev *d = &vdevs[nvdevs];
	struct sockaddr_un sun;
	pthread_t t;

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(sun.sun_path))
		die("socket path too long");
	strcpy(sun.sun_path, path);
	sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock_fd == -1 ||
	    connect(sock_fd, (struct sockaddr *)&sun, sizeof(sun)) == -1)
		die("cannot connect to the network helper at %s: %s", path,
		    strerror(errno));
	if (pthread_create(&t, NULL, sock_reader, NULL) != 0)
		die("cannot start the network thread");

	net_be.send = sock_send;
	net_be.recv = sock_recv;
	d->max_packet = 1514;
	net_finish(d, net_local_mac, cmdline, cmdlen);
}

/* -- backend: macOS vmnet. Needs root. -- */

static interface_ref vmnet_if;
static size_t vmnet_max;

static void
vmnet_send(const uint8_t *buf, size_t len)
{
	struct iovec out = { .iov_base = (void *)(uintptr_t)buf, .iov_len = len };
	struct vmpktdesc pkt;
	int count = 1;

	memset(&pkt, 0, sizeof(pkt));
	pkt.vm_pkt_size = len;
	pkt.vm_pkt_iov = &out;
	pkt.vm_pkt_iovcnt = 1;
	(void)vmnet_write(vmnet_if, &pkt, &count);
}

static size_t
vmnet_recv(uint8_t *buf, size_t max)
{
	struct iovec in = { .iov_base = buf, .iov_len = max };
	struct vmpktdesc pkt;
	int count = 1;

	memset(&pkt, 0, sizeof(pkt));
	pkt.vm_pkt_size = max;
	pkt.vm_pkt_iov = &in;
	pkt.vm_pkt_iovcnt = 1;
	if (vmnet_read(vmnet_if, &pkt, &count) != VMNET_SUCCESS || count < 1)
		return 0;
	return pkt.vm_pkt_size;
}

static void
net_add_vmnet(char *cmdline, size_t cmdlen)
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
	vmnet_if = vmnet_start_interface(desc, q,
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
	if (vmnet_if != NULL && dispatch_semaphore_wait(sem,
	    dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC)) != 0)
		die("vmnet did not answer within 20 seconds (vmm/vmnet-probe.c "
		    "tests it on its own)");
	if (vmnet_if == NULL || status != VMNET_SUCCESS)
		die("cannot start a vmnet interface (status %d); vmnet "
		    "needs root", (int)status);

	/* Frames arrive on vmnet's own thread; the vCPU thread moves them. */
	vmnet_interface_set_event_callback(vmnet_if,
	    VMNET_INTERFACE_PACKETS_AVAILABLE, q,
	    ^(interface_event_t ev, xpc_object_t params) {
		(void)ev;
		(void)params;
		net_wake();
	});

	vmnet_max = (maxpkt != 0 && maxpkt <= NET_FRAME_MAX) ?
	    (size_t)maxpkt : 1514;
	net_be.send = vmnet_send;
	net_be.recv = vmnet_recv;
	d->max_packet = vmnet_max;
	fprintf(stderr, "nvmm-run: network up, MAC "
	    "%02x:%02x:%02x:%02x:%02x:%02x, largest frame %zu\n", mac[0],
	    mac[1], mac[2], mac[3], mac[4], mac[5], d->max_packet);
	net_finish(d, mac, cmdline, cmdlen);
}

/* -------------------------------------------------------------------------- */
/*
 * Local APICs and the I/O APIC, for machines with more than one CPU.
 *
 * Only what Linux uses: fixed and lowest-priority interrupts, physical and
 * flat logical destinations, the timer, and INIT and startup IPIs to bring
 * the other CPUs up. The timer counts nanoseconds, so its "bus" runs at
 * 1 GHz; the kernel measures that for itself.
 */

#define LAPIC_BASE	0xFEE00000ULL
#define IOAPIC_BASE	0xFEC00000ULL
#define IOAPIC_PINS	24

#define LVT_TIMER	0
#define LVT_LINT0	3
#define LVT_MASKED	(1u << 16)
#define LVT_PERIODIC	(1u << 17)

static struct {
	uint32_t regsel;
	uint32_t id;
	uint64_t rte[IOAPIC_PINS];
} ioapic;

static int
bits_highest(const uint32_t *w)
{
	int i;

	for (i = 7; i >= 0; i--) {
		if (w[i] != 0)
			return i * 32 + 31 - __builtin_clz(w[i]);
	}
	return -1;
}

static void
lapic_reset(struct cpu *c)
{
	struct lapic *a = &c->apic;
	int i;

	memset(a, 0, sizeof(*a));
	a->dfr = 0xFFFFFFFF;
	a->svr = 0xFF;
	for (i = 0; i < 6; i++)
		a->lvt[i] = LVT_MASKED;
	a->base_msr = LAPIC_BASE | 0x800 | (c->id == 0 ? 0x100 : 0);
}

/* The vector this CPU's APIC would deliver now, or -1. */
static int
lapic_pending(const struct cpu *c)
{
	const struct lapic *a = &c->apic;
	const int irr = bits_highest(a->irr), isr = bits_highest(a->isr);
	int ppr = (int)(a->tpr & 0xF0);

	if ((a->svr & 0x100) == 0 || irr < 0)
		return -1;
	if (isr >= 0 && (isr & 0xF0) > ppr)
		ppr = isr & 0xF0;
	return ((irr & 0xF0) > ppr) ? irr : -1;
}

static void
lapic_set_irq(struct cpu *c, uint8_t vector, bool level)
{
	struct lapic *a = &c->apic;

	if (vector < 16)
		return;
	a->irr[vector / 32] |= 1u << (vector % 32);
	if (level)
		a->tmr[vector / 32] |= 1u << (vector % 32);
	else
		a->tmr[vector / 32] &= ~(1u << (vector % 32));
	cpu_wake(c);
}

/* Does an interrupt addressed like this go to CPU c? */
static bool
lapic_match(const struct cpu *c, uint8_t dest, bool logical)
{
	if (!logical)
		return dest == 0xFF || dest == c->id;
	if (dest == 0xFF)
		return true;
	if ((c->apic.dfr >> 28) == 0xF)			/* flat */
		return ((c->apic.ldr >> 24) & dest) != 0;
	return (c->apic.ldr >> 24) == dest;		/* cluster: exact */
}

/*
 * Deliver one interrupt message, from the I/O APIC or from a CPU's
 * interrupt command register. 'self' is the sending CPU, or NULL.
 * shorthand: 0 use dest, 1 self, 2 all, 3 all but self.
 */
static void
apic_deliver(struct cpu *self, uint8_t dest, bool logical, unsigned int mode,
    uint8_t vector, bool level, unsigned int shorthand)
{
	unsigned int i;

	for (i = 0; i < ncpus; i++) {
		struct cpu *c = &cpus[i];

		if (shorthand == 1 && c != self)
			continue;
		if (shorthand == 3 && c == self)
			continue;
		if (shorthand == 0 && !lapic_match(c, dest, logical))
			continue;

		switch (mode) {
		case 0:				/* fixed */
			lapic_set_irq(c, vector, level);
			break;
		case 1:				/* lowest priority: the first */
			lapic_set_irq(c, vector, level);
			return;
		case 4:				/* NMI */
			c->nmi_pending = true;
			cpu_wake(c);
			break;
		case 5:				/* INIT: back to waiting */
			if (c != self && c->id != 0) {
				c->wait_sipi = true;
				c->sipi_pending = false;
				cpu_wake(c);
			}
			break;
		case 6:				/* startup */
			if (c->wait_sipi) {
				c->sipi_vector = vector;
				c->sipi_pending = true;
				cpu_wake(c);
			}
			break;
		}
	}
}

static void
ioapic_eoi(uint8_t vector)
{
	unsigned int i;

	for (i = 0; i < IOAPIC_PINS; i++) {
		if ((ioapic.rte[i] & 0xFF) == vector)
			ioapic.rte[i] &= ~(1ULL << 14);	/* remote IRR */
	}
}

/* Is the last timer tick still waiting for the guest to take it? */
static bool
timer_irq_waiting(void)
{
	const uint64_t rte = ioapic.rte[2];
	const unsigned int vec = (unsigned int)rte & 0xFF;
	unsigned int i;

	if (apic_mode && (rte & (1ULL << 16)) == 0) {
		for (i = 0; i < ncpus; i++) {
			if (cpus[i].apic.irr[vec / 32] & (1u << (vec % 32)))
				return true;
		}
		return false;
	}
	return (pics[0].irr & 1) != 0 && (pics[0].imr & 1) == 0;
}

/* An ISA line was raised. The timer's line 0 arrives on pin 2. */
static void
ioapic_set_irq(int line)
{
	const unsigned int pin = (line == 0) ? 2 : (unsigned int)line;
	uint64_t rte;
	bool level;

	if (pin >= IOAPIC_PINS)
		return;
	rte = ioapic.rte[pin];
	if (rte & (1ULL << 16))				/* masked */
		return;
	level = (rte & (1ULL << 15)) != 0;
	if (level) {
		if (rte & (1ULL << 14))
			return;		/* still being serviced */
		ioapic.rte[pin] |= 1ULL << 14;
	}
	apic_deliver(NULL, (uint8_t)(rte >> 56), (rte & (1ULL << 11)) != 0,
	    (unsigned int)(rte >> 8) & 7, (uint8_t)rte, level, 0);
}

static uint32_t
ioapic_read(uint64_t off)
{
	const uint32_t r = ioapic.regsel;

	if (off == 0x00)
		return r;
	if (off != 0x10)
		return 0;
	if (r == 0)
		return ioapic.id << 24;
	if (r == 1)
		return ((IOAPIC_PINS - 1) << 16) | 0x20;	/* version */
	if (r >= 0x10 && r < 0x10 + 2 * IOAPIC_PINS) {
		const uint64_t rte = ioapic.rte[(r - 0x10) / 2];

		return (uint32_t)((r & 1) ? rte >> 32 : rte);
	}
	return 0;
}

static void
ioapic_write(uint64_t off, uint32_t v)
{
	const uint32_t r = ioapic.regsel;

	if (off == 0x00) {
		ioapic.regsel = v & 0xFF;
	} else if (off == 0x40) {
		ioapic_eoi((uint8_t)v);
	} else if (off == 0x10) {
		if (r == 0) {
			ioapic.id = (v >> 24) & 0xF;
		} else if (r >= 0x10 && r < 0x10 + 2 * IOAPIC_PINS) {
			uint64_t *rte = &ioapic.rte[(r - 0x10) / 2];

			if (r & 1) {
				*rte = (*rte & 0xFFFFFFFFULL) | ((uint64_t)v << 32);
			} else {
				/* Delivery status and remote IRR are ours. */
				*rte = (*rte & 0xFFFFFFFF00005000ULL) |
				    (v & ~0x5000u);
			}
		}
	}
}

static unsigned int
lapic_divisor(const struct lapic *a)
{
	const unsigned int v = (a->divide & 3) | ((a->divide >> 1) & 4);

	return (v == 7) ? 1 : 2u << v;
}

static void
lapic_timer_start(struct lapic *a)
{
	if (a->init_count == 0) {
		a->deadline_ns = 0;
	} else {
		a->deadline_ns = now_ns() +
		    (uint64_t)a->init_count * lapic_divisor(a);
		clock_rearm(a->deadline_ns);
	}
}

/* Fire this CPU's timer if it is due. */
static void
lapic_timer_poll(struct cpu *c, uint64_t now)
{
	struct lapic *a = &c->apic;

	const uint8_t vec = (uint8_t)a->lvt[LVT_TIMER];

	if (a->deadline_ns == 0 || now < a->deadline_ns)
		return;
	if (a->lvt[LVT_TIMER] & LVT_PERIODIC) {
		const uint64_t per = (uint64_t)a->init_count * lapic_divisor(a);

		/*
		 * One period per call, held back while the last tick is
		 * still waiting, and with a few periods of debt at most:
		 * as for the 8254 and for the same reasons. The kernel
		 * checks each of these timers against the other by counting
		 * interrupts.
		 */
		if ((a->lvt[LVT_TIMER] & LVT_MASKED) == 0) {
			if (a->irr[vec / 32] & (1u << (vec % 32)))
				return;
			lapic_set_irq(c, vec, false);
		}
		a->deadline_ns += per;
		if (now > a->deadline_ns + 4 * per)
			a->deadline_ns = now + per;
	} else {
		if ((a->lvt[LVT_TIMER] & LVT_MASKED) == 0)
			lapic_set_irq(c, vec, false);
		a->deadline_ns = 0;
	}
}

static uint32_t
lapic_read(struct cpu *c, uint64_t off)
{
	struct lapic *a = &c->apic;
	uint64_t now, per;

	if (off >= 0x100 && off < 0x180)
		return a->isr[(off - 0x100) / 16];
	if (off >= 0x180 && off < 0x200)
		return a->tmr[(off - 0x180) / 16];
	if (off >= 0x200 && off < 0x280)
		return a->irr[(off - 0x200) / 16];
	if (off >= 0x320 && off <= 0x370)
		return a->lvt[(off - 0x320) / 16];

	switch (off) {
	case 0x020: return c->id << 24;
	case 0x030: return 0x00050014;		/* version, six LVT entries */
	case 0x080: return a->tpr;
	case 0x0A0: {
		const int isr = bits_highest(a->isr);
		uint32_t ppr = a->tpr & 0xF0;

		if (isr >= 0 && (uint32_t)(isr & 0xF0) > ppr)
			ppr = (uint32_t)isr & 0xF0;
		return ppr;
	}
	case 0x0D0: return a->ldr;
	case 0x0E0: return a->dfr;
	case 0x0F0: return a->svr;
	case 0x280: return a->esr;
	case 0x300: return a->icr_lo;
	case 0x310: return a->icr_hi;
	case 0x380: return a->init_count;
	case 0x390:
		if (a->init_count == 0)
			return 0;
		now = now_ns();
		per = (uint64_t)a->init_count * lapic_divisor(a);
		if (a->deadline_ns == 0)
			return 0;
		if (now >= a->deadline_ns)
			return (a->lvt[LVT_TIMER] & LVT_PERIODIC) ?
			    a->init_count : 0;
		(void)per;
		return (uint32_t)((a->deadline_ns - now) / lapic_divisor(a));
	case 0x3E0: return a->divide;
	default:    return 0;
	}
}

static void
lapic_write(struct cpu *c, uint64_t off, uint32_t v)
{
	struct lapic *a = &c->apic;
	int vec;

	if (off >= 0x320 && off <= 0x370) {
		a->lvt[(off - 0x320) / 16] = v;
		return;
	}
	switch (off) {
	case 0x080: a->tpr = v & 0xFF; break;
	case 0x0B0:				/* end of interrupt */
		vec = bits_highest(a->isr);
		if (vec >= 0) {
			a->isr[vec / 32] &= ~(1u << (vec % 32));
			if (a->tmr[vec / 32] & (1u << (vec % 32)))
				ioapic_eoi((uint8_t)vec);
		}
		break;
	case 0x0D0: a->ldr = v & 0xFF000000; break;
	case 0x0E0: a->dfr = v | 0x0FFFFFFF; break;
	case 0x0F0: a->svr = v & 0x1FF; break;
	case 0x280: a->esr = 0; break;
	case 0x310: a->icr_hi = v; break;
	case 0x300:
		a->icr_lo = v & ~(1u << 12);	/* never left pending */
		/* An INIT "de-assert" is a leftover of older APICs. */
		if (((v >> 8) & 7) == 5 && (v & (1u << 14)) == 0)
			break;
		apic_deliver(c, (uint8_t)(a->icr_hi >> 24),
		    (v & (1u << 11)) != 0, (v >> 8) & 7, (uint8_t)v, false,
		    (v >> 18) & 3);
		break;
	case 0x380:
		a->init_count = v;
		lapic_timer_start(a);
		break;
	case 0x3E0: a->divide = v & 0xB; break;
	}
}

/*
 * The MP table: how a kernel with no ACPI learns of the CPUs, the I/O APIC
 * and which pin each ISA interrupt arrives on. It goes where the BIOS would
 * have put it.
 */
#define MPFP_GPA	0x000F0000ULL
#define MPTABLE_GPA	0x000F0100ULL

static uint8_t
sum8(const uint8_t *p, size_t n)
{
	uint8_t s = 0;

	while (n-- > 0)
		s += *p++;
	return s;
}

static void
mptable_build(void)
{
	uint8_t *fp = ram + MPFP_GPA, *t = ram + MPTABLE_GPA, *e;
	const uint8_t ioapic_id = (uint8_t)ncpus;
	uint16_t count = 0;
	unsigned int i;

	memset(t, 0, 0x1000);
	memcpy(t, "PCMP", 4);
	t[6] = 4;					/* spec 1.4 */
	memcpy(t + 8, "NVMM    ", 8);
	memcpy(t + 16, "NVMM-RUN    ", 12);
	*(uint32_t *)(void *)(t + 36) = (uint32_t)LAPIC_BASE;
	e = t + 44;

	for (i = 0; i < ncpus; i++) {			/* processors */
		e[0] = 0;
		e[1] = (uint8_t)i;
		e[2] = 0x14;
		e[3] = (uint8_t)(1 | (i == 0 ? 2 : 0));	/* usable, boot */
		*(uint32_t *)(void *)(e + 4) = 0x00800F00;
		*(uint32_t *)(void *)(e + 8) = 0x00000201;	/* FPU, APIC */
		e += 20;
		count++;
	}
	e[0] = 1;					/* the ISA bus */
	e[1] = 0;
	memcpy(e + 2, "ISA   ", 6);
	e += 8;
	count++;

	e[0] = 2;					/* the I/O APIC */
	e[1] = ioapic_id;
	e[2] = 0x20;
	e[3] = 1;
	*(uint32_t *)(void *)(e + 4) = (uint32_t)IOAPIC_BASE;
	e += 8;
	count++;

	for (i = 0; i < 16; i++) {			/* ISA lines to pins */
		if (i == 2)
			continue;			/* the cascade */
		e[0] = 3;
		e[1] = 0;				/* a vectored interrupt */
		e[4] = 0;				/* from bus 0 */
		e[5] = (uint8_t)i;
		e[6] = ioapic_id;
		e[7] = (uint8_t)(i == 0 ? 2 : i);
		e += 8;
		count++;
	}
	e[0] = 4; e[1] = 3; e[4] = 0; e[5] = 0; e[6] = 0xFF; e[7] = 0;
	e += 8;						/* ExtINT on LINT0 */
	count++;
	e[0] = 4; e[1] = 1; e[4] = 0; e[5] = 0; e[6] = 0xFF; e[7] = 1;
	e += 8;						/* NMI on LINT1 */
	count++;

	*(uint16_t *)(void *)(t + 4) = (uint16_t)(e - t);
	*(uint16_t *)(void *)(t + 34) = count;
	t[7] = (uint8_t)-sum8(t, (size_t)(e - t));

	memset(fp, 0, 16);
	memcpy(fp, "_MP_", 4);
	*(uint32_t *)(void *)(fp + 4) = (uint32_t)MPTABLE_GPA;
	fp[8] = 1;					/* 16 bytes */
	fp[9] = 4;
	fp[10] = (uint8_t)-sum8(fp, 16);

	ioapic.id = ioapic_id;
	for (i = 0; i < IOAPIC_PINS; i++)
		ioapic.rte[i] = 1ULL << 16;		/* masked */
}

/* -------------------------------------------------------------------------- */
/* I/O ports. */

static void
io_callback(struct nvmm_io *io)
{
	const uint16_t port = io->port;
	const uint64_t t0 = now_ns();
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
	stall_check(t0, "I/O port", port);
}

/* Memory the guest touches that is not RAM: the virtio devices. */
static void
mem_callback(struct nvmm_mem *mem)
{
	const uint64_t idx = (mem->gpa - VIRTIO_BASE) / VIRTIO_STRIDE;
	struct cpu *c = (struct cpu *)(void *)((char *)mem->vcpu -
	    offsetof(struct cpu, vcpu));
	const uint64_t t0 = now_ns();
	uint32_t v = 0;

	if (apic_mode && mem->size == 4 &&
	    (mem->gpa & ~0xFFFULL) == LAPIC_BASE) {
		if (mem->write) {
			memcpy(&v, mem->data, 4);
			lapic_write(c, mem->gpa & 0xFFF, v);
		} else {
			v = lapic_read(c, mem->gpa & 0xFFF);
			memcpy(mem->data, &v, 4);
		}
		return;
	}
	if (apic_mode && mem->size == 4 &&
	    (mem->gpa & ~0xFFFULL) == IOAPIC_BASE) {
		if (mem->write) {
			memcpy(&v, mem->data, 4);
			ioapic_write(mem->gpa & 0xFFF, v);
		} else {
			v = ioapic_read(mem->gpa & 0xFFF);
			memcpy(mem->data, &v, 4);
		}
		return;
	}

	if (mem->gpa >= VIRTIO_BASE && idx < nvdevs && mem->size <= 4) {
		const uint64_t off = (mem->gpa - VIRTIO_BASE) % VIRTIO_STRIDE;

		if (mem->write) {
			memcpy(&v, mem->data, mem->size);
			virtio_write(&vdevs[idx], off, v);
		} else {
			v = virtio_read(&vdevs[idx], off, mem->size);
			memcpy(mem->data, &v, mem->size);
		}
		stall_check(t0, "virtio device at", (unsigned long)mem->gpa);
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
setup_cpu(struct cpu *c, uint64_t entry)
{
	struct nvmm_x64_state *st = c->vcpu.state;
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

	if (nvmm_vcpu_getstate(&mach, &c->vcpu, NVMM_X64_STATE_ALL) == -1)
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

	if (nvmm_vcpu_setstate(&mach, &c->vcpu, NVMM_X64_STATE_SEGS |
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
 * Something happened that CPU c should look at. If it is asleep, wake it;
 * if it is in the guest, interrupt the host CPU it is on, which brings it
 * back out. Called with 'big' held.
 */
static void
cpu_wake(struct cpu *c)
{
	if (c->in_guest)
		pthread_kill(c->thread, SIGUSR1);
	pthread_cond_signal(&c->cond);
}

/*
 * The clock thread fires the timers and passes on what is typed. It sleeps
 * until the next timer is due or a key is pressed, so a machine with nothing
 * to do costs the host next to nothing.
 *
 * It also brings every CPU that is in the guest back out, a hundred times a
 * second. An interrupt raised for a CPU just as it was going in can be
 * missed (the signal that should fetch it out arrives a moment too soon),
 * and this bounds how long it then waits.
 */
#define CLOCK_IDLE_NS	100000000ULL	/* with no timer set: 100 ms */
#define CLOCK_SWEEP_NS	10000000ULL	/* the safety sweep: 10 ms */

static int clock_pipe[2] = { -1, -1 };
static uint64_t clock_target;		/* when the clock thread will wake */

/* A timer was set for 'deadline'. Wake the clock thread if that is sooner
 * than it meant to. Called with 'big' held. */
static void
clock_rearm(uint64_t deadline)
{
	if (clock_pipe[1] != -1 && deadline < clock_target) {
		clock_target = deadline;
		(void)write(clock_pipe[1], "", 1);
	}
}

static void *
ticker(void *arg)
{
	uint64_t now, next, last_sweep = 0;
	struct timespec ts;
	unsigned int i;
	char drain[64];
	fd_set rfds;

	(void)arg;
	while (running) {
		pthread_mutex_lock(&big);
		if (!console_eof)
			console_poll();
		next = pit_poll();
		now = now_ns();
		next = (next != 0) ? now + next : now + CLOCK_IDLE_NS;
		for (i = 0; i < ncpus; i++) {
			struct cpu *c = &cpus[i];

			if (!apic_mode)
				break;
			lapic_timer_poll(c, now);
			if (c->apic.deadline_ns != 0 && c->apic.deadline_ns < next)
				next = c->apic.deadline_ns;
		}
		if (now - last_sweep >= CLOCK_SWEEP_NS) {
			last_sweep = now;
			for (i = 0; i < ncpus; i++) {
				if (cpus[i].in_guest)
					pthread_kill(cpus[i].thread, SIGUSR1);
			}
		}
		/* A CPU in the guest needs the sweep; an idle machine does not. */
		for (i = 0; i < ncpus; i++) {
			if (cpus[i].in_guest && next > now + CLOCK_SWEEP_NS)
				next = now + CLOCK_SWEEP_NS;
		}
		if (next > now + CLOCK_IDLE_NS)
			next = now + CLOCK_IDLE_NS;
		/* Behind already: the CPUs make it up; do not spin here. */
		if (next < now + 50000)
			next = now + 50000;
		clock_target = next;
		pthread_mutex_unlock(&big);

		FD_ZERO(&rfds);
		FD_SET(clock_pipe[0], &rfds);
		if (!console_eof)
			FD_SET(STDIN_FILENO, &rfds);
		ts.tv_sec = 0;
		ts.tv_nsec = (long)(next - now);
		if (pselect(clock_pipe[0] + 1, &rfds, NULL, NULL, &ts,
		    NULL) > 0 && FD_ISSET(clock_pipe[0], &rfds))
			(void)read(clock_pipe[0], drain, sizeof(drain));
		/* Slept far past the deadline: the host held this process. */
		stall_check(next, "the clock thread oversleeping", 0);
	}

	pthread_mutex_lock(&big);
	for (i = 0; i < ncpus; i++)
		cpu_wake(&cpus[i]);
	pthread_mutex_unlock(&big);
	return NULL;
}

static void
set_gpr(struct cpu *c, int reg, uint64_t val)
{
	if (nvmm_vcpu_getstate(&mach, &c->vcpu, NVMM_X64_STATE_GPRS) == -1)
		die("getstate: %s", strerror(errno));
	c->vcpu.state->gprs[reg] = val;
	if (nvmm_vcpu_setstate(&mach, &c->vcpu, NVMM_X64_STATE_GPRS) == -1)
		die("setstate: %s", strerror(errno));
}

static void
dump_and_die(struct cpu *c, const char *why)
{
	const struct nvmm_x64_state *st = c->vcpu.state;

	(void)nvmm_vcpu_getstate(&mach, &c->vcpu, NVMM_X64_STATE_ALL);
	die("cpu %u: %s: rip %#llx rsp %#llx cr0 %#llx cr2 %#llx cr3 %#llx "
	    "cr4 %#llx efer %#llx", c->id, why,
	    (unsigned long long)st->gprs[NVMM_X64_GPR_RIP],
	    (unsigned long long)st->gprs[NVMM_X64_GPR_RSP],
	    (unsigned long long)st->crs[NVMM_X64_CR_CR0],
	    (unsigned long long)st->crs[NVMM_X64_CR_CR2],
	    (unsigned long long)st->crs[NVMM_X64_CR_CR3],
	    (unsigned long long)st->crs[NVMM_X64_CR_CR4],
	    (unsigned long long)st->msrs[NVMM_X64_MSR_EFER]);
}

/*
 * Do the 8259s have an interrupt for this CPU? They only ever interrupt the
 * first one, and once it has a local APIC, only while that lets them
 * through: before it is enabled, or with LINT0 set to pass them on.
 */
static bool
pic_wants(const struct cpu *c)
{
	const struct lapic *a = &c->apic;

	if (c->id != 0 || !pic_pending())
		return false;
	if (!apic_mode || (a->svr & 0x100) == 0)
		return true;
	return (a->lvt[LVT_LINT0] & LVT_MASKED) == 0 &&
	    ((a->lvt[LVT_LINT0] >> 8) & 7) == 7;		/* ExtINT */
}

static bool
cpu_has_interrupt(const struct cpu *c)
{
	return c->nmi_pending || pic_wants(c) ||
	    (apic_mode && lapic_pending(c) >= 0);
}

/* Hand the guest its next interrupt, or ask to be told when it can take it. */
static void
deliver_interrupts(struct cpu *c)
{
	const struct nvmm_vcpu_exit *exit = c->vcpu.exit;
	int vec;

	if (c->nmi_pending && !exit->exitstate.evt_pending) {
		c->nmi_pending = false;
		c->vcpu.event->type = NVMM_VCPU_EVENT_EXCP;
		c->vcpu.event->vector = 2;
		c->vcpu.event->u.excp.error = 0;
		if (nvmm_vcpu_inject(&mach, &c->vcpu) == -1)
			die("inject: %s", strerror(errno));
		return;
	}
	if (!cpu_has_interrupt(c))
		return;

	if ((exit->exitstate.rflags & 0x200) && !exit->exitstate.int_shadow &&
	    !exit->exitstate.evt_pending) {
		if (apic_mode && (vec = lapic_pending(c)) >= 0) {
			/* Taken: from requested to in service. */
			c->apic.irr[vec / 32] &= ~(1u << (vec % 32));
			c->apic.isr[vec / 32] |= 1u << (vec % 32);
		} else if (pic_wants(c)) {
			vec = pic_ack();
		} else {
			return;
		}
		c->vcpu.event->type = NVMM_VCPU_EVENT_INTR;
		c->vcpu.event->vector = (uint8_t)vec;
		if (nvmm_vcpu_inject(&mach, &c->vcpu) == -1)
			die("inject: %s", strerror(errno));
		return;
	}

	if (!exit->exitstate.int_window_exiting) {
		if (nvmm_vcpu_getstate(&mach, &c->vcpu,
		    NVMM_X64_STATE_INTR) == -1)
			die("getstate: %s", strerror(errno));
		c->vcpu.state->intr.int_window_exiting = 1;
		if (nvmm_vcpu_setstate(&mach, &c->vcpu,
		    NVMM_X64_STATE_INTR) == -1)
			die("setstate: %s", strerror(errno));
	}
}

/* Sleep, with 'big' released, until woken. */
static void
cpu_sleep(struct cpu *c)
{
	struct timespec ts;
	struct timeval tv;

	/* Woken when there is something to do; the timeout is a backstop. */
	gettimeofday(&tv, NULL);
	ts.tv_sec = tv.tv_sec;
	ts.tv_nsec = (tv.tv_usec + 250000) * 1000;	/* 250 ms */
	if (ts.tv_nsec >= 1000000000) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000;
	}
	(void)pthread_cond_timedwait(&c->cond, &big, &ts);
}

/* The guest executed HLT: wait for something to interrupt it. */
static void
cpu_idle(struct cpu *c)
{
	unsigned int i, stopped = 0;

	if ((c->vcpu.exit->exitstate.rflags & 0x200) == 0 && !c->nmi_pending) {
		/*
		 * Halted with interrupts off: this CPU is finished. When
		 * they all are, so is the machine.
		 */
		c->stopped = true;
		for (i = 0; i < ncpus; i++)
			stopped += cpus[i].stopped || cpus[i].wait_sipi;
		if (stopped == ncpus) {
			fprintf(stderr, "\r\nnvmm-run: the guest halted\r\n");
			running = false;
		}
	}
	while (running && !cpu_has_interrupt(c) && !c->wait_sipi)
		cpu_sleep(c);
	c->stopped = false;
}

/* An AP is started: real mode, at the address the startup IPI names. */
static void
cpu_startup(struct cpu *c)
{
	struct nvmm_x64_state *st = c->vcpu.state;
	struct nvmm_x64_state_seg seg;

	if (nvmm_vcpu_getstate(&mach, &c->vcpu, NVMM_X64_STATE_ALL) == -1)
		die("getstate: %s", strerror(errno));

	memset(&seg, 0, sizeof(seg));
	seg.attrib.type = 0x3;
	seg.attrib.s = 1;
	seg.attrib.p = 1;
	seg.limit = 0xFFFF;
	st->segs[NVMM_X64_SEG_DS] = seg;
	st->segs[NVMM_X64_SEG_ES] = seg;
	st->segs[NVMM_X64_SEG_SS] = seg;
	st->segs[NVMM_X64_SEG_FS] = seg;
	st->segs[NVMM_X64_SEG_GS] = seg;
	seg.attrib.type = 0xB;
	seg.selector = (uint16_t)(c->sipi_vector << 8);
	seg.base = (uint64_t)c->sipi_vector << 12;
	st->segs[NVMM_X64_SEG_CS] = seg;
	st->segs[NVMM_X64_SEG_GDT].base = 0;
	st->segs[NVMM_X64_SEG_GDT].limit = 0xFFFF;
	st->segs[NVMM_X64_SEG_IDT].base = 0;
	st->segs[NVMM_X64_SEG_IDT].limit = 0xFFFF;

	memset(st->gprs, 0, sizeof(st->gprs));
	st->gprs[NVMM_X64_GPR_RFLAGS] = 0x2;
	st->gprs[NVMM_X64_GPR_RDX] = 0x00800F00;
	st->crs[NVMM_X64_CR_CR0] = 0x60000010;
	st->crs[NVMM_X64_CR_CR2] = 0;
	st->crs[NVMM_X64_CR_CR3] = 0;
	st->crs[NVMM_X64_CR_CR4] = 0;
	st->msrs[NVMM_X64_MSR_EFER] = 0;
	memset(&st->intr, 0, sizeof(st->intr));

	if (nvmm_vcpu_setstate(&mach, &c->vcpu, NVMM_X64_STATE_SEGS |
	    NVMM_X64_STATE_GPRS | NVMM_X64_STATE_CRS | NVMM_X64_STATE_MSRS |
	    NVMM_X64_STATE_INTR) == -1)
		die("setstate: %s", strerror(errno));
	memset(&c->vcpu.exit->exitstate, 0, sizeof(c->vcpu.exit->exitstate));
	lapic_reset(c);
}

static void
handle_rdmsr(struct cpu *c)
{
	const struct nvmm_vcpu_exit *exit = c->vcpu.exit;
	uint64_t val = 0;

	if (exit->u.rdmsr.msr == 0x1B && apic_mode)	/* APIC base */
		val = c->apic.base_msr;
	else if (verbose)
		fprintf(stderr, "[cpu %u rdmsr %#x]\r\n", c->id,
		    exit->u.rdmsr.msr);

	(void)nvmm_vcpu_getstate(&mach, &c->vcpu, NVMM_X64_STATE_GPRS);
	c->vcpu.state->gprs[NVMM_X64_GPR_RAX] = (uint32_t)val;
	c->vcpu.state->gprs[NVMM_X64_GPR_RDX] = val >> 32;
	c->vcpu.state->gprs[NVMM_X64_GPR_RIP] = exit->u.rdmsr.npc;
	(void)nvmm_vcpu_setstate(&mach, &c->vcpu, NVMM_X64_STATE_GPRS);
}

static void *
cpu_thread(void *arg)
{
	struct cpu *c = arg;
	struct nvmm_vcpu_exit *exit = c->vcpu.exit;
	uint64_t now;

	c->thread = pthread_self();
	pthread_mutex_lock(&big);
	/* Before the first run there is no exit state: interrupts are off. */
	memset(&exit->exitstate, 0, sizeof(exit->exitstate));

	while (running) {
		if (c->wait_sipi) {
			if (!c->sipi_pending) {
				cpu_sleep(c);
				continue;
			}
			c->sipi_pending = false;
			c->wait_sipi = false;
			cpu_startup(c);
		}
		if (c->id == 0)
			(void)pit_poll();
		now = now_ns();
		if (apic_mode)
			lapic_timer_poll(c, now);
		deliver_interrupts(c);

		/* The clock's safety sweep must be running while this is. */
		clock_rearm(now + CLOCK_SWEEP_NS);
		c->in_guest = true;
		pthread_mutex_unlock(&big);
		now = now_ns();
		if (nvmm_vcpu_run(&mach, &c->vcpu) == -1 && errno != EINTR)
			die("cpu %u: vcpu run: %s", c->id, strerror(errno));
		stall_check(now, "one call into the guest, cpu", c->id);
		now = now_ns();
		pthread_mutex_lock(&big);
		stall_check(now, "waiting for the lock, cpu", c->id);
		c->in_guest = false;
		c->nruns++;

		switch (exit->reason) {
		case NVMM_VCPU_EXIT_NONE:
		case NVMM_VCPU_EXIT_INT_READY:
			break;
		case NVMM_VCPU_EXIT_IO:
			if (nvmm_assist_io(&mach, &c->vcpu) == -1)
				dump_and_die(c, "I/O emulation failed");
			break;
		case NVMM_VCPU_EXIT_MEMORY:
			if (nvmm_assist_mem(&mach, &c->vcpu) == -1)
				dump_and_die(c, "memory access emulation failed");
			break;
		case NVMM_VCPU_EXIT_HALTED:
			cpu_idle(c);
			break;
		case NVMM_VCPU_EXIT_RDMSR:
			handle_rdmsr(c);
			break;
		case NVMM_VCPU_EXIT_WRMSR:
			if (exit->u.wrmsr.msr == 0x1B && apic_mode)
				c->apic.base_msr = exit->u.wrmsr.val;
			else if (verbose)
				fprintf(stderr, "[cpu %u wrmsr %#x = %#llx]\r\n",
				    c->id, exit->u.wrmsr.msr,
				    (unsigned long long)exit->u.wrmsr.val);
			set_gpr(c, NVMM_X64_GPR_RIP, exit->u.wrmsr.npc);
			break;
		case NVMM_VCPU_EXIT_MONITOR:
		case NVMM_VCPU_EXIT_MWAIT:
			set_gpr(c, NVMM_X64_GPR_RIP, exit->u.insn.npc);
			break;
		case NVMM_VCPU_EXIT_SHUTDOWN:
			dump_and_die(c, "the guest triple-faulted");
			break;
		default:
			fprintf(stderr, "\r\nnvmm-run: cpu %u: exit reason "
			    "%#llx (hw %#llx)\r\n", c->id,
			    (unsigned long long)exit->reason,
			    (unsigned long long)exit->u.inv.hwcode);
			dump_and_die(c, "unexpected exit");
		}
	}

	pthread_mutex_unlock(&big);
	return NULL;
}

static void
usage(void)
{
	fprintf(stderr, "usage: nvmm-run -k vmlinuz [-i initramfs] "
	    "[-d disk.img] [-n vmnet|socket] [-c cpus]\n"
	    "                [-m megabytes] [-a \"extra cmdline\"] [-v]\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	const char *kpath = NULL, *ipath = NULL, *dpath = NULL, *extra = "";
	const char *net = NULL;
	struct nvmm_assist_callbacks cbs = { io_callback, mem_callback };
	struct nvmm_vcpu_conf_cpuid cpuid;
	struct sigaction sa;
	char cmdline[CMDLINE_MAX];
	uint64_t tsc_hz = 0, entry;
	size_t len = sizeof(tsc_hz);
	pthread_t tick, threads[MAX_CPUS];
	unsigned int i;
	long mb = 512, n = 1;
	int ch;

	while ((ch = getopt(argc, argv, "k:i:d:n:c:m:a:v")) != -1) {
		switch (ch) {
		case 'k': kpath = optarg; break;
		case 'i': ipath = optarg; break;
		case 'd': dpath = optarg; break;
		case 'n': net = optarg; break;
		case 'c': n = atol(optarg); break;
		case 'm': mb = atol(optarg); break;
		case 'a': extra = optarg; break;
		case 'v': verbose = true; break;
		default: usage();
		}
	}
	if (kpath == NULL || mb < 64 || mb > MEM_MAX_MB || n < 1 ||
	    n > MAX_CPUS)
		usage();
	ram_size = (uint64_t)mb << 20;
	ncpus = (unsigned int)n;
	apic_mode = ncpus > 1;

	if (nvmm_init() == -1)
		die("cannot open /dev/nvmm: %s", strerror(errno));
	if (nvmm_machine_create(&mach) == -1)
		die("machine create: %s", strerror(errno));

	for (i = 0; i < ncpus; i++) {
		struct cpu *c = &cpus[i];

		c->id = i;
		c->wait_sipi = (i != 0);
		pthread_cond_init(&c->cond, NULL);
		lapic_reset(c);
		if (nvmm_vcpu_create(&mach, i, &c->vcpu) == -1)
			die("vcpu %u create: %s", i, strerror(errno));
		if (nvmm_vcpu_configure(&mach, &c->vcpu,
		    NVMM_VCPU_CONF_CALLBACKS, &cbs) == -1)
			die("vcpu configure: %s", strerror(errno));

		/*
		 * Hide what is not emulated: x2APIC and the TSC-deadline
		 * timer always, and with one CPU the local APIC itself, so
		 * that the kernel does not go looking for it.
		 */
		memset(&cpuid, 0, sizeof(cpuid));
		cpuid.mask = 1;
		cpuid.leaf = 0x00000001;
		cpuid.u.mask.del.ecx = (1u << 21) | (1u << 24);
		if (!apic_mode)
			cpuid.u.mask.del.edx = 1u << 9;
		if (nvmm_vcpu_configure(&mach, &c->vcpu, NVMM_VCPU_CONF_CPUID,
		    &cpuid) == -1)
			die("cpuid configure: %s", strerror(errno));
	}

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
	 * With no ACPI the kernel has little to measure the CPU's clock
	 * against. Tell it instead, and tell it to trust the answer.
	 */
	(void)sysctlbyname("machdep.tsc.frequency", &tsc_hz, &len, NULL, 0);
	snprintf(cmdline, sizeof(cmdline),
	    "console=ttyS0 earlyprintk=serial,ttyS0 %sacpi=off pci=off "
	    "reboot=k panic=-1 tsc=reliable clocksource=tsc "
	    "i8042.noaux i8042.nokbd", apic_mode ? "" : "nolapic noapic ");
	if (tsc_hz != 0) {
		snprintf(cmdline + strlen(cmdline),
		    sizeof(cmdline) - strlen(cmdline), " tsc_early_khz=%llu",
		    (unsigned long long)(tsc_hz / 1000));
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = kick_handler;
	sigaction(SIGUSR1, &sa, NULL);

	if (dpath != NULL)
		blk_add(dpath, cmdline, sizeof(cmdline));
	if (net != NULL && strcmp(net, "vmnet") == 0)
		net_add_vmnet(cmdline, sizeof(cmdline));
	else if (net != NULL && strcmp(net, "unplugged") == 0)
		net_add_unplugged(cmdline, sizeof(cmdline));
	else if (net != NULL)
		net_add_socket(net, cmdline, sizeof(cmdline));
	snprintf(cmdline + strlen(cmdline), sizeof(cmdline) - strlen(cmdline),
	    " %s", extra);

	entry = load_linux(kpath, ipath, cmdline);
	if (apic_mode)
		mptable_build();
	setup_cpu(&cpus[0], entry);

	console_init();
	if (pipe(clock_pipe) == -1)
		die("pipe: %s", strerror(errno));
	(void)fcntl(clock_pipe[0], F_SETFL, O_NONBLOCK);
	(void)fcntl(clock_pipe[1], F_SETFL, O_NONBLOCK);
	for (i = 0; i < ncpus; i++) {
		if (pthread_create(&threads[i], NULL, cpu_thread,
		    &cpus[i]) != 0)
			die("cannot start a CPU thread");
	}
	if (pthread_create(&tick, NULL, ticker, NULL) != 0)
		die("cannot start the timer thread");

	for (i = 0; i < ncpus; i++)
		pthread_join(threads[i], NULL);
	running = false;
	(void)write(clock_pipe[1], "", 1);
	pthread_join(tick, NULL);

	if (verbose) {
		for (i = 0; i < ncpus; i++)
			fprintf(stderr, "\r\n[cpu %u: %lu returns from the "
			    "kernel]", i, cpus[i].nruns);
	}
	for (i = 0; i < ncpus; i++)
		(void)nvmm_vcpu_destroy(&mach, &cpus[i].vcpu);
	(void)nvmm_machine_destroy(&mach);
	fprintf(stderr, "\r\n");
	return exit_code;
}
