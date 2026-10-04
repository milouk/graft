/* Loaded by QEMU's PVH loader at 1 MiB, identity-mapped. */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(pvh_start)

PHDRS
{
	text PT_LOAD FLAGS(7);
	note PT_NOTE FLAGS(4);
}

SECTIONS
{
	. = 0x100000;
	.note.Xen : { KEEP(*(.note.Xen)) } :text :note
	.text : { *(.text.boot) *(.text .text.*) } :text
	.rodata : { *(.rodata .rodata.*) } :text
	.data : { *(.data .data.*) } :text
	.bss : {
		__bss_start = .;
		*(.bss .bss.*) *(COMMON)
		__bss_end = .;
	} :text
	. = ALIGN(0x200000);
	__heap_start = .;
	/DISCARD/ : { *(.comment) *(.eh_frame) *(.note.GNU-stack) }
}
