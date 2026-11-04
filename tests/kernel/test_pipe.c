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

/*
 * Each allocation pipe_create() makes fails in turn, the others
 * succeeding: -ENOMEM each time, with nothing kept. Then, with none
 * failing, a pipe is made.
 */
KTEST(pipe_create_out_of_memory_keeps_nothing)
{
    u32 files = file_count(), pipes = pipe_count();
    struct kmalloc_stats k0, k1;
    int failed = 0, made = 0;

    kmalloc_get_stats(&k0);
    for (long n = 0; n < 8; n++) {
        struct file *rd = NULL, *wr = NULL;
        int err;

        preempt_disable();
        long previous = kmalloc_test_fail_one(n);

        err = pipe_create(&rd, &wr);
        (void)kmalloc_test_fail_one(previous);
        preempt_enable();
        if (err) {
            KEXPECT_EQ(err, -ENOMEM);
            KEXPECT_EQ(made, 0); /* past the last allocation, nothing fails */
            failed++;
            continue;
        }
        made++;
        KEXPECT_EQ(pipe_count(), pipes + 1);
        file_put(rd);
        file_put(wr);
    }
    KEXPECT_EQ(failed, 4); /* the pipe, its two ends, its ring */
    kmalloc_get_stats(&k1);
    KEXPECT_EQ(k1.small_live, k0.small_live);
    KEXPECT_EQ(k1.large_live, k0.large_live);
    KEXPECT_EQ(file_count(), files);
    KEXPECT_EQ(pipe_count(), pipes);
}
