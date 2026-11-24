/* sbase-dos.c -- what sbase needs and DJGPP lacks (see sbase-dos.h).

   Directories as file descriptors: DOS can't open a directory, and sbase
   walks trees with openat/fstatat/fdopendir.  open() of a directory gives a
   descriptor of our own (DIRFD0 and up) that stands for its path, and the
   *at() calls put the path and the name together.

   fork(): find -exec, xargs and time fork and exec at once; the fork()
   macro remembers where it was (setjmp), execvp in the "child" runs the
   program with spawnvp and returns to the fork as the parent with the
   program's pid, and waitpid hands back its status.  env, nice and nohup
   exec without forking: the program runs, and we exit with its status.

   Files are binary; the console (stdin, stdout, stderr when they are the
   screen and keyboard) stays text, so lines end where they should. */
#include "sbase-dos.h"
#undef open
#undef close
#undef opendir
#undef closedir
#undef execvp
#undef _exit
#undef waitpid

#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <io.h>
#include <process.h>
#include <utime.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <dos.h>

int _fmode = O_BINARY;

static void __attribute__ ((constructor))
dos_modes (void)
{
  int fd;
  for (fd = 0; fd < 3; fd++)
    setmode (fd, isatty (fd) ? O_TEXT : O_BINARY);
}

/* ------------------------------------------------------------------ */
/* Directories as descriptors                                           */

#define DIRFD0  0x4000
#define NDIRFD  64
static char *dirfds[NDIRFD];			/* path, or 0 */
static struct { DIR *dp; int fd; int owns; } dirs[NDIRFD];

static int
dirfd_new (const char *path)
{
  int i;
  for (i = 0; i < NDIRFD; i++)
    if (!dirfds[i])
      {
	if (!(dirfds[i] = strdup (path)))
	  {
	    errno = ENOMEM;
	    return -1;
	  }
	return DIRFD0 + i;
      }
  errno = EMFILE;
  return -1;
}

static const char *
dirfd_path (int fd)
{
  if (fd >= DIRFD0 && fd < DIRFD0 + NDIRFD)
    return dirfds[fd - DIRFD0];
  return 0;
}

static int
absolute (const char *p)
{
  return p[0] == '/' || p[0] == '\\' || (isalpha ((unsigned char)p[0]) && p[1] == ':');
}

/* dfd's directory and name, in buf (PATH_MAX); 0 if dfd is no directory */
static const char *
at_path (int dfd, const char *name, char *buf)
{
  const char *d;
  size_t n;
  if (dfd == AT_FDCWD || absolute (name))
    return name;
  if (!(d = dirfd_path (dfd)))
    {
      errno = EBADF;
      return 0;
    }
  n = strlen (d);
  if (n + strlen (name) + 2 > PATH_MAX)
    {
      errno = ENAMETOOLONG;
      return 0;
    }
  strcpy (buf, d);
  if (n && d[n - 1] != '/' && d[n - 1] != '\\' && d[n - 1] != ':')
    strcat (buf, "/");
  strcat (buf, name);
  return buf;
}

int
dos_open (const char *path, int flags, ...)
{
  struct stat st;
  int mode = 0;
  va_list ap;
  va_start (ap, flags);
  if (flags & O_CREAT)
    mode = va_arg (ap, int);
  va_end (ap);
  if (stat (path, &st) == 0 && S_ISDIR (st.st_mode))
    {
      if ((flags & O_ACCMODE) != O_RDONLY)
	{
	  errno = EISDIR;
	  return -1;
	}
      return dirfd_new (path);
    }
  return open (path, flags, mode);
}

int
openat (int dfd, const char *name, int flags, ...)
{
  char buf[PATH_MAX];
  const char *p = at_path (dfd, name, buf);
  int mode = 0;
  va_list ap;
  va_start (ap, flags);
  if (flags & O_CREAT)
    mode = va_arg (ap, int);
  va_end (ap);
  return p ? dos_open (p, flags, mode) : -1;
}

int
dos_close (int fd)
{
  if (dirfd_path (fd))
    {
      free (dirfds[fd - DIRFD0]);
      dirfds[fd - DIRFD0] = 0;
      return 0;
    }
  return close (fd);
}

static int
dir_note (DIR *dp, int fd, int owns)
{
  int i;
  for (i = 0; i < NDIRFD; i++)
    if (!dirs[i].dp)
      {
	dirs[i].dp = dp, dirs[i].fd = fd, dirs[i].owns = owns;
	return 0;
      }
  return -1;
}

DIR *
dos_opendir (const char *path)
{
  DIR *dp = opendir (path);
  int fd;
  if (dp && (fd = dirfd_new (path)) >= 0 && dir_note (dp, fd, 1) < 0)
    dos_close (fd);
  return dp;
}

DIR *
fdopendir (int fd)
{
  const char *p = dirfd_path (fd);
  DIR *dp;
  if (!p)
    {
      errno = ENOTDIR;
      return 0;
    }
  if ((dp = opendir (p)))
    dir_note (dp, fd, 1);
  return dp;
}

int
dirfd (DIR *dp)
{
  int i;
  for (i = 0; i < NDIRFD; i++)
    if (dirs[i].dp == dp)
      return dirs[i].fd;
  errno = EINVAL;
  return -1;
}

int
dos_closedir (DIR *dp)
{
  int i;
  for (i = 0; i < NDIRFD; i++)
    if (dirs[i].dp == dp)
      {
	if (dirs[i].owns)
	  dos_close (dirs[i].fd);
	dirs[i].dp = 0;
      }
  return closedir (dp);
}

#define AT1(call)				\
  char buf[PATH_MAX];				\
  const char *p = at_path (dfd, name, buf);	\
  if (!p)					\
    return -1;					\
  return call

int fstatat (int dfd, const char *name, struct stat *st, int flags)
{ AT1 ((flags & AT_SYMLINK_NOFOLLOW) ? lstat (p, st) : stat (p, st)); }

int faccessat (int dfd, const char *name, int mode, int flags)
{ (void)flags; AT1 (access (p, mode)); }

int unlinkat (int dfd, const char *name, int flags)
{ AT1 ((flags & AT_REMOVEDIR) ? rmdir (p) : unlink (p)); }

int mkdirat (int dfd, const char *name, mode_t mode)
{ AT1 (mkdir (p, mode)); }

int fchmodat (int dfd, const char *name, mode_t mode, int flags)
{ (void)flags; AT1 (chmod (p, mode)); }

int fchownat (int dfd, const char *name, uid_t u, gid_t g, int flags)
{ (void)u, (void)g, (void)flags; AT1 (access (p, F_OK)); }

ssize_t readlinkat (int dfd, const char *name, char *b, size_t n)
{ AT1 (readlink (p, b, n)); }

int
symlinkat (const char *target, int dfd, const char *name)
{ AT1 (symlink (target, p)); }

int
linkat (int ofd, const char *oname, int dfd, const char *name, int flags)
{
  char obuf[PATH_MAX];
  const char *op = at_path (ofd, oname, obuf);
  (void)flags;
  if (!op)
    return -1;
  { AT1 (link (op, p)); }
}

int
renameat (int ofd, const char *oname, int dfd, const char *name)
{
  char obuf[PATH_MAX];
  const char *op = at_path (ofd, oname, obuf);
  if (!op)
    return -1;
  { AT1 (rename (op, p)); }
}

/* utime(), or DOS's own call (INT 21h 5701h: DJGPP's utime goes through
   an LFN call DOSLFN may not have).  DOS keeps no time for a directory:
   leave it. */
static int
set_times (const char *p, struct stat *st, struct utimbuf *u)
{
  struct tm *tm;
  int fd, r;
  if (S_ISDIR (st->st_mode))
    return 0;
  if (utime (p, u) == 0)
    return 0;
  if ((fd = open (p, O_RDONLY)) < 0)
    return -1;
  tm = localtime (&u->modtime);
  r = tm ? _dos_setftime (fd, ((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday,
			  (tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2)) : 1;
  close (fd);
  if (r)
    {
      errno = EIO;
      return -1;
    }
  return 0;
}

int
utimensat (int dfd, const char *name, const struct timespec ts[2], int flags)
{
  char buf[PATH_MAX];
  const char *p = at_path (dfd, name, buf);
  struct utimbuf u;
  struct stat st;
  time_t now = time (0);
  (void)flags;
  if (!p)
    return -1;
  if (stat (p, &st) < 0)
    return -1;
  if (!ts)
    {
      u.actime = u.modtime = now;
      return set_times (p, &st, &u);
    }
  if (ts[0].tv_nsec == UTIME_OMIT && ts[1].tv_nsec == UTIME_OMIT)
    return 0;
  u.actime = ts[0].tv_nsec == UTIME_OMIT ? st.st_atime
	     : ts[0].tv_nsec == UTIME_NOW ? now : ts[0].tv_sec;
  u.modtime = ts[1].tv_nsec == UTIME_OMIT ? st.st_mtime
	      : ts[1].tv_nsec == UTIME_NOW ? now : ts[1].tv_sec;
  return set_times (p, &st, &u);
}

int
futimens (int fd, const struct timespec ts[2])
{
  (void)fd, (void)ts;
  return 0;
}

int
clock_gettime (clockid_t c, struct timespec *ts)
{
  struct timeval tv;
  (void)c;
  gettimeofday (&tv, 0);
  ts->tv_sec = tv.tv_sec;
  ts->tv_nsec = tv.tv_usec * 1000l;
  return 0;
}

int
nanosleep (const struct timespec *req, struct timespec *rem)
{
  if (req->tv_sec)
    sleep (req->tv_sec);
  if (req->tv_nsec >= 1000)
    usleep (req->tv_nsec / 1000);
  if (rem)
    rem->tv_sec = 0, rem->tv_nsec = 0;
  return 0;
}

/* ------------------------------------------------------------------ */

ssize_t
getdelim (char **line, size_t *size, int delim, FILE *fp)
{
  size_t n = 0;
  int c;
  if (!line || !size)
    {
      errno = EINVAL;
      return -1;
    }
  if (!*line || *size == 0)
    {
      *size = 128;
      if (!(*line = malloc (*size)))
	return -1;
    }
  while ((c = getc (fp)) != EOF)
    {
      if (n + 2 > *size)
	{
	  char *t = realloc (*line, *size * 2);
	  if (!t)
	    return -1;
	  *line = t;
	  *size *= 2;
	}
      (*line)[n++] = c;
      if (c == delim)
	break;
    }
  (*line)[n] = 0;
  return n ? (ssize_t)n : -1;
}

ssize_t
getline (char **line, size_t *size, FILE *fp)
{
  return getdelim (line, size, '\n', fp);
}

char *
strsep (char **sp, const char *delim)
{
  char *s = *sp, *e;
  if (!s)
    return 0;
  e = s + strcspn (s, delim);
  if (*e)
    *e++ = 0;
  else
    e = 0;
  *sp = e;
  return s;
}

char *
mkdtemp (char *tmpl)
{
  if (!mktemp (tmpl) || !*tmpl || mkdir (tmpl, 0700) < 0)
    return 0;
  return tmpl;
}

/* ------------------------------------------------------------------ */
/* fork, for fork-and-exec                                              */

jmp_buf dos_fork_jb;
static int in_child;
static pid_t lastpid = 1000;
static struct { pid_t pid; int status; } done[32];
static int ndone;

pid_t
dos_fork_child (void)
{
  fflush (NULL);
  in_child = 1;
  ++lastpid;
  return 0;
}

pid_t
dos_fork_parent (void)
{
  return lastpid;
}

static void __attribute__ ((noreturn))
child_done (int status)
{
  in_child = 0;
  fflush (NULL);
  if (ndone < 32)
    {
      done[ndone].pid = lastpid;
      done[ndone].status = status & 0xff;
      ndone++;
    }
  longjmp (dos_fork_jb, 1);
}

int
dos_execvp (const char *file, char *const argv[])
{
  int r;
  fflush (NULL);
  r = spawnvp (P_WAIT, file, argv);
  if (r == -1)
    return -1;
  if (in_child)
    child_done (r);
  exit (r);
}

void
dos__exit (int status)
{
  if (in_child)
    child_done (status);
  _exit (status);
}

pid_t
dos_waitpid (pid_t pid, int *status, int options)
{
  int i;
  (void)options;
  for (i = 0; i < ndone; i++)
    if (pid <= 0 || done[i].pid == pid)
      {
	pid_t p = done[i].pid;
	if (status)
	  *status = done[i].status;
	done[i] = done[--ndone];
	return p;
      }
  errno = ECHILD;
  return -1;
}

/* ------------------------------------------------------------------ */

int
clock_settime (clockid_t c, const struct timespec *ts)
{
  (void)c, (void)ts;
  errno = EPERM;
  return -1;
}

/* Reading from memory (grep's and sed's patterns): a temporary file. */
FILE *
fmemopen (void *buf, size_t size, const char *mode)
{
  FILE *fp;
  size_t n;
  if (strchr (mode, 'w') || strchr (mode, 'a') || strchr (mode, '+'))
    {
      errno = EINVAL;
      return 0;
    }
  if (!(fp = tmpfile ()))
    return 0;
  n = strnlen (buf, size);	/* up to the first NUL, as reading would stop */
  if (n == size || ((char *)buf)[n] != 0)
    n = size;
  if (fwrite (buf, 1, n, fp) != n)
    {
      fclose (fp);
      return 0;
    }
  rewind (fp);
  return fp;
}

/* strptime, for touch -d/-t: %Y %y %m %d %H %M %S %T %D %e %j %%,
   spaces, and literal characters. */
static const char *
num (const char *s, int min, int max, int digits, int *out)
{
  int v = 0, n = 0;
  while (n < digits && isdigit ((unsigned char)*s))
    v = v * 10 + (*s++ - '0'), n++;
  if (n == 0 || v < min || v > max)
    return 0;
  *out = v;
  return s;
}

char *
strptime (const char *s, const char *f, struct tm *tm)
{
  int v;
  for (; *f && s; f++)
    {
      if (isspace ((unsigned char)*f))
	{
	  while (isspace ((unsigned char)*s))
	    s++;
	  continue;
	}
      if (*f != '%')
	{
	  if (*s++ != *f)
	    return 0;
	  continue;
	}
      switch (*++f)
	{
	case 'Y': if ((s = num (s, 0, 9999, 4, &v))) tm->tm_year = v - 1900; break;
	case 'y': if ((s = num (s, 0, 99, 2, &v))) tm->tm_year = v < 69 ? v + 100 : v; break;
	case 'm': if ((s = num (s, 1, 12, 2, &v))) tm->tm_mon = v - 1; break;
	case 'd': case 'e':
	  while (*s == ' ')
	    s++;
	  if ((s = num (s, 1, 31, 2, &v)))
	    tm->tm_mday = v;
	  break;
	case 'H': if ((s = num (s, 0, 23, 2, &v))) tm->tm_hour = v; break;
	case 'M': if ((s = num (s, 0, 59, 2, &v))) tm->tm_min = v; break;
	case 'S': if ((s = num (s, 0, 61, 2, &v))) tm->tm_sec = v; break;
	case 'j': if ((s = num (s, 1, 366, 3, &v))) tm->tm_yday = v - 1; break;
	case 'T': s = strptime (s, "%H:%M:%S", tm); break;
	case 'D': s = strptime (s, "%m/%d/%y", tm); break;
	case '%': if (*s++ != '%') return 0; break;
	default: return 0;
	}
    }
  return (char *)s;
}

/* tsearch, for du (which files it has counted): a plain binary tree */
struct tnode { const void *key; struct tnode *l, *r; };

void *
tsearch (const void *key, void **rootp, int (*cmp) (const void *, const void *))
{
  struct tnode **t = (struct tnode **)rootp;
  if (!rootp)
    return 0;
  while (*t)
    {
      int c = cmp (key, (*t)->key);
      if (c == 0)
	return *t;
      t = c < 0 ? &(*t)->l : &(*t)->r;
    }
  if (!(*t = malloc (sizeof **t)))
    return 0;
  (*t)->key = key;
  (*t)->l = (*t)->r = 0;
  return *t;
}
