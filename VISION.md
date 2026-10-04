# Where this could go

Today this repository is three things: the NVMM hypervisor running as a macOS
driver on AMD processors, a layer that let it run there, and a small program
that boots Linux on top of it and serves Docker to the Mac. It was built to
solve one person's problem on one machine. This page is about what larger
problem the same pieces could solve, and what would have to be true for each
step to be worth taking.

## The problem worth solving

Running a virtual machine today means depending on one of a handful of very
large codebases. The hypervisor is part of the operating system vendor's
kernel (KVM, Hyper-V, Apple's Hypervisor framework) and the program that
drives it is QEMU, or something of similar size. If your operating system's
vendor does not provide the first, or does not provide it for your hardware,
you have no virtualization at all, and nothing you can install will change
that. macOS on AMD is one such gap. It is not the only one: most small and
research operating systems are in the same position, and on the BSDs that do
have NVMM, the only program that drives it is QEMU.

The pieces here are small enough to read. NVMM's kernel half is about 7,000
lines and its library 4,600; the layer that carries it to a new operating
system is about 2,000, and the program that boots Linux and runs Docker
about 2,800. That is the asset: **a
complete virtualization stack small enough for one person to understand,
port and audit**, from the instruction that enters the guest to the socket
`docker` talks to.

## Three directions, nearest first

### 1. A complete container host for Macs that Apple's hypervisor skips

What exists is a demonstration. To be something people rely on it needs
shared folders (`docker run -v`), `docker compose` and local Kubernetes
working unmodified, a memory footprint that follows what the guest uses
instead of what it was given, a proper installer, and evidence from weeks of
use on current macOS rather than an evening on an old one.

The audience is AMD Hackintoshes and macOS guests on AMD servers. It is
real, and it has a ceiling: Apple has stopped developing macOS for x86.
This direction is worth finishing because it is the project's only source of
real users and real bugs, not because it is where the growth is.

### 2. A small virtual machine monitor for every system that has NVMM

`nvmm-run` depends on libnvmm and POSIX, and on nothing that is specific to
macOS apart from one optional network backend. NetBSD and DragonFly BSD have
NVMM in their kernels and only QEMU to use it with. A monitor of a few
thousand lines that boots a Linux kernel in well under a second, with virtio
devices and nothing else, is what Firecracker is to KVM, and those systems
do not have one.

This is the cheapest direction and probably the one with the most lasting
value: the work is mostly building and testing on those systems, and it
outlives x86 macOS. What it needs first is the things a monitor is judged
on: boot time measured and driven down, idle cost near zero, snapshots, and
a device model that does not go through an instruction emulator for every
interrupt.

### 3. A hypervisor you can bring to an operating system that has none

The portable layer asks an operating system for very little: wired pages
whose physical addresses it may know, a way to map a buffer into a process,
a way to run a function on every processor, and locks. macOS is one
implementation of that list, and the test kernel in this repository, which
is not an operating system at all, is a second. Any x86 system that can
supply those few things can have hardware virtualization: hobby and research
kernels, unikernel hosts, embedded systems.

The other half of this is the way it was tested. The engine runs under an
emulated AMD processor inside an ordinary test, with deliberate bugs
injected to prove the tests can fail. NVMM upstream has nothing like it.
Offering that back to NetBSD and DragonFly would make the engine everyone
shares more trustworthy, and it is the natural way to become a contributor
to the project this one is built on rather than a fork beside it.

To be real, this direction needs a second operating system ported by
someone else, the Intel half of NVMM brought through the same layer, and the
layer's interface written down as a contract instead of a header.

## What would not be worth doing

- **Pretending to be Apple's Hypervisor framework**, so that programs written
  for it run unchanged on AMD. It is the idea with the most obvious appeal
  and the worst cost: on x86 that interface exposes Intel's own control
  structures, so it means translating one vendor's virtualization into the
  other's, bug for bug, forever.
- **Growing the monitor into a general one.** The moment it has a BIOS, PCI
  and a graphics card it is a worse QEMU. Its value is that it does one job.
- **Apple Silicon.** A third-party driver cannot reach the processor mode a
  hypervisor needs there, and Apple's own interface works.

## What has to be true first

None of the above is worth starting until the basics are honest: the driver
has run for weeks, not hours; it has run on a current macOS; someone other
than its author has loaded it. The order is the one above, and the test for
moving from one step to the next is whether somebody else is using the
previous one.
