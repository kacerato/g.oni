// LD_PRELOAD shim: log every mmap and catch the one that fails with ENOMEM.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

static void *(*real_mmap)(void *, size_t, int, int, int, off_t);

static void init_real(void) {
    if (!real_mmap) {
        real_mmap = dlsym(RTLD_NEXT, "mmap64");
        if (!real_mmap) real_mmap = dlsym(RTLD_NEXT, "mmap");
    }
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    init_real();
    if (!real_mmap) {
        // last resort: plain mmap fallback should exist by now
        return MAP_FAILED;
    }
    void *ret = real_mmap(addr, length, prot, flags, fd, offset);
    if (ret == MAP_FAILED && errno == ENOMEM && length >= (1UL << 30)) {
        // Grande reserva anônima recusada pelo overcommit heurístico do
        // kernel (host sem swap). MAP_NORESERVE pula a checagem de commit
        // e é o modo correto para pools de endereço (ex.: 4GB do
        // SwiftShader/gfxstream) que tocam poucas páginas de fato.
        ret = real_mmap(addr, length, prot, flags | MAP_NORESERVE, fd, offset);
        if (ret != MAP_FAILED) {
            char buf[256];
            int n = snprintf(buf, sizeof buf,
                             "[MMAP-RESCUED] len=%zu (%.2f GB) flags=%#x via "
                             "MAP_NORESERVE\n",
                             length, length / 1073741824.0, flags);
            if (n > 0) {
                ssize_t m = write(2, buf, (size_t)n);
                (void)m;
            }
            return ret;
        }
    }
    if (ret == MAP_FAILED && errno == ENOMEM) {
        char buf[512];
        int n = snprintf(buf, sizeof buf,
                         "[MMAP-FAIL] len=%zu (%.2f GB) prot=%d flags=%#x fd=%d "
                         "errno=%d\n",
                         length, length / 1073741824.0, prot, flags, fd, errno);
        if (n > 0) {
            ssize_t m = write(2, buf, (size_t)n);
            (void)m;
        }
        // also dump proc status hint
        FILE *f = fopen("/proc/self/status", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof line, f)) {
                if (strncmp(line, "VmSize", 6) == 0 ||
                    strncmp(line, "VmRSS", 5) == 0 ||
                    strncmp(line, "VmData", 6) == 0 ||
                    strncmp(line, "Threads", 7) == 0) {
                    ssize_t m = write(2, "[MMAP-FAIL] ", 12);
                    (void)m;
                    m = write(2, line, strlen(line));
                    (void)m;
                }
            }
            fclose(f);
        }
    }
    return ret;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    return mmap64(addr, length, prot, flags, fd, offset);
}
