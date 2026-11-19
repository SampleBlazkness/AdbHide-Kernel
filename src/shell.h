/* adbng - SPDX-License-Identifier: MIT */
#ifndef ADBNG_SHELL_H
#define ADBNG_SHELL_H

#include <sys/types.h>

/* A child process spawned for a shell/exec service.  fd_in is the write end of
 * the child's stdin, fd_out/fd_err the read ends of stdout/stderr.  When
 * separate_err is 0 both streams share one pipe (fd_err == -1). */
struct shell_proc {
    pid_t pid;
    int fd_in;
    int fd_out;
    int fd_err;
};

void shell_proc_init(struct shell_proc *p);

/* Run `cmd` through /system/bin/sh -c (or $SHELL).  Empty cmd runs an
 * interactive shell. */
int shell_spawn_sh(const char *cmd, int separate_err, struct shell_proc *p);

/* Run argv[0] directly, no shell in between.  argv must be NULL terminated. */
int shell_spawn_argv(char *const argv[], int separate_err, struct shell_proc *p);

/* Kill the child and its process group, then reap it. */
void shell_kill(struct shell_proc *p);

#endif
