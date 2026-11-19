/* adbng - SPDX-License-Identifier: MIT
 *
 * Child process plumbing shared by the `shell:` and `exec:` services.
 *
 * Two wirings are supported:
 *
 *   pipes - separate stdin/stdout(/stderr).  Used by `shell:` without pty,
 *           by `exec:` and by every non-interactive path.
 *   pty   - one pseudo-terminal for the child.  isatty() is true, the line
 *           discipline turns ^C into SIGINT, and full-screen programs work.
 *
 * Either way the child runs in its own session (setsid), so a killed shell
 * does not take the daemon with it and we can signal the whole group.
 */
#include "shell.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define DEFAULT_PATH "/sbin:/system/sbin:/system/bin:/system/xbin:/usr/local/bin:/usr/bin:/bin"
#define DEFAULT_ROWS 24
#define DEFAULT_COLS 80

void shell_proc_init(struct shell_proc *p)
{
    p->pid = -1;
    p->fd_in = -1;
    p->fd_out = -1;
    p->fd_err = -1;
    p->pty = 0;
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

struct spawn_ctx {
    void (*body)(void *);
    void *arg;
    const char *term;
    int pty;
};

static void child_env(const struct spawn_ctx *ctx)
{
    setenv("PATH", DEFAULT_PATH, 1);
    if (!getenv("HOME")) setenv("HOME", "/", 1);
    if (ctx->term && *ctx->term) {
        setenv("TERM", ctx->term, 1);
    } else if (!getenv("TERM")) {
        /* "dumb" would be honest for pipes but kills every colour and
         * cursor-addressing program on a terminal. */
        setenv("TERM", ctx->pty ? "xterm-256color" : "dumb", 1);
    }
}

/* Everything a fresh child must undo before it becomes somebody else's
 * process: daemon signal policy, inherited descriptors of the other kind. */
static void child_reset_signals(void)
{
    signal(SIGPIPE, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}

static int spawn_pipes(const struct spawn_ctx *ctx, int separate_err, struct shell_proc *p)
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
        child_reset_signals();
        child_env(ctx);
        ctx->body(ctx->arg);
        _exit(127);
    }

    p->pid = pid;
    p->fd_in = in_pipe[1];
    p->fd_out = out_pipe[0];
    p->fd_err = separate_err ? err_pipe[0] : -1;
    p->pty = 0;
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

/* Open a pty master without libutil: /dev/ptmx + TIOCSPTLCK/TIOCGPTN behaves
 * the same on glibc, musl and bionic, which posix_openpt() does not promise.
 * The slave name is written into `path`. */
static int pty_master_open(char *path, size_t pathlen)
{
    int master = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) return -1;

    int unlock = 0;
    if (ioctl(master, TIOCSPTLCK, &unlock) != 0) { close(master); return -1; }

    int ptn = -1;
    if (ioctl(master, TIOCGPTN, &ptn) != 0 || ptn < 0) { close(master); return -1; }

    int n = snprintf(path, pathlen, "/dev/pts/%d", ptn);
    if (n <= 0 || (size_t)n >= pathlen) { close(master); return -1; }
    return master;
}

int shell_pty_winsize(int fd, int rows, int cols)
{
    if (fd < 0) return -1;
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)(rows > 0 ? rows : DEFAULT_ROWS);
    ws.ws_col = (unsigned short)(cols > 0 ? cols : DEFAULT_COLS);
    return ioctl(fd, TIOCSWINSZ, &ws);
}

static int spawn_pty(const struct spawn_ctx *ctx, int rows, int cols, struct shell_proc *p)
{
    char slave_path[64];
    int master = pty_master_open(slave_path, sizeof slave_path);
    if (master < 0) return -1;

    int slave = open(slave_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) { close(master); return -1; }

    /* Set the size before the child can run, so the very first program already
     * sees the size the host asked for. */
    shell_pty_winsize(master, rows, cols);

    pid_t pid = fork();
    if (pid < 0) { close(master); close(slave); return -1; }

    if (pid == 0) {
        close(master);
        setsid();
        /* Acquiring the controlling terminal is the whole point: only then
         * does the line discipline signal this process group on ^C/^\\/^Z. */
        ioctl(slave, TIOCSCTTY, 0);
        if (dup2(slave, 0) < 0) _exit(127);
        if (dup2(slave, 1) < 0) _exit(127);
        if (dup2(slave, 2) < 0) _exit(127);
        if (slave > 2) close(slave);
        child_reset_signals();
        child_env(ctx);
        ctx->body(ctx->arg);
        _exit(127);
    }

    /* The parent must not keep the slave open: as long as it does, read() on
     * the master returns EAGAIN after the child dies instead of EIO, and we
     * would never notice EOF. */
    close(slave);
    set_nonblock(master);

    p->pid = pid;
    p->fd_in = master;
    p->fd_out = master;
    p->fd_err = -1;
    p->pty = 1;
    return 0;
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

int shell_spawn_sh(const struct shell_opts *o, struct shell_proc *p)
{
    struct sh_arg a = { .cmd = o->cmd };
    struct spawn_ctx ctx = { .body = child_sh, .arg = &a, .term = o->term, .pty = 0 };

    if (o->pty) {
        ctx.pty = 1;
        if (spawn_pty(&ctx, o->rows, o->cols, p) == 0) return 0;
        /* No pty available: a pipe-backed shell is still far better than
         * refusing the channel outright. */
        logmsg("shell: pty unavailable (%s), falling back to pipes", strerror(errno));
        ctx.pty = 0;
    }
    return spawn_pipes(&ctx, o->separate_err, p);
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
    struct spawn_ctx ctx = { .body = child_argv, .arg = &a, .term = NULL, .pty = 0 };
    return spawn_pipes(&ctx, separate_err, p);
}

void shell_kill(struct shell_proc *p)
{
    int in = p->fd_in, out = p->fd_out;
    if (in >= 0) { close(in); p->fd_in = -1; }
    /* With a pty both directions are the same descriptor. */
    if (out >= 0 && out != in) { close(out); }
    p->fd_out = -1;
    if (p->fd_err >= 0) { close(p->fd_err); p->fd_err = -1; }

    if (p->pid > 0) {
        kill(-p->pid, SIGKILL);
        kill(p->pid, SIGKILL);
        while (waitpid(p->pid, NULL, 0) < 0 && errno == EINTR) { }
        p->pid = -1;
    }
}