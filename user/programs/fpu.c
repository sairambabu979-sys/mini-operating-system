// SPDX-License-Identifier: MIT
/*
 * Fills all 16 XMM registers with a pid-specific pattern, then yields
 * hundreds of times while other processes do the same. Any register that
 * changed means FPU state leaked between processes. Written in assembly so
 * the compiler cannot use the XMM registers in between.
 */
#include "libsys.h"

int fpu_check(u64 seed, int rounds);

__asm__(".text\n"
        ".global fpu_check\n"
        "fpu_check:\n" /* rdi = seed, esi = rounds */
        "  mov %rdi, %rax\n"
        "  .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n"
        "  movq %rax, %xmm\\n\n"
        "  inc %rax\n"
        "  .endr\n"
        "1:\n"
        "  mov $2, %eax\n" /* SYS_YIELD: clobbers rax, rcx, r11 only */
        "  syscall\n"
        "  mov %rdi, %rdx\n"
        "  .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n"
        "  movq %xmm\\n, %rax\n"
        "  cmp %rdx, %rax\n"
        "  jne 2f\n"
        "  inc %rdx\n"
        "  .endr\n"
        "  dec %esi\n"
        "  jnz 1b\n"
        "  xor %eax, %eax\n"
        "  ret\n"
        "2:\n"
        "  mov $1, %eax\n"
        "  ret\n");

int main(void)
{
    long pid = sys_getpid();
    u64 seed = 0x1111111111111111ull * (u64)(pid % 15 + 1);
    int bad = fpu_check(seed, 300);

    printf("fpu[%ld]: XMM state %s across 300 yields\n", pid, bad ? "CORRUPTED" : "intact");
    return bad;
}
