// LD_PRELOAD shim: (1) O_DIRECT desteklemeyen dosya sistemi simulasyonu, (2) YAVAS depolama simulasyonu.
//
//  (1) SHIM_ODIRECT=open_einval   -> open(..., O_DIRECT) dogrudan EINVAL
//      SHIM_ODIRECT=write_einval  -> open OK, O_DIRECT fd'ye write EINVAL
//
//  (2) Yalniz yolunda SHIM_TARGET alt-dizgisi (varsayilan "profiler_output_durable") gecen dosyanin fd'sine:
//      SHIM_WRITE_DELAY_US=N   -> her write()/pwrite() oncesi N mikrosaniye uyu
//      SHIM_SYNC_DELAY_US=N    -> her fdatasync()/fsync() oncesi N mikrosaniye uyu
//      SHIM_STALL_AT_MS=T SHIM_STALL_MS=D
//                              -> dosya acildiktan T ms sonraki ILK yazmada BIR KEZ D ms takil
//                                 (SD kartin garbage-collection takilmasini taklit eder)
//  Uyku = cekirdekte bloke olmak (gercek yavas cihazda thread D durumunda bekler, CPU yemez).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_target_fd = -1;
static long long g_t0_ns = 0;
static int g_stall_done = 0;

static long long mono_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static void sleep_us(long long us) {
    if (us <= 0) return;
    struct timespec ts = { us / 1000000LL, (us % 1000000LL) * 1000LL };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}
static void slow_hook(int fd, int is_sync) {
    if (fd < 0 || fd != g_target_fd) return;
    const char* d = getenv(is_sync ? "SHIM_SYNC_DELAY_US" : "SHIM_WRITE_DELAY_US");
    if (d) sleep_us(atoll(d));
    if (!is_sync) {
        const char* at = getenv("SHIM_STALL_AT_MS");
        const char* ms = getenv("SHIM_STALL_MS");
        if (at && ms && !g_stall_done && (mono_ns() - g_t0_ns) / 1000000LL >= atoll(at)) {
            if (__sync_bool_compare_and_swap(&g_stall_done, 0, 1)) sleep_us(atoll(ms) * 1000LL);
        }
    }
}

int open(const char* path, int flags, ...) {
    static int (*real_open)(const char*, int, ...) = 0;
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
    va_list ap; va_start(ap, flags); mode_t mode = va_arg(ap, mode_t); va_end(ap);
    const char* m = getenv("SHIM_ODIRECT");
    if ((flags & O_DIRECT) && m && strcmp(m, "open_einval") == 0) { errno = EINVAL; return -1; }
    int fd = real_open(path, flags, mode);
    if (fd >= 0) {
        const char* tgt = getenv("SHIM_TARGET");
        if (!tgt) tgt = "profiler_output_durable";
        if (strstr(path, tgt)) { g_target_fd = fd; g_t0_ns = mono_ns(); g_stall_done = 0; }
    }
    return fd;
}

ssize_t write(int fd, const void* buf, size_t n) {
    static ssize_t (*real_write)(int, const void*, size_t) = 0;
    if (!real_write) real_write = dlsym(RTLD_NEXT, "write");
    const char* m = getenv("SHIM_ODIRECT");
    if (m && strcmp(m, "write_einval") == 0) {
        int fl = fcntl(fd, F_GETFL);
        if (fl >= 0 && (fl & O_DIRECT)) { errno = EINVAL; return -1; }
    }
    slow_hook(fd, 0);
    return real_write(fd, buf, n);
}

ssize_t pwrite(int fd, const void* buf, size_t n, off_t off) {
    static ssize_t (*real_pwrite)(int, const void*, size_t, off_t) = 0;
    if (!real_pwrite) real_pwrite = dlsym(RTLD_NEXT, "pwrite");
    slow_hook(fd, 0);
    return real_pwrite(fd, buf, n, off);
}
ssize_t pwrite64(int fd, const void* buf, size_t n, off_t off) {
    static ssize_t (*real_pwrite64)(int, const void*, size_t, off_t) = 0;
    if (!real_pwrite64) real_pwrite64 = dlsym(RTLD_NEXT, "pwrite64");
    slow_hook(fd, 0);
    return real_pwrite64(fd, buf, n, off);
}
int fdatasync(int fd) {
    static int (*real_fdatasync)(int) = 0;
    if (!real_fdatasync) real_fdatasync = dlsym(RTLD_NEXT, "fdatasync");
    slow_hook(fd, 1);
    return real_fdatasync(fd);
}
int fsync(int fd) {
    static int (*real_fsync)(int) = 0;
    if (!real_fsync) real_fsync = dlsym(RTLD_NEXT, "fsync");
    slow_hook(fd, 1);
    return real_fsync(fd);
}
