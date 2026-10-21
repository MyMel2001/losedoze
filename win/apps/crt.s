/* crt.s — compiler helpers for freestanding PE32+ apps (original code).
 * __chkstk: ms x64 convention (rcx = bytes, probe below rsp, return size in rax). */
    .text
    .globl __chkstk
__chkstk:
    movq    %rcx, %rax
    cmpq    $4096, %rcx
    jbe     3f
    movq    %rsp, %rdx
2:
    subq    $4096, %rdx
    movq    (%rdx), %r11
    movq    %rsp, %r10
    subq    %rdx, %r10
    cmpq    %rax, %r10
    jb      2b
3:
    retq
