# The first code that runs: set the stack, call the kernel written in Zevic, then wait forever.
.section .text.boot
.global _start
_start:
    la sp, stack_top
    call kmain
1:  wfi
    j 1b

.section .bss
.balign 16
stack_bottom:
    .skip 16384
.global stack_top
stack_top:
