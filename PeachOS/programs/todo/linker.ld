/* linker script for todo.elf.
   nothing in here is novel - it mirrors what the other PeachOS user
   programs (shell, blank) use, because the kernel's ELF loader expects
   exactly this layout. if you change the base address or the section
   ordering you will get a process that loads but immediately faults. */

/* _start lives in the stdlib's startup assembly; that's the symbol
   the loader jumps to first. it sets up the C runtime and then calls
   our main(). */
ENTRY(_start)

/* 32-bit i386 ELF, because that's what the loader knows how to parse. */
OUTPUT_FORMAT(elf32-i386)

SECTIONS
{
    /* 0x400000 is where PeachOS maps user processes. has to match the
       loader's expectations exactly. */
    . = 0x400000;

    /* code. page-aligned because paging is set up in 4 KB units. */
    .text : ALIGN(4096)
    {
        *(.text)
    }

    /* dedicated section for hand-written assembly bits from stdlib's
       startup/runtime - kept separate so the linker doesn't merge them
       into .text in surprising ways. */
    .asm : ALIGN(4096)
    {
        *(.asm)
    }

    /* string literals and other read-only constants. */
    .rodata : ALIGN(4096)
    {
        *(.rodata)
    }

    /* initialized globals/statics. */
    .data : ALIGN(4096)
    {
        *(.data)
    }

    /* zero-initialized globals + COMMON. the loader is responsible for
       actually zeroing this region at process start. */
    .bss : ALIGN(4096)
    {
        *(COMMON)
        *(.bss)
    }
}
