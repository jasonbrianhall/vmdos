/*
 * dos-compat.h: forced into every OpenSSH source file (-include) when
 * building ssh for DOS (DJGPP + Watt-32), for vmdos.
 */
#ifndef DOS_COMPAT_H
#define DOS_COMPAT_H

#ifdef __DJGPP__
#include <signal.h>
#include <errno.h>

/* Signals DOS doesn't have; ssh only installs handlers for them. */
#ifndef SIGCHLD
#define SIGCHLD   (305 + 1)
#endif
#ifndef SIGHUP
#define SIGHUP    (305 + 2)
#endif
#ifndef SIGWINCH
#define SIGWINCH  (305 + 3)
#endif
#ifndef SIGTSTP
#define SIGTSTP   (305 + 4)
#endif
#ifndef SIGTTIN
#define SIGTTIN   (305 + 5)
#endif
#ifndef SIGTTOU
#define SIGTTOU   (305 + 6)
#endif
#ifndef SIGINFO
#define SIGINFO   (305 + 7)
#endif
#ifndef SIGXCPU
#define SIGXCPU   (305 + 8)
#endif
#ifndef SIGXFSZ
#define SIGXFSZ   (305 + 9)
#endif
#ifndef SIGBUS
#define SIGBUS    (305 + 10)
#endif
#ifndef SIGSYS
#define SIGSYS    (305 + 11)
#endif
#ifndef SIGSTOP
#define SIGSTOP   (305 + 12)
#endif
#ifndef SIGCONT
#define SIGCONT   (305 + 13)
#endif

/* termios and stat bits DJGPP leaves out (defs.h would make S_IFSOCK 0,
   which is DJGPP's S_IFREG). */
#define IXANY     0
#define IP_TOS_IS_BROKEN 1	/* Watt-32 has no IP_TOS */
#define S_IFSOCK  0xc000
#define S_ISVTX   01000

/* No mmap in DOS: only lib_contains_symbol() asks, and fails cleanly. */
#include <sys/types.h>
#include <sys/mman.h>
#define MAP_PRIVATE 0x02
#define MAP_SHARED  0x01
#define MAP_FAILED  ((void *)-1)
#define mmap(a, l, p, f, fd, o) ((void)(fd), errno = ENOSYS, MAP_FAILED)
#define munmap(a, l)            ((void)(a), (void)(l), 0)

/* See dos-compat.c: Ctrl-C goes to the remote side in raw mode. */
#include <termios.h>
int dos_tcsetattr(int, int, const struct termios *);
#define tcsetattr dos_tcsetattr

/* See dos-term.c: the console is a terminal emulator. */
#include <unistd.h>
#include <sys/ioctl.h>
ssize_t dos_read(int, void *, size_t);
ssize_t dos_write(int, const void *, size_t);
int dos_ioctl(int, int, ...);
void dos_term_mode(const struct termios *);
int dos_term_readable(int);
int dos_term_con_out(int);
#define read  dos_read
#define write dos_write
#define ioctl dos_ioctl

/* See dos-compat.c: non-blocking sockets. */
#include <fcntl.h>
int dos_fcntl(int, int, ...);
#define fcntl dos_fcntl

/* See dos-compat.c: the clock without DJGPP's mktime(). */
struct timeval;
struct timezone;
int dos_gettimeofday(struct timeval *, struct timezone *);
time_t dos_time(time_t *);
#define gettimeofday dos_gettimeofday
#define time(p) dos_time(p)

/* See dos-compat.c: messages for the network errnos. */
#include <string.h>
#include <sys/werrno.h>	/* Watt-32: it #undefs strerror */
char *dos_strerror(int);
#define strerror dos_strerror

/* See dos-compat.c: poll that knows sockets, the console and files. */
#include <poll.h>
#define poll  dos_poll
#define ppoll dos_ppoll
struct timespec;
int dos_ppoll(struct pollfd *, unsigned int, const struct timespec *,
    const sigset_t *);
#ifndef POLLRDNORM
#define POLLRDNORM POLLIN
#endif
#ifndef POLLWRNORM
#define POLLWRNORM POLLOUT
#endif

#endif /* __DJGPP__ */
#endif /* DOS_COMPAT_H */
