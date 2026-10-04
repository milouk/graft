#!/usr/bin/env python3
"""
mutate.py — check that the tests can fail.

Each entry below introduces one deliberate bug into the code under test,
rebuilds from scratch, and runs the bare-metal test kernel. A bug the tests do
not notice is reported as NOT DETECTED, and that is a gap in the tests.

The working tree must be clean for the files being mutated; every mutation is
reverted with `git checkout` afterwards.

Usage:  ./test/mutation/mutate.py
"""
import subprocess
import sys

MUTATIONS = [
    ("nested page table: leaf entry without the user bit", "port/npt.c",
     "\tuint64_t pte = PTE_P | PTE_U;", "\tuint64_t pte = PTE_P;"),
    ("2M page: page-size bit missing", "port/npt.c",
     "pd->entries[idx] = hpa | npt_leaf_bits(prot) | PTE_PS;",
     "pd->entries[idx] = hpa | npt_leaf_bits(prot);"),
    ("2M page: split maps every 4K page to the first one", "port/npt.c",
     "pt->entries[i] = (base + (uint64_t)i * NPT_PAGE_SIZE) | bits;",
     "pt->entries[i] = base | bits;"),
    ("2M page: partial unmap removes nothing", "port/npt.c",
     "\t\t\tif ((lo < start || hi > end) &&\n\t\t\t    npt_split(npt, t, i) == 0) {",
     "\t\t\tif (lo < start || hi > end) {\n\t\t\t\tcontinue;"),
    ("2M page: used although the host address is not aligned", "port/nvmm_port_vm.c",
     "\tif (left < NPT_LARGE_SIZE || (gpa & (NPT_LARGE_SIZE - 1)) != 0 ||\n\t    (hpa & (NPT_LARGE_SIZE - 1)) != 0)",
     "\tif (left < NPT_LARGE_SIZE || (gpa & (NPT_LARGE_SIZE - 1)) != 0)"),
    ("nested page table: intermediate entry without the user bit", "port/npt.c",
     "t->entries[idx] = child->pa | PTE_P | PTE_W | PTE_U;",
     "t->entries[idx] = child->pa | PTE_P | PTE_W;"),
    ("guest memory: read-only mapped writable", "port/nvmm_port_vm.c",
     "\tif (prot & PROT_WRITE)\n\t\tnprot |= NPT_PROT_WRITE;",
     "\tnprot |= NPT_PROT_WRITE;"),
    ("guest memory: unmap leaves pages mapped", "port/nvmm_port_vm.c",
     "\tif (npt_unmap(&vs->npt, start, end - start) != 0) {", "\tif (0) {"),
    ("guest memory: object reference leaked on destroy", "port/nvmm_port_vm.c",
     "\t\tnext = m->next;\n\t\tos_vmobj_rel(m->obj);\n\t\tport_free(m, sizeof(*m));\n\t}\n\tnpt_destroy",
     "\t\tnext = m->next;\n\t\tport_free(m, sizeof(*m));\n\t}\n\tnpt_destroy"),
    ("guest memory: task mapping never unmapped", "port/nvmm_port_vm.c",
     "\t\t\tport_membuf_unmap(m->cookie);\n\t\t\tos_vmobj_rel(m->obj);\n\t\t\tport_free(m, sizeof(*m));\n\t\t\tport_mtx_lock(&port_maps_lock);\n\t\t\tmp = &port_maps[map->kind];",
     "\t\t\tos_vmobj_rel(m->obj);\n\t\t\tport_free(m, sizeof(*m));\n\t\t\tport_mtx_lock(&port_maps_lock);\n\t\t\tmp = &port_maps[map->kind];"),
    ("guest memory: partial unmap drops the tail of a mapping", "port/nvmm_port_vm.c",
     "\t\t\ttail->next = m->next;\n\t\t\tm->size = start - mstart;\n\t\t\tm->next = tail;",
     "\t\t\ttail->next = m->next;\n\t\t\tm->size = start - mstart;"),
    ("world switch: host VMLOAD missing", "src/x86/nvmm_x86_svmfunc.S",
     "\tpopq\t%rax\n\tvmload\t%rax\n\n\t/* Restore the Host GPRs. */",
     "\tpopq\t%rax\n\n\t/* Restore the Host GPRs. */"),
    ("world switch: host VMSAVE missing", "src/x86/nvmm_x86_svmfunc.S",
     "\tmovq\t%rdx,%rax\n\tvmsave\t%rax\n\tpushq\t%rdx", "\tmovq\t%rdx,%rax\n\tpushq\t%rdx"),
    ("world switch: guest registers not loaded", "src/x86/nvmm_x86_svmfunc.S",
     "\tpushq\t%rdx\n\n\t/* Prepare RAX. */\n\tpushq\t%rsi\n\tpushq\t%rdi\n\n\t/* Restore the Guest GPRs. */\n\tmovq\t%rsi,%rax\n\tGUEST_RESTORE_GPRS(%rax)",
     "\tpushq\t%rdx\n\n\t/* Prepare RAX. */\n\tpushq\t%rsi\n\tpushq\t%rdi\n\n\t/* Restore the Guest GPRs. */\n\tmovq\t%rsi,%rax"),
    ("FPU: host state not restored", "port/nvmm_port_x86.h",
     "\tx86_restore_fpu(pc->hfpu, port_xsave_features);\n\tif (pc->cr0 & CR0_TS)",
     "\tif (pc->cr0 & CR0_TS)"),
    ("FPU: guest state not saved", "port/nvmm_port_x86.h",
     "static inline void\nx86_save_fpu(void *area, uint64_t mask)\n{\n\tif (mask != 0) {",
     "static inline void\nx86_save_fpu(void *area, uint64_t mask)\n{\n\tif (area != port_pcpu[port_curcpu()].hfpu) return;\n\tif (mask != 0) {"),
    ("debug registers: DR7 not restored", "port/nvmm_port_x86.h",
     "\tx86_set_dr7(pc->dr7);\n}", "}"),
    ("debug registers: DR0 not restored", "port/nvmm_port_x86.h",
     "\tx86_set_dr0(pc->dr[0]);\n\tx86_set_dr1(pc->dr[1]);", "\tx86_set_dr1(pc->dr[1]);"),
    ("run loop: every exit bounced to userland", "src/x86/nvmm_x86_svm.c",
     "\t\tif (host_intr || ++inkernel_exits >= NVMM_PORT_EXIT_BUDGET) {",
     "\t\tif (1) {"),
    ("run loop: no cap on exits handled in a row", "src/x86/nvmm_x86_svm.c",
     "\t\tif (host_intr || ++inkernel_exits >= NVMM_PORT_EXIT_BUDGET) {",
     "\t\tif (host_intr) {"),
    ("sleep/wake: vCPU allowed to run while suspended", "src/x86/nvmm_x86_svm.c",
     "\t\t\tif (__predict_false(svm_suspended)) {", "\t\t\tif (0) {"),
    ("sleep/wake: SVM not re-enabled on resume", "src/x86/nvmm_x86_svm.c",
     "\tos_ipi_broadcast(svm_change_cpu, (void *)true);\n\tsvm_suspended = false;",
     "\tsvm_suspended = false;"),
]

# Bugs the emulator cannot reveal. They are run anyway, so that this list is
# kept honest: if one of them starts being detected, it should move up.
KNOWN_BLIND = [
    # QEMU's software AMD-V flushes its translation cache on every world
    # switch, so a guest never sees a stale mapping even if the engine forgets
    # to ask for a TLB flush. Only real hardware can show this one.
    ("engine: address-space generation ignored (TLB flush skipped)",
     "src/x86/nvmm_x86_svm.c",
     "\tmachgen = os_vmspace_gen(mach->vm);", "\tmachgen = cpudata->vcpu_htlb_gen;"),
]

MARKERS = ("FAIL test", "PANIC", "TRAP", "unexpected exit", "LINK FAILED")


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def run_tests():
    sh("rm -rf build/bare")
    mk = sh("make bare")
    if mk.returncode != 0:
        err = [l for l in (mk.stdout + mk.stderr).splitlines() if "error" in l]
        return None, "does not compile: " + (err[0][:120] if err else "?")
    r = sh("./test/baremetal/run.sh")
    out = (r.stdout + r.stderr).splitlines()
    hit = [l.strip() for l in out if any(m in l for m in MARKERS)]
    return r.returncode == 0, (hit[0][:140] if hit else (out[-1][:140] if out else ""))


def main():
    dirty = sh("git status --porcelain -- src port").stdout.strip()
    if dirty:
        sys.exit("refusing to run: src/ or port/ has uncommitted changes:\n" + dirty)

    undetected = invalid = 0
    for name, path, old, new in MUTATIONS + KNOWN_BLIND:
        blind = (name, path, old, new) in KNOWN_BLIND
        src = open(path).read()
        if src.count(old) != 1:
            print(f"INVALID       {name}: pattern found {src.count(old)} times")
            invalid += 1
            continue
        open(path, "w").write(src.replace(old, new))
        try:
            passed, detail = run_tests()
        finally:
            sh(f"git checkout -q -- {path}")
        if passed is None:
            print(f"INVALID       {name}: {detail}")
            invalid += 1
        elif passed and blind:
            print(f"blind spot    {name}", flush=True)
        elif passed:
            print(f"NOT DETECTED  {name}", flush=True)
            undetected += 1
        elif blind:
            print(f"now detected  {name}: move it out of KNOWN_BLIND", flush=True)
        else:
            print(f"detected      {name}: {detail}", flush=True)

    passed, detail = run_tests()
    print("unmutated tree:", "PASS" if passed else f"FAIL ({detail})")
    print(f"{len(MUTATIONS) - undetected - invalid} of {len(MUTATIONS)} detected, "
          f"{undetected} not detected, {invalid} invalid, "
          f"{len(KNOWN_BLIND)} known blind spot(s)")
    sys.exit(0 if passed and undetected == 0 and invalid == 0 else 1)


if __name__ == "__main__":
    main()
