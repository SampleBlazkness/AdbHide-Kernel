/* adbng - SPDX-License-Identifier: MIT */
#ifndef ADBNG_SHELL_H
#define ADBNG_SHELL_H

#include <sys/types.h>

/* A child process spawned for a shell/exec service.  fd_in is the write end of
 * the child's stdin, fd_out/fd_err the read ends of stdout/stderr.  When
 * separate_err is 0 both streams share one pipe (fd_err == -1).
 *
 * A pty-backed child has a single master fd instead: fd_in == fd_out and
 * fd_err == -1, with pty set.  The master carries both directions, so the
 * channel must never close fd_in on its own - that would hang up the session
 * and swallow whatever the child still had to say. */
struct shell_proc {
    pid_t pid;
    int fd_in;
    int fd_out;
    int fd_err;
    int pty;
};

void shell_proc_init(struct shell_proc *p);

struct shell_opts {
    const char *cmd;      /* NULL or "" runs an interactive shell */
    const char *term;     /* $TERM for the child; NULL keeps the environment */
    int separate_err;     /* ignored with pty: a tty always merges stderr */
    int pty;
    int rows, cols;       /* initial window size; 0 means 24x80 */
};

/* Run opts->cmd through /system/bin/sh -c, or an interactive shell when the
 * command is empty.  Falls back to pipes if the pty cannot be opened, in which
 * case p->pty is left 0. */
int shell_spawn_sh(const struct shell_opts *o, struct shell_proc *p);

/* Run argv[0] directly, no shell in between.  argv must be NULL terminated. */
int shell_spawn_argv(char *const argv[], int separate_err, struct shell_proc *p);

/* TIOCSWINSZ on a pty master; the kernel then sends SIGWINCH for us.
 * Returns 0 on success, -1 when fd is not a terminal. */
int shell_pty_winsize(int fd, int rows, int cols);

/* Kill the child and its process group, then reap it. */
void shell_kill(struct shell_proc *p);

#endif