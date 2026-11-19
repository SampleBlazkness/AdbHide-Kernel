/* adbng - SPDX-License-Identifier: MIT
 *
 * Child process plumbing shared by the `shell:` and `exec:` services.
 *
 * Both run the child in its own session (setsid) so a killed shell does not
 * take the daemon with it and so we can signal the whole group on close.
 */
#include "shell.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_PATH "/sbin:/system/sbin:/system/bin:/system/xbin:/usr/local/bin:/usr/bin:/bin"

void shell_proc_init(struct shell_proc *p)
{
    p->pid = -1;
    p->fd_in = -1;
    p->fd_out = -1;
    p->fd_err = -1;
}

static int make_pipe(int fds[2])
{
    if (pipe(fds) != 0) return -1;
    for (int i = 0; i < 2; i++)
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    return 0;
}

/* Only the ends *we* keep must be non-blocking.  The child's ends stay
 * blocking: O_NONBLOCK is a file-description flag, so setting it on the write
 * end would make the child's own writes fail with EAGAIN as soon as the pipe
 * fills (any program writing more than 64 KB would die). */
static void set_nonblock(int fd)
{
    if (fd < 0) return;
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void child_env(void)
{
    setenv("PATH", DEFAULT_PATH, 1);
    if (!getenv("HOME")) setenv("HOME", "/", 1);
    if (!getenv("TERM")) setenv("TERM", "dumb", 1);
}

/* Shared fork + wiring.  Returns 0 on success with the parent-side fds filled
 * in; the caller then execs in the child.  Because exec happens in the child
 * branch we duplicate the child setup here rather than passing callbacks. */
static int spawn_common(int separate_err, struct shell_proc *p,
                        void (*child_body)(void *arg), void *arg)
{
    int in_pipe[2] = { -1, -1 }, out_pipe[2] = { -1, -1 }, err_pipe[2] = { -1, -1 };

    if (make_pipe(in_pipe) != 0) return -1;
    if (make_pipe(out_pipe) != 0) goto fail;
    if (separate_err && make_pipe(err_pipe) != 0) goto fail;

    pid_t pid = fork();
    if (pid < 0) goto fail;

    if (pid == 0) {
        setsid();
        if (dup2(in_pipe[0], 0) < 0) _exit(127);
        if (dup2(out_pipe[1], 1) < 0) _exit(127);
        if (dup2(separate_err ? err_pipe[1] : out_pipe[1], 2) < 0) _exit(127);

        /* Signal handling must go back to defaults: the daemon ignores
         * SIGPIPE and we do not want that inherited by a shell. */
        signal(SIGPIPE, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        child_env();

        child_body(arg);
        _exit(127);
    }

    p->pid = pid;
    p->fd_in = in_pipe[1];
    p->fd_out = out_pipe[0];
    p->fd_err = separate_err ? err_pipe[0] : -1;
    set_nonblock(p->fd_in);
    set_nonblock(p->fd_out);
    set_nonblock(p->fd_err);

    close(in_pipe[0]);
    close(out_pipe[1]);
    if (separate_err) close(err_pipe[1]);
    return 0;

fail:
    for (int i = 0; i < 2; i++) {
        if (in_pipe[i] >= 0) close(in_pipe[i]);
        if (out_pipe[i] >= 0) close(out_pipe[i]);
        if (err_pipe[i] >= 0) close(err_pipe[i]);
    }
    return -1;
}

struct sh_arg {
    const char *cmd;
};

static void child_sh(void *arg)
{
    struct sh_arg *a = arg;
    if (!a->cmd || !*a->cmd) {
        execlp("sh", "sh", "-i", (char *)NULL);
        execlp("sh", "sh", (char *)NULL);
        return;
    }
    execlp("sh", "sh", "-c", a->cmd, (char *)NULL);
}

int shell_spawn_sh(const char *cmd, int separate_err, struct shell_proc *p)
{
    struct sh_arg a = { .cmd = cmd };
    return spawn_common(separate_err, p, child_sh, &a);
}

struct argv_arg {
    char *const *argv;
};

static void child_argv(void *arg)
{
    struct argv_arg *a = arg;
    if (!a->argv || !a->argv[0]) return;
    execvp(a->argv[0], a->argv);
}

int shell_spawn_argv(char *const argv[], int separate_err, struct shell_proc *p)
{
    struct argv_arg a = { .argv = argv };
    return spawn_common(separate_err, p, child_argv, &a);
}

void shell_kill(struct shell_proc *p)
{
    if (p->fd_in >= 0) { close(p->fd_in); p->fd_in = -1; }
    if (p->fd_out >= 0) { close(p->fd_out); p->fd_out = -1; }
    if (p->fd_err >= 0) { close(p->fd_err); p->fd_err = -1; }

    if (p->pid > 0) {
        kill(-p->pid, SIGKILL);
        kill(p->pid, SIGKILL);
        while (waitpid(p->pid, NULL, 0) < 0 && errno == EINTR) { }
        p->pid = -1;
    }
}
