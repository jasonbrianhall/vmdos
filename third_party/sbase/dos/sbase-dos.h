/* sbase-dos.h -- included first in every sbase file when building for DOS
   with DJGPP (-include).  What DJGPP lacks of POSIX 2008, as sbase uses it:
   the *at() calls, directories opened as file descriptors, nanosecond file
   times, getline, and fork() for the commands find -exec, xargs, time, env
   and nohup run.  See sbase-dos.c. */
#ifndef SBASE_DOS_H
#define SBASE_DOS_H

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <setjmp.h>

struct timespec { time_t tv_sec; long tv_nsec; };

#define AT_FDCWD		(-100)
#define AT_SYMLINK_NOFOLLOW	0x100
#define AT_SYMLINK_FOLLOW	0x400
#define AT_REMOVEDIR		0x200
#define AT_EACCESS		0x800
#define O_DIRECTORY		0
#define O_CLOEXEC		0
#ifndef O_NOFOLLOW
#define O_NOFOLLOW		0
#endif
#define S_ISVTX			0
#define UTIME_NOW		((1l << 30) - 1l)
#define UTIME_OMIT		((1l << 30) - 2l)
#define CLOCK_REALTIME		0
#define CLOCK_MONOTONIC		1
typedef int clockid_t;
#ifndef S_ISSOCK
#define S_ISSOCK(m)		0
#endif
#define _SC_LOGIN_NAME_MAX	64	/* ls: a buffer size there */
#define _POSIX_HOST_NAME_MAX	255

/* nanosecond times: DOS has 2 seconds */
#define DOS_TS(t)	((struct timespec){ (t), 0 })

/* directories as file descriptors */
#define open		dos_open
#define close		dos_close
#define opendir		dos_opendir
#define closedir	dos_closedir
int dos_open (const char *, int, ...);
int dos_close (int);
DIR *dos_opendir (const char *);
int dos_closedir (DIR *);
DIR *fdopendir (int);
int dirfd (DIR *);
int openat (int, const char *, int, ...);
int fstatat (int, const char *, struct stat *, int);
int faccessat (int, const char *, int, int);
int unlinkat (int, const char *, int);
int mkdirat (int, const char *, mode_t);
int fchmodat (int, const char *, mode_t, int);
int fchownat (int, const char *, uid_t, gid_t, int);
int linkat (int, const char *, int, const char *, int);
int symlinkat (const char *, int, const char *);
ssize_t readlinkat (int, const char *, char *, size_t);
int renameat (int, const char *, int, const char *);
int utimensat (int, const char *, const struct timespec[2], int);
int futimens (int, const struct timespec[2]);
int clock_gettime (clockid_t, struct timespec *);
int nanosleep (const struct timespec *, struct timespec *);

int clock_settime (clockid_t, const struct timespec *);
FILE *fmemopen (void *, size_t, const char *);
char *strptime (const char *, const char *, struct tm *);
void *tsearch (const void *, void **, int (*) (const void *, const void *));
ssize_t getdelim (char **, size_t *, int, FILE *);
ssize_t getline (char **, size_t *, FILE *);
char *strsep (char **, const char *);
char *mkdtemp (char *);

/* fork(), only as the commands here use it: the child runs a program
   (execvp) or gives up (_exit).  The program runs at once (spawn) and the
   parent goes on with its exit status for waitpid. */
extern jmp_buf dos_fork_jb;
pid_t dos_fork_child (void);
pid_t dos_fork_parent (void);
#define fork()		(setjmp (dos_fork_jb) ? dos_fork_parent () : dos_fork_child ())
#define execvp		dos_execvp
#define _exit		dos__exit
#define waitpid		dos_waitpid
int dos_execvp (const char *, char *const[]);
void dos__exit (int) __attribute__ ((noreturn));
pid_t dos_waitpid (pid_t, int *, int);

#endif
