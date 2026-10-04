# nvmm-darwin

A port of [NVMM](https://www.dragonflybsd.org/docs/docs/howtos/nvmm/), the
NetBSD/DragonFly BSD hypervisor, to macOS on AMD processors.

macOS only offers hardware virtualization through Apple's Hypervisor
framework, which does not work on AMD CPUs. On an AMD Hackintosh that rules out
Docker Desktop, OrbStack, Colima and current VirtualBox. NVMM implements AMD-V
(SVM) itself, and QEMU already has an `nvmm` accelerator, so a macOS driver for
it gives QEMU, and anything built on QEMU, hardware-speed virtual machines.

## Status

**The kernel extension has never been loaded on a real machine.** Read this
table before trusting any of it.

| Piece | State |
|---|---|
| SVM engine (from DragonFly BSD) | Runs guests under emulated AMD-V; tests pass |
| Nested page table builder (`port/npt.c`) | Unit tested, and exercised by the engine tests |
| Guest-memory layer (`port/nvmm_port_vm.c`) | Exercised by the engine tests, with leak accounting |
| World-switch assembly (host VMCB variant) | Exercised by the engine tests; host state checked after every guest run |
| macOS glue (`darwin/`) | **Compiles and links only.** Every imported symbol is confirmed exported on macOS 15. Never run |
| `libnvmm` for macOS | Not started |
| QEMU with `-accel nvmm` on macOS | Not started |

What the tests cannot show, because the emulator hides it:

- **TLB flushing.** QEMU's software AMD-V flushes on every world switch, so a
  flush the engine forgets would go unnoticed. The mutation checker lists this
  as a known blind spot.
- **Next-RIP save.** QEMU does not emulate it, so test builds define
  `NVMM_TEST_NO_NRIPS`, which fills the value in for the fixed-length
  instructions the tests use. Real CPUs take the normal path, which the tests
  therefore do not cover.
- **More than one CPU.** The test kernel runs on one.
- **Anything in `darwin/`.**

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

## First load on real hardware

None of this has been done. It is the plan.

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

- `libnvmm` and the QEMU build are not done, so nothing can use the driver yet.
- Sleep and wake are handled in code and tested under emulation, but untested
  on a real host.
- Hosts that enable AVX-512 lazily per thread (not AMD Zen or Zen+) are not
  handled: the engine assumes one host XCR0.
- `/dev/nvmm` is root-only.
- A process that exits without closing its mappings relies on the device close
  path to unwire guest RAM; that path has not run on macOS.

## Licence

The imported files carry their original two-clause BSD licence headers. New
files in this repository are offered under the same terms.
