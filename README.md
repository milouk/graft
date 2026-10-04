# nvmm-darwin

A port of [NVMM](https://www.dragonflybsd.org/docs/docs/howtos/nvmm/), the
NetBSD/DragonFly BSD hypervisor, to macOS on AMD processors.

macOS only offers hardware virtualization through Apple's Hypervisor
framework, which does not work on AMD CPUs. On an AMD Hackintosh that rules out
Docker Desktop, OrbStack, Colima and current VirtualBox. NVMM implements AMD-V
(SVM) itself, and QEMU already has an `nvmm` accelerator, so a macOS driver for
it gives QEMU, and anything built on QEMU, hardware-speed virtual machines.

## Status

**No guest has run on real hardware yet.** The engine has only run under
emulation, and the macOS glue has only run without the engine. Read this table
before trusting any of it.

| Piece | State |
|---|---|
| SVM engine (from DragonFly BSD) | Runs guests under emulated AMD-V; tests pass |
| Nested page table builder (`port/npt.c`) | Unit tested, and exercised by the engine tests |
| Guest-memory layer (`port/nvmm_port_vm.c`) | Exercised by the engine tests, with leak accounting |
| World-switch assembly (host VMCB variant) | Exercised by the engine tests; host state checked after every guest run |
| macOS glue (`darwin/`) | Runs on real hardware **without the engine**: the self-test kext loads on macOS 15.7.9 (MacBookPro11,1, Intel) and passes. Every imported symbol is confirmed exported on macOS 15 and 10.15 |
| Engine and glue together, on an AMD CPU | **Never run** |
| `libnvmm` for macOS (`lib/`) | Works against the kernel core on real Sequoia, through real ioctls, with the stand-in engine |
| QEMU with `-accel nvmm` on macOS | Builds (`tools/build-qemu.sh`) and starts a machine through the driver on real Sequoia, with the stand-in engine. **Has never run guest code** |

What the tests cannot show, because the emulator hides it:

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
tools/      check-kpi.sh
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
make libnvmm      # the userland library and headers
make selftest     # the self-test kext (stand-in engine) and its test programs
./tools/build-qemu.sh   # QEMU with the nvmm accelerator; run on an x86_64 Mac
./tools/check-kpi.sh          # every symbol the kext imports is exported
./test/mutation/mutate.py     # each deliberate bug is caught (slow)
```

All of it runs on an Apple Silicon Mac. `make test-bare` builds a small Docker
image holding `lld` and `qemu-system-x86_64`; remove it with
`docker rmi nvmm-darwin-test`.

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
- **The vCPU loop returns to userland after every exit.** macOS offers no way
  for an extension to ask whether the scheduler or a signal is waiting. This
  costs a system call for exits the engine could have handled alone.
- **`/dev/nvmm` is a cloning device**, so that each open gets its own minor
  and machines can be tied to the open that created them.
- **4K pages only** in the nested page table, for now.

## The glue self-test

`make selftest` builds `NVMMSelfTest.kext`, which is the same extension with
the CPU check and the engine left out, and `nvmm-selftest`, which drives it
through `/dev/nvmm-selftest`. It loads on any x86_64 Mac and checks wired
memory, host-physically contiguous allocations, nested page tables built from
real physical addresses, locks, preemption control, calls to every CPU, FPU
and debug-register parking, mappings into a process (anywhere and at a fixed
address), cleanup after a process that exits without unmapping, and that the
device node survives being looked up hundreds of times.

On a MacBookPro11,1 running macOS 15.7.9 it passes 47 in-kernel checks and
all process-side checks, repeatedly, with no growth in IOKit object counts,
and again after a real sleep and wake (both notifications are delivered).

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

The self-test kext has been through this on an Intel Mac. The real kext, with
the engine, has not been loaded anywhere. This is the plan for it.

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

The first thing to exercise after that is a userland port of the checks in
`test/baremetal/test_main.c`: the same guest programs, through real ioctls.

## Known gaps

- Nothing has run guest code outside the emulator tests. The first real
  guest needs an AMD machine.
- Sleep and wake: the notifications arrive and the glue survives a cycle on
  real hardware, and the engine's suspend path is tested under emulation, but
  the two have not been tested together, and never with a guest running.
- Hosts that enable AVX-512 lazily per thread (not AMD Zen or Zen+) are not
  handled: the engine assumes one host XCR0.
- `/dev/nvmm` is root-only.
- A process that exits without closing its mappings relies on the device close
  path to unwire guest RAM; that path has not run on macOS.

## Licence

The imported files carry their original two-clause BSD licence headers. New
files in this repository are offered under the same terms.
