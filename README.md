# nvmm-darwin

A port of [NVMM](https://www.dragonflybsd.org/docs/docs/howtos/nvmm/), the
NetBSD/DragonFly BSD hypervisor, to macOS on AMD processors.

macOS only offers hardware virtualization through Apple's Hypervisor
framework, which does not work on AMD CPUs. On an AMD Hackintosh that rules out
Docker Desktop, OrbStack, Colima and current VirtualBox. NVMM implements AMD-V
(SVM) itself, and QEMU already has an `nvmm` accelerator, so a macOS driver for
it gives QEMU, and anything built on QEMU, hardware-speed virtual machines.

## Status

**It runs Linux and Docker on one machine, with one virtual CPU.** On
2026-10-04, on a Ryzen 7 2700 running macOS 10.15.5, the kext passed
`nvmm-guest-test` (5,598 checks), and QEMU 7.2 with `-accel nvmm` booted
Alpine Linux 3.24, installed Docker in it, and ran containers
(`test/darwin/docker-test.exp`). That is one run, on one machine and one
macOS version, with a single vCPU and 2 GB of guest RAM. It also panicked
that machine once the same evening (since fixed; see below). Read this table
before trusting any of it.

| Piece | State |
|---|---|
| SVM engine (from DragonFly BSD) | Runs guests under emulated AMD-V; tests pass |
| Nested page table builder (`port/npt.c`) | Unit tested, and exercised by the engine tests |
| Guest-memory layer (`port/nvmm_port_vm.c`) | Exercised by the engine tests, with leak accounting |
| World-switch assembly (host VMCB variant) | Exercised by the engine tests; host state checked after every guest run |
| macOS glue (`darwin/`) | Runs on real hardware **without the engine**: the self-test kext loads on macOS 15.7.9 (MacBookPro11,1, Intel) and passes. Every imported symbol is confirmed exported by every macOS from 10.13 to 15, and by 26 |
| Engine and glue together, on an AMD CPU | Loads on macOS 10.15.5 (Ryzen 7 2700, 16 threads) and passes `nvmm-guest-test`: I/O, HLT, nested page faults, CPUID, FPU isolation, multi-second runs interrupted and resumed by the host hundreds of times, two guests at once, 200 machines created and destroyed. Not run on any other macOS version or CPU |
| `libnvmm` for macOS (`lib/`) | Works against the kernel core on real Sequoia, through real ioctls, with the stand-in engine |
| QEMU with `-accel nvmm` on macOS | QEMU 7.2.22 on macOS 10.15.5 boots Linux and runs Docker through the driver, one vCPU. QEMU 11 builds and starts a machine on macOS 15 against the stand-in engine, but has not run a guest. More than one vCPU per machine has never been tried |

**macOS versions.** One binary is meant to serve macOS 10.13 (the first with
AMD Ryzen support in the Hackintosh world) through 15. What that rests on:
the kext imports only exported symbols, and `tools/check-kpi.sh` confirms
each of them against the kernel sources of every release in that range. It
has been *loaded* on 15.7.9 only.

What the emulator tests cannot show, because the emulator hides it (the first
two have since passed on the Ryzen, in stage 4 of `nvmm-guest-test`):

- **TLB flushing.** QEMU's software AMD-V flushes on every world switch, so a
  flush the engine forgets would go unnoticed. The mutation checker lists this
  as a known blind spot.
- **Next-RIP save.** QEMU does not emulate it, so test builds define
  `NVMM_TEST_NO_NRIPS`, which fills the value in for the fixed-length
  instructions the tests use. Real CPUs take the normal path, which the tests
  therefore do not cover.
- **More than one CPU.** The test kernel runs on one.

What the glue self-test cannot show: anything about SVM. It runs on Intel
Macs precisely because it leaves the engine out.

## Layout

```
upstream/   pristine DragonFly BSD sources at a pinned commit (see UPSTREAM)
src/        the working copy of those sources, with NVMM_PORT hooks
port/       NVMM's os_* interface for a host with no BSD VM system
darwin/     the macOS kernel extension: platform hooks, /dev/nvmm, load/unload
test/unit       userspace test for the page table builder
test/baremetal  a freestanding kernel that boots in QEMU and drives the engine
test/mutation   deliberate bugs, to check that the tests can fail
test/darwin     programs that drive a loaded kext: the glue self-test, libnvmm
                against the stand-in engine, and the first real guests
lib/        libnvmm
tools/      check-kpi.sh, build-qemu.sh, bootstrap-deps.sh
```

`diff -ru upstream/sys/dev/virtual/nvmm src` shows exactly what was changed in
the imported code. Only the AMD half was imported; there is no Intel support.

## Building and testing

Needs the Xcode command line tools and, for the emulator tests, Docker.

```
make unit         # page table builder, with sanitizers
make test-bare    # link the test kernel and boot it under emulated AMD-V
make kext         # build/NVMM.kext, x86_64
make check        # all three
make release      # build/release/NVMM.kext, stripped (about 50 KB)
make guest-test   # nvmm-guest-test: the first real guests, for an AMD machine
make libnvmm      # the userland library and headers
make selftest     # the self-test kext (stand-in engine) and its test programs
./tools/build-qemu.sh   # QEMU with the nvmm accelerator; run on an x86_64 Mac
./tools/check-kpi.sh          # every symbol the kext imports is exported
./test/mutation/mutate.py     # each deliberate bug is caught (slow)
```

All of it runs on an Apple Silicon Mac. `make test-bare` builds a small Docker
image holding `lld` and `qemu-system-x86_64`; remove it with
`docker rmi nvmm-darwin-test`.

Pushing a tag that starts with `v` runs `.github/workflows/release.yml`,
which builds the stripped kext, repeats the symbol check for every macOS
release, and attaches the result to a GitHub pre-release.

## How it differs from NVMM on BSD

On BSD, a machine owns a vmspace whose pmap doubles as the guest's nested page
table, and guest pages are faulted in on demand. A macOS kernel extension has
no supported way to do that, and earlier hypervisor ports to macOS that reached
into private kernel structures stopped working when those structures changed.
So this port uses only exported interfaces, and pays for it in a few places:

- **Guest memory is eager and wired.** Guest RAM is an IOKit buffer; every page
  is entered into a hand-built nested page table when it is mapped. A nested
  page fault therefore always means "not RAM" and goes to the emulator. Guest
  RAM stays wired for the life of the machine.
- **Host state is saved in a VMCB.** The BSD code restores TR after a guest run
  by clearing the busy bit in the host GDT. Here the host's VMLOAD/VMSAVE state
  is parked in a per-CPU VMCB instead, which never touches the GDT.
- **Preemption is held off with a spin lock.** macOS does not export its
  preemption-disable primitive, but holding a spin lock has the same effect, so
  there is one per CPU.
- **The vCPU loop is bounded.** macOS offers no way for an extension to ask
  whether the scheduler or a signal is waiting, so the loop cannot stay in the
  kernel until one is. It handles up to 32 exits by itself, and returns to
  userland at once on a host interrupt or when that budget is spent.
- **`/dev/nvmm` is a cloning device**, so that each open gets its own minor
  and machines can be tied to the open that created them.
- **2M pages when the memory allows.** A guest buffer of 2M or more is built
  from 2M-aligned, host-contiguous runs, as many as the system gives within
  two seconds, with ordinary pages for the rest. Each run that lines up with
  the guest address becomes one 2M entry in the nested page table, and is
  split back into 4K pages if part of it is later unmapped.

## The glue self-test

`make selftest` builds `NVMMSelfTest.kext`, which is the same extension with
the CPU check and the engine left out, and `nvmm-selftest`, which drives it
through `/dev/nvmm-selftest`. It loads on any x86_64 Mac and checks wired
memory, host-physically contiguous allocations, nested page tables built from
real physical addresses, locks, preemption control, calls to every CPU, FPU
and debug-register parking, mappings into a process (anywhere and at a fixed
address), cleanup after a process that exits without unmapping, and that the
device node survives being looked up hundreds of times.

On a MacBookPro11,1 running macOS 15.7.9 it passes 68 in-kernel checks and
all process-side checks, repeatedly, with no growth in IOKit object counts,
and (in an earlier version) again after a real sleep and wake.
`test/darwin/final-check.sh` runs all of it, the libnvmm test and QEMU in
one go, then unloads and reloads the kext.

The 2M memory path on that machine, two minutes after boot with 8 GB of RAM:
buffers up to 16 MiB got every 2M run they asked for, in 1 to 60 ms. A 1 GiB
buffer got 11 of 512 in 0.4 s before the system ran out of contiguous memory,
and the rest fell back to ordinary pages as designed. So small guests get 2M
pages; whether a large one does depends on how fragmented memory is, and has
not been measured on a machine with more RAM.

Running it on a real machine found two bugs that emulation could not:

- **The device node.** devfs calls a cloning device's clone function on every
  lookup, not only on open. Reserving a slot there exhausted the device after
  sixteen `lstat()` calls, and returning -1 then left the node permanently
  "being created", which froze `sudo` and `sshd`.
- **Contiguity.** With an I/O mapper present, `kIOMemoryPhysicallyContiguous`
  is contiguous for a device, not for the CPU. Control blocks now ask for
  `kIOMemoryHostPhysicallyContiguous`.

On macOS 11 and later a kext has to be approved in System Settings and the
machine restarted before it will load, and again each time the binary changes.

## libnvmm and QEMU

`lib/` is libnvmm from DragonFly BSD. One thing differs on macOS: the kernel
owns the process's mapping of the comm page, so `NVMM_IOC_VCPU_DESTROY`
removes it and the library does not `munmap()` it.

QEMU has carried an `nvmm` accelerator since 6.0, written against libnvmm,
but its build only looks for it on NetBSD. `tools/build-qemu.sh` widens that
one check and builds QEMU against this repository's library. It needs only
the Xcode command line tools: `tools/bootstrap-deps.sh` fetches meson, ninja
and pkg-config as Python wheels and builds glib from source, because Homebrew
no longer installs on x86_64 Macs and MacPorts compiles about eighty packages
to provide the same four things.

To test all of this without AMD-V, the self-test kext puts a stand-in engine
(`darwin/nvmm_fake_engine.c`) behind `/dev/nvmm`. It runs no guest code: it
obeys a command in RAX, and treats a vCPU fresh out of reset as a guest that
halts. Against it, on a MacBookPro11,1 running macOS 15.7.9:

- `nvmm-fake-test` (libnvmm: machines, vCPUs, state through the comm page,
  guest RAM over an existing mapping, the guest-physical map, error paths, a
  child that exits without tidying up) passes 314 checks, repeatedly, with no
  growth in IOKit object counts.
- `qemu-system-x86_64 -accel nvmm -m 1G` starts, reports "NetBSD Virtual
  Machine Monitor accelerator is operational" and `VM status: running`, shows
  the vCPU in its reset state as read back through the driver, and quits
  cleanly; the driver passes its tests afterwards.

## First load on real hardware

This is the procedure that was followed for the first load, on macOS 10.15.5
with SIP disabled; it went through without a panic.

Requirements: an AMD CPU with SVM enabled in the firmware, a system that
accepts unsigned kernel extensions (`csr-active-config` with the kext-signing
bit clear), and something you can afford to panic. A spare macOS install
booted from another disk is ideal; so is macOS running as a guest under
Linux/KVM with AMD-V passed through.

1. Build on any Mac: `make kext`, then `./tools/check-kpi.sh <xnu tag of the
   target macOS>`.
2. Copy `build/NVMM.kext` to the target and `sudo chown -R root:wheel` it.
3. Load it at run time, not from the bootloader, so that a panic costs one
   reboot instead of a boot loop: `sudo kextutil -v NVMM.kext` on macOS 10.15,
   `sudo kmutil load -p NVMM.kext` on 11 and later.
4. Look for `nvmm: attached, using backend x86-svm` in `dmesg`, and for
   `/dev/nvmm`.
5. Unload with `sudo kextunload NVMM.kext`.

Then run `nvmm-guest-test` (`make guest-test`), over ssh so that the last
line printed survives a panic. It is the checks of
`test/baremetal/test_main.c` through libnvmm, in stages:

```
sudo ./nvmm-guest-test 1   # open the device. No guest runs
sudo ./nvmm-guest-test 2   # + machines and vCPUs. No guest runs
sudo ./nvmm-guest-test 3   # + the first VMRUN: a guest that halts
sudo ./nvmm-guest-test 4   # + I/O, memory faults, CPUID, FPU, 2M pages
sudo ./nvmm-guest-test     # + long runs, two guests at once, 200 machines
```

Stage 4 is where the two things emulation hides get their first test: TLB
flushing after an unmap, and next-RIP save.

## Known gaps

- More than one vCPU in a machine is untested, as are guests larger than
  2 GB, long uptimes, and sleep and wake with a guest running.
- A guest too large for the host to wire used to panic the host: the imported
  code assumed that allocation could not fail. It now returns ENOMEM, and
  the emulator tests cover it. Why a 4 GB guest could not be allocated on a
  16 GB host after several earlier runs is not explained; guest memory was
  seen to be released after a clean run.
- Whether guest memory really ends up in 2M pages on the Ryzen is not known:
  the guest test passes either way and does not report it.
- Sleep and wake: the notifications arrive and the glue survives a cycle on
  real hardware, and the engine's suspend path is tested under emulation, but
  the two have not been tested together, and never with a guest running.
- Hosts that enable AVX-512 lazily per thread (not AMD Zen or Zen+) are not
  handled: the engine assumes one host XCR0.
- `/dev/nvmm` is root-only.
- A process that exits without closing its mappings relies on the device close
  path to unwire guest RAM; that path has not run on macOS.

## Licence

Two-clause BSD; see `LICENSE`. The imported files carry their original
headers. QEMU is not part of this repository and is not distributed with it:
`tools/build-qemu.sh` downloads it and changes one line of its build.
