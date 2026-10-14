// SPDX-License-Identifier: MIT
/*
 * The first user process: starts a tour of the system, reaps every child,
 * and reports how each one ended. Exit status 0 iff every child ended the
 * way it should have, including the ones the kernel was supposed to kill.
 */
#include "libsys.h"

struct child {
    const char *name;
    int expected; /* exit status */
    long pid;
};

int main(void)
{
    struct child children[] = {
        {"hello", 42, 0},
        {"spinner", 0, 0},
        {"spinner", 0, 0},
        {"sleeper", 0, 0},
        {"fpu", 0, 0},
        {"fpu", 0, 0},
        {"badptr", 0, 0},
        {"faulter", EXIT_STATUS_FAULT(14), 0},    /* #PF */
        {"privileged", EXIT_STATUS_FAULT(13), 0}, /* #GP */
        {"kread", EXIT_STATUS_FAULT(14), 0},
        {"selfmod", EXIT_STATUS_FAULT(14), 0},
    };
    const int n = (int)(sizeof(children) / sizeof(children[0]));
    int wrong = 0;

    printf("init: pid %ld, starting %d programs\n", sys_getpid(), n);
    for (int i = 0; i < n; i++) {
        children[i].pid = sys_spawn(children[i].name);
        if (children[i].pid < 0) {
            printf("init: spawn %s failed: %ld\n", children[i].name, children[i].pid);
            wrong++;
        }
    }
    for (int done = 0; done < n;) {
        int status;
        long pid = sys_wait(-1, &status);

        if (pid < 0)
            break;
        done++;
        for (int i = 0; i < n; i++) {
            if (children[i].pid != pid)
                continue;
            bool ok = status == children[i].expected;

            printf("init: %-10s pid %-3ld exit %-3d %s\n", children[i].name, pid, status,
                   ok ? "(as expected)" : "(UNEXPECTED)");
            wrong += !ok;
        }
    }
    printf("init: all children reaped, %d unexpected\n", wrong);
    return wrong;
}
