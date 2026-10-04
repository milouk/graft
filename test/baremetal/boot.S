/*
 * boot.S — entry for the bare-metal test kernel.
 *
 * QEMU's PVH loader starts us in 32-bit protected mode with paging off. This
 * builds an identity map of the first 4 GiB with 2 MiB pages, switches to
 * long mode and calls kmain().
 */

	.section .note.Xen, "a", @note
	.balign 4
	.long	4			/* name size */
	.long	4			/* desc size */
	.long	18			/* XEN_ELFNOTE_PHYS32_ENTRY */
	.asciz	"Xen"
	.long	pvh_start

	.section .text.boot, "ax"
	.code32
	.globl	pvh_start
pvh_start:
	cli
	movl	$boot_stack_top, %esp

	/* PML4[0] -> PDPT */
	movl	$boot_pdpt, %eax
	orl	$0x3, %eax
	movl	%eax, boot_pml4

	/* PDPT[0..3] -> PD[0..3] */
	movl	$boot_pd, %eax
	orl	$0x3, %eax
	movl	$boot_pdpt, %edi
	movl	$4, %ecx
1:	movl	%eax, (%edi)
	addl	$0x1000, %eax
	addl	$8, %edi
	loop	1b

	/* 2048 entries of 2 MiB each: present, writable, large. */
	movl	$boot_pd, %edi
	xorl	%ebx, %ebx
2:	movl	%ebx, %eax
	shll	$21, %eax
	orl	$0x83, %eax
	movl	%eax, (%edi)
	movl	%ebx, %eax
	shrl	$11, %eax
	movl	%eax, 4(%edi)
	addl	$8, %edi
	incl	%ebx
	cmpl	$2048, %ebx
	jne	2b

	movl	$boot_pml4, %eax
	movl	%eax, %cr3

	/* PAE, plus OSFXSR and OSXMMEXCPT so SSE works in the test kernel. */
	movl	%cr4, %eax
	orl	$0x620, %eax
	movl	%eax, %cr4

	/* EFER: long mode enable, NX enable. */
	movl	$0xC0000080, %ecx
	rdmsr
	orl	$0x900, %eax
	wrmsr

	/* Paging on; MP and NE set, EM and TS clear. */
	movl	%cr0, %eax
	andl	$0xFFFFFFF3, %eax
	orl	$0x80000023, %eax
	movl	%eax, %cr0

	lgdt	boot_gdt_desc
	ljmp	$0x08, $long_entry

	.code64
long_entry:
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	xorl	%eax, %eax
	movw	%ax, %fs
	movw	%ax, %gs
	movq	$boot_stack_top, %rsp
	xorl	%ebp, %ebp
	call	kmain
3:	hlt
	jmp	3b

/*
 * Exception stubs. Anything that traps here is a bug in the test or in the
 * code under test, so the handler just reports it and stops the machine.
 */
	.macro	TRAP_NOERR n
	.globl	trap_stub_\n
trap_stub_\n:
	pushq	$0
	pushq	$\n
	jmp	trap_common
	.endm

	.macro	TRAP_ERR n
	.globl	trap_stub_\n
trap_stub_\n:
	pushq	$\n
	jmp	trap_common
	.endm

	TRAP_NOERR 0
	TRAP_NOERR 1
	TRAP_NOERR 2
	TRAP_NOERR 3
	TRAP_NOERR 4
	TRAP_NOERR 5
	TRAP_NOERR 6
	TRAP_NOERR 7
	TRAP_ERR   8
	TRAP_NOERR 9
	TRAP_ERR   10
	TRAP_ERR   11
	TRAP_ERR   12
	TRAP_ERR   13
	TRAP_ERR   14
	TRAP_NOERR 15
	TRAP_NOERR 16
	TRAP_ERR   17
	TRAP_NOERR 18
	TRAP_NOERR 19
	TRAP_NOERR 20
	TRAP_ERR   21
	TRAP_NOERR 22
	TRAP_NOERR 23
	TRAP_NOERR 24
	TRAP_NOERR 25
	TRAP_NOERR 26
	TRAP_NOERR 27
	TRAP_NOERR 28
	TRAP_ERR   29
	TRAP_ERR   30
	TRAP_NOERR 31

trap_common:
	/* Stack: vector, error code, rip, cs, rflags, rsp, ss */
	movq	0(%rsp), %rdi
	movq	8(%rsp), %rsi
	movq	16(%rsp), %rdx
	movq	%cr2, %rcx
	call	trap_handler
4:	hlt
	jmp	4b

	.section .rodata
	.globl	trap_stubs
	.balign 8
trap_stubs:
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	.quad	trap_stub_\n
	.endr

	.section .data
	.balign 16
	.globl	boot_gdt
boot_gdt:
	.quad	0x0000000000000000	/* 0x00 null */
	.quad	0x00209A0000000000	/* 0x08 kernel code, 64-bit */
	.quad	0x0000920000000000	/* 0x10 kernel data */
	.quad	0x0000F20000000000	/* 0x18 user data */
	.quad	0x0020FA0000000000	/* 0x20 user code, 64-bit */
	.quad	0			/* 0x28 TSS, low half  (filled in C) */
	.quad	0			/* 0x30 TSS, high half */
boot_gdt_end:

boot_gdt_desc:
	.word	boot_gdt_end - boot_gdt - 1
	.long	boot_gdt
	.long	0

	.section .bss
	.balign 4096
boot_pml4:
	.skip	4096
boot_pdpt:
	.skip	4096
boot_pd:
	.skip	4096 * 4
	.balign 16
	.skip	65536
boot_stack_top:
