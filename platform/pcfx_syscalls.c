/* pcfx_syscalls.c — minimal newlib OS glue for the freestanding PC-FX target.
 *
 * newlib's reentrant stdio bottoms out in bare POSIX syscalls (write/read/...);
 * -lsim only supplies _sbrk (the heap). We provide the rest here: writes are
 * discarded (there is no host-visible output sink), everything else is a
 * benign stub. */
#include <sys/stat.h>
#include <errno.h>

int write(int fd, const char *buf, int len)
{
    (void)fd;
    (void)buf;
    return len;
}

int read(int fd, char *buf, int len)   { (void)fd; (void)buf; (void)len; return 0; }
int lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return 0; }
int close(int fd)                      { (void)fd; return 0; }
int isatty(int fd)                     { (void)fd; return 1; }
int getpid(void)                       { return 1; }
int kill(int pid, int sig)             { (void)pid; (void)sig; errno = EINVAL; return -1; }

int fstat(int fd, struct stat *st)
{
    (void)fd;
    st->st_mode = S_IFCHR;
    return 0;
}

/* _exit is provided by crt0.o. */
