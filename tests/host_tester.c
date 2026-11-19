/* adbng - SPDX-License-Identifier: MIT
 *
 * Minimal ADB *host* used to test usbgwd without a PC: it drives the protocol
 * over a socketpair, so the whole handshake runs inside the phone.
 *
 * usage: host_tester <usbgwd-path> <adb_keys> <hostkey.txt> [bad]
 */
#include "proto.h"
#include "rsa.h"
#include "util.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fd = -1;
static uint32_t g_next_id = 1;

static void send_msg(uint32_t cmd, uint32_t a0, uint32_t a1, const void *data, size_t len)
{
    struct amessage m;
    memset(&m, 0, sizeof m);
    m.command = cmd;
    m.arg0 = a0;
    m.arg1 = a1;
    m.data_length = (uint32_t)len;
    m.magic = amsg_magic(cmd);
    printf("[host] -> %-4s arg0=%u arg1=%u len=%zu\n", amsg_name(cmd), a0, a1, len);
    io_write_full(g_fd, &m, AMSG_SIZE);
    if (len) io_write_full(g_fd, data, len);
}

/* returns 1 on a message, 0 on clean EOF, -1 on error/timeout */
static int recv_msg(uint32_t *cmd, uint32_t *a0, uint32_t *a1, unsigned char **data,
                    uint32_t *len, int timeout_ms)
{
    struct pollfd p = { .fd = g_fd, .events = POLLIN };
    int pr = poll(&p, 1, timeout_ms);
    if (pr <= 0) return -1;

    struct amessage m;
    if (io_read_full(g_fd, &m, AMSG_SIZE) != 0) return 0;
    if (m.magic != amsg_magic(m.command)) {
        printf("[host] bad magic %08x\n", m.command);
        return -1;
    }
    unsigned char *buf = NULL;
    if (m.data_length) {
        buf = malloc(m.data_length + 1);
        if (!buf) return -1;
        if (io_read_full(g_fd, buf, m.data_length) != 0) { free(buf); return 0; }
        buf[m.data_length] = 0;
    }
    *cmd = m.command;
    *a0 = m.arg0;
    *a1 = m.arg1;
    *data = buf;
    *len = m.data_length;
    printf("[host] <- %-4s arg0=%u arg1=%u len=%u\n", amsg_name(m.command), m.arg0, m.arg1,
           m.data_length);
    return 1;
}

static int load_hostkey(const char *path, uint32_t n[RSA_LIMBS], uint32_t d[RSA_LIMBS])
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1200];
    int have_n = 0, have_d = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "n=", 2)) {
            if (rsa_limbs_from_hex(line + 2, n) != 0) return -1;
            have_n = 1;
        } else if (!strncmp(line, "d=", 2)) {
            if (rsa_limbs_from_hex(line + 2, d) != 0) return -1;
            have_d = 1;
        }
    }
    fclose(f);
    return (have_n && have_d) ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <usbgwd> <adb_keys> <hostkey.txt> [bad]\n", argv[0]);
        return 2;
    }
    int bad_signature = (argc > 4 && !strcmp(argv[4], "bad"));

    uint32_t n[RSA_LIMBS], d[RSA_LIMBS];
    if (load_hostkey(argv[3], n, d) != 0) {
        fprintf(stderr, "cannot read host key %s\n", argv[3]);
        return 2;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair");
        return 2;
    }

    pid_t child = fork();
    if (child == 0) {
        close(sv[0]);
        if (dup2(sv[1], 3) < 0) _exit(3);
        close(sv[1]);
        setenv("ADBNG_PRODUCT_NAME", "adbng", 1);
        setenv("ADBNG_PRODUCT_MODEL", "adbng-self-test", 1);
        setenv("ADBNG_PRODUCT_DEVICE", "adbng-self-test", 1);
        execl(argv[1], argv[1], "--fd", "3", "--keys", argv[2], (char *)NULL);
        _exit(4);
    }
    close(sv[1]);
    g_fd = sv[0];
    alarm(60);

    int pass = 0, stage = 0;
    uint32_t cmd, a0, a1, len;
    unsigned char *data = NULL;

    /* 1. device banner */
    if (recv_msg(&cmd, &a0, &a1, &data, &len, 10000) != 1 || cmd != A_CNXN) goto done;
    printf("[host] banner: %s\n", data ? (char *)data : "");
    free(data); data = NULL;

    /* 2. host banner */
    const char banner[] = "host::features=shell_v2,cmd";
    send_msg(A_CNXN, A_VERSION, A_MAXDATA, banner, sizeof banner);

    /* 3. auth token */
    if (recv_msg(&cmd, &a0, &a1, &data, &len, 10000) != 1 || cmd != A_AUTH || a0 != AUTH_TOKEN) goto done;
    unsigned char token[TOKEN_SIZE];
    memcpy(token, data, TOKEN_SIZE);
    free(data); data = NULL;
    printf("[host] token: ");
    for (int i = 0; i < TOKEN_SIZE; i++) printf("%02x", token[i]);
    printf("\n");

    /* 4. signature */
    unsigned char sig[RSA_SIG_SIZE];
    rsa_sign_sha1(n, d, token, TOKEN_SIZE, sig);
    if (bad_signature) sig[100] ^= 0xff;
    send_msg(A_AUTH, AUTH_SIGNATURE, 0, sig, sizeof sig);

    /* 5. expect either a fresh token (rejected) or the banner again (accepted) */
    if (recv_msg(&cmd, &a0, &a1, &data, &len, 20000) != 1) goto done;
    if (bad_signature) {
        if (cmd == A_AUTH && a0 == AUTH_TOKEN) {
            printf("[host] RESULT: bad signature correctly rejected\n");
            pass = 1;
        }
        free(data);
        goto done;
    }
    if (cmd != A_CNXN) { free(data); goto done; }
    printf("[host] authenticated, banner again: %s\n", data ? (char *)data : "");
    free(data); data = NULL;

    /* 6. open the echo service */
    const char svc[] = "echo:hello-adbng";
    uint32_t my_id = g_next_id++;
    send_msg(A_OPEN, my_id, 0, svc, sizeof svc);

    /* 7. OKAY, WRTE, CLSE */
    if (recv_msg(&cmd, &a0, &a1, &data, &len, 20000) != 1 || cmd != A_OKAY) goto done;
    uint32_t peer = a0;
    free(data); data = NULL;

    if (recv_msg(&cmd, &a0, &a1, &data, &len, 20000) != 1 || cmd != A_WRTE) goto done;
    printf("[host] service payload: %s", data ? (char *)data : "");
    free(data); data = NULL;

    if (recv_msg(&cmd, &a0, &a1, &data, &len, 20000) != 1 || cmd != A_CLSE) goto done;
    free(data); data = NULL;
    (void)peer;

    printf("[host] RESULT: handshake + auth + channel round trip OK\n");
    pass = 1;

done:
    if (!pass) printf("[host] RESULT: FAILED at stage %d\n", stage);
    close(g_fd);
    int st = 0;
    waitpid(child, &st, 0);
    printf("[host] child exit status %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    return pass ? 0 : 1;
}