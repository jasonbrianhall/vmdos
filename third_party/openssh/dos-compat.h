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
int dos_tcgetattr(int, struct termios *);
#define tcsetattr dos_tcsetattr
#define tcgetattr dos_tcgetattr
void dos_term_own_input(void);			/* dos-term.c */
void dos_term_set_completer(int (*)(char *, int *, int *, int));

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

/*
 * See dos-co.c: ssh runs inside sftp and scp as a coroutine, joined by
 * in-memory pipes; these follow it (and are plain calls in SSH.EXE).
 */
#include <stdlib.h>
#include <sys/wait.h>
#include <dirent.h>
#include "dos-co.h"
#include "dos-sock.h"
#define d_ino d_namlen	/* DJGPP's dirent has no inode: sftp only tests != 0 */
int dos_isatty(int);
void closefrom(int);
#define USE_PIPES 1
#include <sys/uio.h>
ssize_t dos_readv(int, const struct iovec *, int);
ssize_t dos_writev(int, const struct iovec *, int);
#define readv  dos_readv
#define writev dos_writev
#define alarm(s)       dos_alarm(s)
#define pipe(f)        dos_pipe(f)
#define close(f)       dos_close(f)
#define dup(f)         dos_dup(f)
#define dup2(f, t)     dos_dup2(f, t)
#define isatty(f)      dos_isatty(f)
#define exit(s)        dos_exit(s)
#define _exit(s)       dos__exit(s)
#define waitpid(p, s, o) dos_waitpid(p, s, o)
#define kill(p, s)     dos_kill(p, s)
#undef STDIN_FILENO
#undef STDOUT_FILENO
#define STDIN_FILENO   (dos_stdin_fd())
#define STDOUT_FILENO  (dos_stdout_fd())
pid_t dos_spawn(const char *, char **, int, int);	/* dos-spawn.c */

/*
 * See dos-compat.c: POSIX file type bits (sftp and scp compare the remote
 * side's with them; DJGPP's own differ: its S_IFREG is 0), converted at
 * stat(); and gmtime()/localtime() without DJGPP's broken tz code.
 */
#include <sys/stat.h>
struct tm;		/* not <time.h>: DJGPP's declares select() */
#undef S_IFMT
#undef S_IFREG
#undef S_IFDIR
#undef S_IFCHR
#undef S_IFBLK
#undef S_IFIFO
#undef S_IFLNK
#undef S_IFSOCK
#undef S_ISREG
#undef S_ISDIR
#undef S_ISCHR
#undef S_ISBLK
#undef S_ISFIFO
#undef S_ISLNK
#undef S_ISSOCK
#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
int dos_stat(const char *, struct stat *);
int dos_lstat(const char *, struct stat *);
int dos_fstat(int, struct stat *);
#define stat(p, b)  dos_stat(p, b)
#define lstat(p, b) dos_lstat(p, b)
#define fstat(f, b) dos_fstat(f, b)
struct tm *dos_gmtime(const time_t *);
struct tm *dos_gmtime_r(const time_t *, struct tm *);
#define gmtime(t)        dos_gmtime(t)
#define localtime(t)     dos_gmtime(t)
#define gmtime_r(t, r)   dos_gmtime_r(t, r)
#define localtime_r(t, r) dos_gmtime_r(t, r)

/*
 * See dos-compat.c: basename() as POSIX has it. DJGPP's returns what follows
 * the last / or \, so "dir/" gave "" and sftp's get -r / mget -r made the
 * local directory "" (mkdir: ENOENT): sftp's glob marks directories with /.
 */
char *dos_basename(const char *);
#define basename(p) dos_basename(p)

/* See dos-compat.c: ssh.c (patched) asks for the user name. */
char *dos_ask_user(const char *);

#endif /* __DJGPP__ */
#endif /* DOS_COMPAT_H */
