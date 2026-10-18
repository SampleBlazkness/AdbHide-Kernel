// SPDX-License-Identifier: MIT
//
// AdbHide native layer.
//
// An app process inherits the property-area mappings from zygote (r--s), so it
// never re-opens the files and the kernel module's file-layer redirect cannot
// reach it. Hook functions only cover the libraries that call them, and an app
// loads most of its native code after specialization.
//
// Instead we replace the inherited mapping with a private copy of the same
// file at the same address, patch the values in that copy, and drop write
// permission. Every reader in the process (bionic, JNI, reflection, mmap)
// then sees consistent values, and the path in /proc/self/maps is unchanged
// apart from the p/s flag.
//
// The module library itself is unloaded right after specialization
// (DLCLOSE_MODULE_LIBRARY), so it does not show up in the app's maps.

#include <jni.h>
#include <unistd.h>
#include <fcntl.h>
#include <link.h>
#include <elf.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <android/log.h>

#include "zygisk.hpp"

#define LOG_TAG "AdbHideNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define UIDS_FILE "/data/adb/adbhide/uids.txt"
#define MAX_TARGETS 64
#define PROP_VALUE_MAX 92
#define PROP_AREA_HDR 128
#define PROP_TRIE_NAME 20

struct Spoof {
    const char *name;
    const char *value;
};

/* contexts that carry adb-relevant keys */
static const char *const kContexts[] = {
    "u:object_r:system_prop:s0",
    "u:object_r:usb_control_prop:s0",
    "u:object_r:usb_config_prop:s0",
    "u:object_r:init_service_status_prop:s0",
};

static const Spoof kSpoof[] = {
    { "persist.sys.usb.config", "mtp" },
    { "sys.usb.config", "mtp" },
    { "sys.usb.state", "mtp" },
    { "persist.sys.usb.state", "mtp" },
    { "init.svc.adbd", "stopped" },
    { "ro.debuggable", "0" },
    { "ro.secure", "1" },
    { "ro.adb.secure", "1" },
    { "service.adb.root", "0" },
};

static int g_uids[MAX_TARGETS];
static int g_uid_count;

/* ---------- bionic property area ---------- */

static inline uint32_t rd32(const void *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static int cmp_name(const char *a, uint32_t alen, const char *b, uint32_t blen) {
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return memcmp(a, b, alen);
}

static ptrdiff_t find_prop_info(const uint8_t *data, size_t len, const char *name) {
    const char *rest = name;
    const uint8_t *cur = data;

    for (;;) {
        const char *sep = strchr(rest, '.');
        uint32_t tok = sep ? (uint32_t)(sep - rest) : (uint32_t)strlen(rest);
        const uint8_t *node;
        uint32_t off;
        bool hit = false;

        if (!tok) return -1;
        if ((size_t)(cur - data) + PROP_TRIE_NAME > len) return -1;
        off = rd32(cur + 16);
        if (!off || off >= len) return -1;

        node = data + off;
        while (node) {
            if ((size_t)(node - data) + PROP_TRIE_NAME > len) return -1;
            uint32_t nlen = rd32(node);
            if (nlen > 200 || (size_t)(node - data) + PROP_TRIE_NAME + nlen > len) return -1;
            int r = cmp_name(rest, tok, (const char *)(node + PROP_TRIE_NAME), nlen);
            if (r == 0) {
                hit = true;
                break;
            }
            uint32_t next = rd32(node + (r < 0 ? 8 : 12));
            node = (next && next < len) ? data + next : nullptr;
        }
        if (!hit) return -1;

        if (!sep) {
            uint32_t poff = rd32(node + 4);
            return (poff && poff < len) ? (ptrdiff_t)poff : -1;
        }
        cur = node;
        rest = sep + 1;
    }
}

static int patch_area(uint8_t *base, size_t len) {
    if (len < PROP_AREA_HDR + 8) return 0;
    if (rd32(base + 8) != 0x504f5250) return 0;

    int patched = 0;
    for (const auto &s : kSpoof) {
        ptrdiff_t off = find_prop_info(base + PROP_AREA_HDR, len - PROP_AREA_HDR, s.name);
        if (off < 0) continue;
        size_t at = PROP_AREA_HDR + (size_t)off + 4;
        if (at + PROP_VALUE_MAX > len) continue;
        memset(base + at, 0, PROP_VALUE_MAX);
        strncpy((char *)(base + at), s.value, PROP_VALUE_MAX - 1);
        patched++;
    }
    return patched;
}

static bool wanted_context(const char *path) {
    for (const char *c : kContexts) {
        if (strstr(path, c)) return true;
    }
    return false;
}

static int harden_process() {
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) return 0;

    struct Region {
        uintptr_t start, end;
        char path[256];
    };
    Region regions[32];
    int nregions = 0;
    char line[512];

    while (nregions < 32 && fgets(line, sizeof(line), f)) {
        if (!strstr(line, "/dev/__properties__/")) continue;
        unsigned long start = 0, end = 0;
        char perms[8] = {0}, path[256] = {0};
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &start, &end, perms, path) != 4)
            continue;
        if (!wanted_context(path)) continue;
        regions[nregions].start = start;
        regions[nregions].end = end;
        snprintf(regions[nregions].path, sizeof(regions[nregions].path), "%s", path);
        nregions++;
    }
    fclose(f);

    int total = 0;
    for (int i = 0; i < nregions; i++) {
        size_t len = regions[i].end - regions[i].start;
        int fd = open(regions[i].path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            LOGE("open %s failed", regions[i].path);
            continue;
        }
        void *p = mmap((void *)regions[i].start, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_FIXED, fd, 0);
        if (p == MAP_FAILED) {
            LOGE("remap %s failed", regions[i].path);
            close(fd);
            continue;
        }
        total += patch_area((uint8_t *)p, len);
        mprotect(p, len, PROT_READ);
        close(fd);
    }
    return total;
}

/* ---------- uid list ---------- */

static bool read_full(int fd, void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t r = read(fd, (char *)buf + done, len - done);
        if (r <= 0) return false;
        done += (size_t)r;
    }
    return true;
}

static bool write_full(int fd, const void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t r = write(fd, (const char *)buf + done, len - done);
        if (r <= 0) return false;
        done += (size_t)r;
    }
    return true;
}

static void load_uids(zygisk::Api *api) {
    int fd = api->connectCompanion();
    if (fd < 0) return;
    int32_t n = 0;
    if (read_full(fd, &n, sizeof(n)) && n > 0 && n <= MAX_TARGETS) {
        if (read_full(fd, g_uids, sizeof(int32_t) * n)) g_uid_count = n;
    }
    close(fd);
}

static bool uid_is_target(int uid) {
    for (int i = 0; i < g_uid_count; i++) {
        if (g_uids[i] == uid) return true;
    }
    return false;
}

/* ---------- zygisk plumbing ---------- */

class AdbHideNative : public zygisk::ModuleBase {
  public:
    void onLoad(zygisk::Api *api, JNIEnv *) override { this->api = api; }

    void preAppSpecialize(zygisk::AppSpecializeArgs *) override {
        /* the module library must not stay mapped in the app: it would show up
         * in /proc/self/maps as /data/adb/modules/adbhide_native/... */
        api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        load_uids(api);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        int uid = getuid();
        if (!g_uid_count || !uid_is_target(uid)) return;
        int n = harden_process();
        LOGI("uid=%d patched=%d", uid, n);
    }

  private:
    zygisk::Api *api = nullptr;
};

REGISTER_ZYGISK_MODULE(AdbHideNative)

static void companion_entry(int fd) {
    int32_t out[MAX_TARGETS];
    int32_t n = 0;
    FILE *f = fopen(UIDS_FILE, "re");
    if (f) {
        char line[256];
        while (n < MAX_TARGETS && fgets(line, sizeof(line), f)) {
            int v = atoi(line);
            if (v >= 10000 && v <= 99999) out[n++] = v;
        }
        fclose(f);
    }
    if (!write_full(fd, &n, sizeof(n))) return;
    if (n) write_full(fd, out, sizeof(int32_t) * n);
}

REGISTER_ZYGISK_COMPANION(companion_entry)
