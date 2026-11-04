// SPDX-License-Identifier: MIT
/*
 * Pipes, in the kernel. pipetest (tests/kernel/test_user.c) checks them
 * from ring 3; this checks what a program cannot make happen.
 */
#include <core/ktest.h>
#include <core/preempt.h>
#include <fs/file.h>
#include <fs/pipe.h>
#include <lib/errno.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>

/* Out of memory at each allocation pipe_create() makes: -ENOMEM, and nothing kept. */
KTEST(pipe_create_out_of_memory_keeps_nothing)
{
    u32 files = file_count(), pipes = pipe_count();
    struct kmalloc_stats k0, k1;
    int failed = 0;

    kmalloc_get_stats(&k0);
    for (long n = 0; n < 8; n++) {
        struct file *rd = NULL, *wr = NULL;
        int err;

        preempt_disable();
        long previous = pmm_test_fail_after(n);

        err = pipe_create(&rd, &wr);
        (void)pmm_test_fail_after(previous);
        preempt_enable();
        if (err) {
            KEXPECT_EQ(err, -ENOMEM);
            failed++;
            continue;
        }
        file_put(rd);
        file_put(wr);
    }
    KEXPECT(failed > 0); /* the ring at least needs a fresh page */
    kmalloc_get_stats(&k1);
    KEXPECT_EQ(k1.small_live, k0.small_live);
    KEXPECT_EQ(k1.large_live, k0.large_live);
    KEXPECT_EQ(file_count(), files);
    KEXPECT_EQ(pipe_count(), pipes);
}
