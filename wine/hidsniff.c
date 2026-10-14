// hidsniff — LD_PRELOAD shim that logs every packet exchanged over /dev/hidraw*.
// Build: gcc -shared -fPIC -O2 -o hidsniff.so hidsniff.c -ldl
// Use:   HIDSNIFF_LOG=/path/log.txt LD_PRELOAD=/path/hidsniff.so wine Bloody8.exe
// Output lines: "<unix time> <pid> <op> fd=<n> len=<n> <hex>"
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_FD 4096
static unsigned char hidraw_fd[MAX_FD];

static void log_packet(const char *op, int fd, const unsigned char *buf, long len)
{
    static int log_fd = -2;
    char line[4096];
    struct timeval tv;
    int n;

    if (log_fd == -2)
    {
        const char *path = getenv("HIDSNIFF_LOG");
        log_fd = path ? open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644) : -1;
    }
    if (log_fd < 0 || len < 0) return;

    gettimeofday(&tv, NULL);
    n = snprintf(line, sizeof(line), "%ld.%06ld %d %s fd=%d len=%ld",
                 (long)tv.tv_sec, (long)tv.tv_usec, getpid(), op, fd, len);
    for (long i = 0; i < len && n < (int)sizeof(line) - 4; i++)
        n += snprintf(line + n, sizeof(line) - n, " %02x", buf[i]);
    line[n++] = '\n';
    write(log_fd, line, n);
}

static void track(int fd, const char *path)
{
    if (fd >= 0 && fd < MAX_FD) hidraw_fd[fd] = path && !strncmp(path, "/dev/hidraw", 11);
    if (fd >= 0 && fd < MAX_FD && hidraw_fd[fd]) log_packet(path, fd, NULL, 0);
}

static int is_hidraw(int fd) { return fd >= 0 && fd < MAX_FD && hidraw_fd[fd]; }

#define GET_MODE(flags) ({ mode_t m = 0; if ((flags) & (O_CREAT | O_TMPFILE)) { \
    va_list ap; va_start(ap, flags); m = va_arg(ap, mode_t); va_end(ap); } m; })

#define OPEN_WRAPPER(name)                                                        \
    int name(const char *path, int flags, ...)                                     \
    {                                                                              \
        static int (*real)(const char *, int, ...);                                \
        if (!real) real = dlsym(RTLD_NEXT, #name);                                 \
        int fd = real(path, flags, GET_MODE(flags));                               \
        track(fd, path);                                                           \
        return fd;                                                                 \
    }

#define OPENAT_WRAPPER(name)                                                      \
    int name(int dirfd, const char *path, int flags, ...)                          \
    {                                                                              \
        static int (*real)(int, const char *, int, ...);                           \
        if (!real) real = dlsym(RTLD_NEXT, #name);                                 \
        int fd = real(dirfd, path, flags, GET_MODE(flags));                        \
        track(fd, path);                                                           \
        return fd;                                                                 \
    }

OPEN_WRAPPER(open)
OPEN_WRAPPER(open64)
OPENAT_WRAPPER(openat)
OPENAT_WRAPPER(openat64)

int close(int fd)
{
    static int (*real)(int);
    if (!real) real = dlsym(RTLD_NEXT, "close");
    if (fd >= 0 && fd < MAX_FD) hidraw_fd[fd] = 0;
    return real(fd);
}

/* ---- guard: only allowlisted vendor commands reach the device ----------
 * HIDSNIFF_GUARD=0 disables it. HIDSNIFF_ALLOW overrides the allowlist:
 * comma-separated hex, "1f" matches opcode (byte 1), "0306" matches opcode+sub,
 * "21r" allows opcode 21 (raw SPI flash passthrough, byte 8 = SPI command) only for
 * read-type SPI commands: 9f JEDEC id, 05/35 status, 03/0b read data, 90/4b ids.
 * Blocked SET_FEATUREs report success; the next GET_FEATURE on that fd returns
 * the blocked packet echoed back (that is what the mouse itself does). */
#define DEFAULT_ALLOW "1f00,2d00,0500,2900,2a00,2f00,0e00,2000,030600,030601,030602"  /* getters only with sub=00; 21 resets the MCU */
#define MAX_ALLOW 64
static int allow_op[MAX_ALLOW], allow_sub[MAX_ALLOW], allow_count = -1, guard_on = 1;
#define SUB_SPI_READ (-2)
static unsigned char fake_reply[MAX_FD][64];
static unsigned char fake_pending[MAX_FD];

static void guard_init(void)
{
    const char *g = getenv("HIDSNIFF_GUARD"), *list = getenv("HIDSNIFF_ALLOW");
    char buf[512], *tok, *save;

    if (allow_count >= 0) return;
    allow_count = 0;
    guard_on = !(g && !strcmp(g, "0"));
    snprintf(buf, sizeof(buf), "%s", list ? list : DEFAULT_ALLOW);
    for (tok = strtok_r(buf, ",", &save); tok && allow_count < MAX_ALLOW; tok = strtok_r(NULL, ",", &save))
    {
        unsigned v = strtoul(tok, NULL, 16);
        if (tok[2] == 'r') { allow_op[allow_count] = v; allow_sub[allow_count] = SUB_SPI_READ; }
        else if (strlen(tok) > 4) { allow_op[allow_count] = v >> 16; allow_sub[allow_count] = 0x10000 | (v & 0xffff); }
        else if (strlen(tok) > 2) { allow_op[allow_count] = v >> 8; allow_sub[allow_count] = v & 0xff; }
        else { allow_op[allow_count] = v; allow_sub[allow_count] = -1; }
        allow_count++;
    }
}

static int allowed(const unsigned char *buf, unsigned len)
{
    guard_init();
    if (!guard_on) return 1;
    if (len < 3 || buf[0] != 0x07) return 0;
    for (int i = 0; i < allow_count; i++)
    {
        if (allow_op[i] != buf[1]) continue;
        if (allow_sub[i] == SUB_SPI_READ)
        {
            static const unsigned char spi_read[] = {0x9f, 0x05, 0x35, 0x03, 0x0b, 0x90, 0x4b};
            if (len < 9) return 0;
            for (unsigned j = 0; j < sizeof(spi_read); j++) if (buf[8] == spi_read[j]) return 1;
            return 0;
        }
        if (allow_sub[i] >= 0 && (allow_sub[i] & 0x10000))
        {
            if (len >= 4 && ((allow_sub[i] >> 8) & 0xff) == buf[2] && (allow_sub[i] & 0xff) == buf[3]) return 1;
            continue;
        }
        if (allow_sub[i] == -1 || allow_sub[i] == buf[2]) return 1;
    }
    return 0;
}

ssize_t read(int fd, void *buf, size_t count)
{
    static ssize_t (*real)(int, void *, size_t);
    if (!real) real = dlsym(RTLD_NEXT, "read");
    ssize_t ret = real(fd, buf, count);
    if (ret > 0 && is_hidraw(fd)) log_packet("IN", fd, buf, ret);
    return ret;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    static ssize_t (*real)(int, const void *, size_t);
    if (!real) real = dlsym(RTLD_NEXT, "write");
    if (is_hidraw(fd))
    {
        guard_init();
        if (guard_on)
        {
            log_packet("BLOCKED_OUT", fd, buf, count);
            return count;
        }
        log_packet("OUT", fd, buf, count);
    }
    return real(fd, buf, count);
}

int ioctl(int fd, unsigned long req, ...)
{
    static int (*real)(int, unsigned long, void *);
    va_list ap;
    void *arg;
    int ret;

    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (!real) real = dlsym(RTLD_NEXT, "ioctl");

    if (!is_hidraw(fd) || _IOC_TYPE(req) != 'H') return real(fd, req, arg);

    unsigned nr = _IOC_NR(req), size = _IOC_SIZE(req);
    if ((nr == 0x06 || nr == 0x09 || nr == 0x0b) && !allowed(arg, size))
    {
        log_packet(nr == 0x06 ? "BLOCKED_FEATURE" : nr == 0x09 ? "BLOCKED_INPUT" : "BLOCKED_OUTPUT", fd, arg, size);
        if (nr == 0x06 && fd < MAX_FD)
        {
            memset(fake_reply[fd], 0, sizeof(fake_reply[fd]));
            memcpy(fake_reply[fd], arg, size < 64 ? size : 64);
            fake_pending[fd] = 1;
        }
        return size;
    }
    if ((nr == 0x06 || nr == 0x09 || nr == 0x0b) && getenv("HIDSNIFF_DRYRUN"))
    {
        log_packet("WOULD_ALLOW", fd, arg, size);   /* rule test: nothing reaches the device */
        return size;
    }
    if (nr == 0x06) log_packet("SET_FEATURE", fd, arg, size);
    if (nr == 0x09) log_packet("SET_INPUT", fd, arg, size);
    if (nr == 0x0b) log_packet("SET_OUTPUT", fd, arg, size);
    if (nr == 0x06 && fd < MAX_FD) fake_pending[fd] = 0;
    if (nr == 0x07 && fd < MAX_FD && fake_pending[fd])
    {
        unsigned n = size < 64 ? size : 64;
        memcpy(arg, fake_reply[fd], n);
        fake_pending[fd] = 0;
        log_packet("FAKE_GET_FEATURE", fd, arg, n);
        return n;
    }
    ret = real(fd, req, arg);
    if (nr == 0x07) log_packet("GET_FEATURE", fd, arg, ret > 0 ? ret : 0);
    if (nr == 0x0a) log_packet("GET_INPUT", fd, arg, ret > 0 ? ret : 0);
    if (nr == 0x0c) log_packet("GET_OUTPUT", fd, arg, ret > 0 ? ret : 0);
    return ret;
}
