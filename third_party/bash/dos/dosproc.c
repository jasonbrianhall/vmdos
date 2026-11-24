/* dosproc.c -- processes for bash on DOS (DJGPP).

   DOS runs one program at a time and has no fork().  bash needs fork() for
   every subshell, pipeline, command substitution and external command.
   This file gives it one, the way a single-tasking machine can:

   fork()   saves the whole of the shell's memory (its data, heap and the
	    live part of its stack) in a snapshot outside the program, and
	    returns 0: the shell carries on as the child.  When the child
	    ends (exit, _exit, or execve of a program, which runs the
	    program and then ends), the snapshot is copied back, the file
	    handles and current directory the parent had are put back, and
	    fork() returns again, in the parent, with the child's pid.  The
	    child has always finished by then, so waitpid() just hands back
	    the status it left.

   pipe()   is a temporary file.  The writer always runs to the end before
	    the reader starts, so a file holds everything that passes.
	    (So does a command that never ends: `yes | head' doesn't.)

   execve() runs the program with spawnve() and ends the child with its
	    exit status.  #! scripts for sh or bash are run by the shell
	    itself (ENOEXEC, as bash expects of a kernel without #!); other
	    interpreters are looked up in PATH.  PATH is kept with `:'
	    between entries inside the shell (C:/FDOS/BIN:C:/VMDOS) and
	    handed to programs the DOS way (C:\FDOS\BIN;C:\VMDOS).

   Ctrl-C   DOS would end the shell when it sees Ctrl-C.  The shell's own
	    INT 23h handler ignores it while the shell itself runs (and
	    hands it to DOS, to end the program, while a program the shell
	    started runs).  readline reads keys through the BIOS
	    (dos_rl_getkey), and bash looks in the BIOS keyboard buffer
	    wherever it checks for interrupts (dos_check_break), so Ctrl-C
	    at the prompt, in a loop, or in a program does what it does on
	    Unix.

   Everything here goes through the linker's --wrap (fork, pipe, waitpid,
   wait, kill, getpid, getppid, execve, exit, _exit, close, dup, dup2,
   fcntl, signal, sigaction, sigprocmask), so bash's sources call them
   unchanged.  The few changes bash's own sources need are in
   bash-5.3-djgpp.patch.

   DOSFORK_DEBUG=1 in the environment shows how much each fork saves.  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <unistd.h>
#include <process.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <crt0.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/movedata.h>
#include <dir.h>
#include <sys/farptr.h>

/* The shell's own settings for the DJGPP start-up code: don't glob or
   read @response files from the command line (bash does its own), and a
   stack big enough for deep recursion in functions and the parser. */
int _crt0_startup_flags = _CRT0_FLAG_DISALLOW_RESPONSE_FILES;
unsigned _stklen = 2 * 1024 * 1024;
char **__crt0_glob_function (char *arg) { (void)arg; return 0; }

extern char djgpp_first_ctor[] __asm__ ("djgpp_first_ctor");	/* start of .data */
extern unsigned __djgpp_stack_limit;

struct dos_regs { unsigned ebx, esi, edi, ebp, esp, eip; };
extern int dos_fork_save (struct dos_regs *) __attribute__ ((returns_twice));
extern void dos_resume (struct dos_regs *, int, int, void *) __attribute__ ((noreturn));

/* The real functions, under --wrap. */
extern pid_t __real_fork (void);
extern int __real_close (int);
extern int __real_dup (int);
extern int __real_dup2 (int, int);
extern int __real_fcntl (int, int, ...);
extern int __real_execve (const char *, char *const [], char *const []);
extern void __real_exit (int) __attribute__ ((noreturn));
extern void __real__exit (int) __attribute__ ((noreturn));
extern pid_t __real_getpid (void);
extern void (*__real_signal (int, void (*)(int))) (int);
extern int __real_sigaction (int, const struct sigaction *, struct sigaction *);

void __wrap__exit (int) __attribute__ ((noreturn));

#define MAXFD     255		/* handles: the most DOS gives a program */
#define SAVEFD    100		/* the parents' handles are kept at 100 and up */
#define MAXDEPTH  24		/* subshells inside subshells */
#define MAXDONE   64		/* finished children not yet waited for */
#define MAXPIPES  64

/* One per level of fork(): what the parent had. */
struct level
{
  struct dos_regs regs;		/* where fork() returns in the parent */
  pid_t pid, ppid;
  int sel;			/* the snapshot: a selector for it, */
  unsigned long handle;		/* its DPMI block */
  int nblocks;			/* the parent's sbrk blocks */
  unsigned char saved[MAXFD + 1];	/* fd -> its copy (0: wasn't open) */
  char cwd[FILENAME_MAX];
};

static struct level lv[MAXDEPTH];
static int depth;			/* 0: the real process */
static pid_t cur_pid, next_pid;
static struct { pid_t pid; int status; } done[MAXDONE];
static int ndone;
static unsigned char reserved[MAXFD + 1];	/* parents' copies: hands off */

/* Pipes: a temporary file, and which fds are which end. */
static struct pipe { char name[FILENAME_MAX + 32]; int depth, used; } pipes[MAXPIPES];
static struct { unsigned char pipe, wr; } fdp[MAXFD + 1];	/* pipe: index+1 */

static void pipe_drop (int fd);
static void pipe_refresh (int p);

/* ------------------------------------------------------------------ */
/* Start-up: PATH the Unix way, room for handles.                       */

/* In a PATH with `:' between entries, is the `:' at s[i] the one in a
   drive letter (C:/DOS) rather than a separator?  `start' is where the
   current entry began. */
int
dos_path_drive_colon (const char *s, int start, int i)
{
  return i == start + 1 && isalpha ((unsigned char)s[start])
	 && (s[i + 1] == '/' || s[i + 1] == '\\');
}

/* C:\FDOS\BIN;C:\VMDOS -> C:/FDOS/BIN:C:/VMDOS */
static char *
path_to_unix (const char *v)
{
  char *r = malloc (strlen (v) + 1), *q = r;
  if (r == 0)
    return 0;
  for (; *v; v++)
    *q++ = *v == ';' ? ':' : *v == '\\' ? '/' : *v;
  *q = 0;
  return r;
}

/* C:/FDOS/BIN:C:/VMDOS -> C:\FDOS\BIN;C:\VMDOS */
static char *
path_to_dos (const char *v)
{
  char *r = malloc (strlen (v) + 1), *q = r;
  int i, start = 0;
  if (r == 0)
    return 0;
  for (i = 0; v[i]; i++)
    {
      if (v[i] == ':' && !dos_path_drive_colon (v, start, i))
	{
	  *q++ = ';';
	  start = i + 1;
	}
      else
	*q++ = v[i] == '/' ? '\\' : v[i];
    }
  *q = 0;
  return r;
}

/* DOS ends the running program when it sees Ctrl-C (INT 23h).  Our
   INT 23h: if the running program is the shell, carry on (IRET), and the
   shell sees Ctrl-C itself (dos_rl_getkey, dos_check_break); if it's a
   program the shell started, do what DOS does (end it). */
#define INT23_FLAG 26
static int int23_seg;

/* Did Ctrl-C end the program just run?  (and forget it) */
static int
int23_fired (void)
{
  unsigned char f = 0;
  if (int23_seg)
    {
      dosmemget (int23_seg * 16 + INT23_FLAG, 1, &f);
      if (f)
	_farpokeb (_dos_ds, int23_seg * 16 + INT23_FLAG, 0);
    }
  return f;
}

static void
int23_install (void)
{
  static const unsigned char code[] = {
    0x50,			/* push ax */
    0x53,			/* push bx */
    0xB4, 0x62,			/* mov ah,62h */
    0xCD, 0x21,			/* int 21h: bx = running program's PSP */
    0x81, 0xFB, 0, 0,		/* cmp bx,<the shell's PSP> */
    0x5B,			/* pop bx */
    0x58,			/* pop ax */
    0x75, 0x01,			/* jne +1 */
    0xCF,			/* iret */
    0x2E, 0xC6, 0x06, INT23_FLAG, 0, 1,	/* mov byte cs:[flag],1 */
    0xEA, 0, 0, 0, 0,		/* jmp far <DOS's INT 23h> */
    0				/* flag: a program was ended by Ctrl-C */
  };
  unsigned char c[sizeof code];
  unsigned psp = _go32_info_block.linear_address_of_original_psp >> 4;
  __dpmi_raddr dos, ours;
  int sel, seg = __dpmi_allocate_dos_memory ((sizeof code + 15) / 16, &sel);

  if (seg == -1)
    return;
  __dpmi_get_real_mode_interrupt_vector (0x23, &dos);
  memcpy (c, code, sizeof c);
  c[8] = psp & 0xff, c[9] = psp >> 8;
  c[22] = dos.offset16 & 0xff, c[23] = dos.offset16 >> 8;
  c[24] = dos.segment & 0xff, c[25] = dos.segment >> 8;
  dosmemput (c, sizeof c, seg * 16);
  ours.segment = seg;
  ours.offset16 = 0;
  __dpmi_set_real_mode_interrupt_vector (0x23, &ours);
  int23_seg = seg;
}

static void child_sig (int);

static void __attribute__ ((constructor))
dos_init (void)
{
  __dpmi_regs r;

  /* Up to 255 open handles (DOS gives 20), so there is room for the
     parents' copies at SAVEFD and up. */
  memset (&r, 0, sizeof r);
  r.h.ah = 0x67;
  r.x.bx = MAXFD;
  __dpmi_int (0x21, &r);

  /* In place in environ: main() gets the same array as envp, and a new
     one from setenv() wouldn't be it. */
  {
    extern char **environ;
    int i;
    for (i = 0; environ && environ[i]; i++)
      if (strncmp (environ[i], "PATH=", 5) == 0
	  && (strchr (environ[i], ';') || strchr (environ[i], '\\')))
	{
	  char *u = path_to_unix (environ[i]);
	  if (u)
	    environ[i] = u;
	}
  }
  /* The Unix tools (third_party/sbase) are in X:\BIN, X being bash's own
     drive: first in the shell's PATH, ahead of DOS's FIND and SORT. */
  {
    extern char **environ;
    char bin[8] = "C:/BIN";
    struct stat st;
    int i;
    if (__dos_argv0 && isalpha ((unsigned char)__dos_argv0[0]) && __dos_argv0[1] == ':')
      bin[0] = toupper ((unsigned char)__dos_argv0[0]);
    else
      bin[0] = 'A' + getdisk ();
    for (i = 0; environ && environ[i]; i++)
      if (strncmp (environ[i], "PATH=", 5) == 0)
	break;
    if (stat (bin, &st) == 0 && S_ISDIR (st.st_mode))
      {
	const char *old = environ && environ[i] ? environ[i] + 5 : "";
	char *n = malloc (strlen (old) + 16), *q;
	int j, start = 0, len = strlen (old);
	if (n)
	  {
	    /* PATH=X:/BIN, then the rest without X:/BIN */
	    q = n + sprintf (n, "PATH=%s", bin);
	    for (j = 0; j <= len; j++)
	      if (old[j] == 0 || (old[j] == ':' && !dos_path_drive_colon (old, start, j)))
		{
		  int k = j - start;
		  if (k > 0 && !((k == 6 || (k == 7 && (old[start + 6] == '/' || old[start + 6] == '\\')))
				 && strncasecmp (old + start, bin, 6) == 0))
		    {
		      *q++ = ':';
		      memcpy (q, old + start, k);
		      q += k;
		    }
		  start = j + 1;
		}
	    *q = 0;
	    if (environ && environ[i])
	      environ[i] = n;
	    else
	      putenv (n);
	  }
      }
  }

  if (getenv ("HOME") == 0)
    {
      char h[4] = "C:/";
      h[0] = 'A' + getdisk ();
      setenv ("HOME", h, 1);
    }
  cur_pid = __real_getpid ();
  next_pid = cur_pid + 1;

  int23_install ();
  /* The default action for Ctrl-C: end quietly, as on Unix (DJGPP's
     prints a register dump).  The shell is told it's SIG_DFL. */
  __real_signal (SIGINT, child_sig);
}

/* ------------------------------------------------------------------ */
/* Finding programs: DOS ones end in .EXE, .COM or .BAT.               */

static const char *const exts[] = { ".exe", ".com", ".bat", ".btm", 0 };

static int
has_ext (const char *path)
{
  const char *b = strrchr (path, '/'), *b2 = strrchr (path, '\\'), *d;
  if (b2 > b)
    b = b2;
  d = strrchr (b ? b : path, '.');
  return d != 0 && d[1] != 0;
}

/* The program PATH names, with the extension DOS needs, or 0. */
char *
dos_exec_ext (const char *path)
{
  int i;
  size_t n = strlen (path);
  char *r;

  if (n == 0 || path[n - 1] == '/' || has_ext (path))
    return 0;
  r = malloc (n + 5);
  if (r == 0)
    return 0;
  for (i = 0; exts[i]; i++)
    {
      struct stat st;
      strcpy (r, path);
      strcat (r, exts[i]);
      if (stat (r, &st) == 0 && !S_ISDIR (st.st_mode))
	return r;
    }
  free (r);
  return 0;
}

/* NAME in the directories of PATH (Unix form), with an extension. */
static char *
search_path (const char *name, const char *path)
{
  char buf[FILENAME_MAX], *e;
  int i, start;

  if (path == 0)
    return 0;
  for (start = i = 0;; i++)
    if (path[i] == 0 || (path[i] == ':' && !dos_path_drive_colon (path, start, i)))
      {
	int n = i - start;
	if (n > 0 && n + strlen (name) + 2 < sizeof buf)
	  {
	    memcpy (buf, path + start, n);
	    buf[n] = '/';
	    strcpy (buf + n + 1, name);
	    if ((e = dos_exec_ext (buf)))
	      return e;
	    if (has_ext (buf) && access (buf, F_OK) == 0)
	      return strdup (buf);
	  }
	if (path[i] == 0)
	  return 0;
	start = i + 1;
      }
}

/* ------------------------------------------------------------------ */
/* fork                                                                 */

/* The pieces of memory a snapshot holds: the program's data and bss and
   every block sbrk has, less the part of the stack below `low' (unused,
   since the stack grows down from where fork() is). */
struct piece { unsigned off, len; };

static int
pieces (struct piece *pc, int max, unsigned low)
{
  int i, n = 0;
  unsigned first = (unsigned)djgpp_first_ctor & ~3u;
  unsigned slo = __djgpp_stack_limit & ~3u, shi = (low - 256) & ~3u;

  for (i = 0; i < 256; i++)
    {
      unsigned a = __djgpp_memory_handle_list[i].address;
      unsigned e = a + __djgpp_memory_handle_size[i];
      if (__djgpp_memory_handle_list[i].handle == 0 && i > 0)
	continue;
      if (__djgpp_memory_handle_size[i] == 0)
	continue;
      if (e <= first)
	continue;
      if (a < first)
	a = first;
      a &= ~3u, e = (e + 3) & ~3u;
      /* cut out the unused stack */
      if (slo < shi && slo < e && shi > a)
	{
	  if (a < slo && n < max)
	    pc[n].off = a, pc[n].len = slo - a, n++;
	  if (shi < e && n < max)
	    pc[n].off = shi, pc[n].len = e - shi, n++;
	}
      else if (n < max)
	pc[n].off = a, pc[n].len = e - a, n++;
    }
  return n;
}

static int
count_blocks (void)
{
  int i, n = 0;
  for (i = 0; i < 256; i++)
    if (__djgpp_memory_handle_list[i].handle || i == 0)
      n = i + 1;
  return n;
}

/* Copy the parent's memory out to a new DPMI block.  0 if there's no
   memory for it. */
static int
snapshot (struct level *L, unsigned low)
{
  struct piece pc[64];
  int n = pieces (pc, 64, low), i;
  unsigned long total = 4 + 8 * n, off;
  __dpmi_meminfo mi;
  int sel;

  for (i = 0; i < n; i++)
    total += pc[i].len;
  mi.size = (total + 4095) & ~4095ul;
  if (__dpmi_allocate_memory (&mi) == -1)
    return 0;
  if ((sel = __dpmi_allocate_ldt_descriptors (1)) == -1)
    {
      __dpmi_free_memory (mi.handle);
      return 0;
    }
  __dpmi_set_segment_base_address (sel, mi.address);
  __dpmi_set_segment_limit (sel, mi.size - 1);
  L->sel = sel;
  L->handle = mi.handle;

  /* L is in the snapshot too, so its fields must be final before this */
  {
    unsigned hdr[1 + 2 * 64];
    hdr[0] = n;
    for (i = 0; i < n; i++)
      hdr[1 + 2 * i] = pc[i].off, hdr[2 + 2 * i] = pc[i].len;
    movedata (_my_ds (), (unsigned)hdr, sel, 0, 4 + 8 * n);
  }
  for (off = 4 + 8 * n, i = 0; i < n; i++)
    {
      movedata (_my_ds (), pc[i].off, sel, off, pc[i].len);
      off += pc[i].len;
    }
  return 1;
}

static void
snapshot_free (struct level *L)
{
  __dpmi_free_ldt_descriptor (L->sel);
  __dpmi_free_memory (L->handle);
}

/* Which handles are open: DOS's own table of them (the JFT, which the
   PSP points to), read in one go rather than asking DOS 255 times. */
static int
open_fds (unsigned char *open)
{
  unsigned long psp = _go32_info_block.linear_address_of_original_psp;
  unsigned n = _farpeekw (_dos_ds, psp + 0x32);
  unsigned long jft = _farpeekw (_dos_ds, psp + 0x36) * 16ul + _farpeekw (_dos_ds, psp + 0x34);
  unsigned i;
  if (n > MAXFD + 1)
    n = MAXFD + 1;
  memset (open, 0, MAXFD + 1);
  for (i = 0; i < n; i++)
    open[i] = _farpeekb (_dos_ds, jft + i) != 0xFF;
  return n;
}

pid_t
__wrap_fork (void)
{
  unsigned char isopen[MAXFD + 1];
  struct level *L;
  int fd, r;
  pid_t pid;

  if (depth >= MAXDEPTH)
    {
      errno = EAGAIN;
      return -1;
    }
  fflush (NULL);
  L = &lv[depth];
  pid = ++next_pid;

  /* The parent's handles: copies at SAVEFD and up, out of the child's way
     (marked reserved, so the loop passes over the copies it makes). */
  memset (L->saved, 0, sizeof L->saved);
  open_fds (isopen);
  for (fd = 0; fd <= MAXFD; fd++)
    {
      int c;
      if (reserved[fd] || !isopen[fd])
	continue;
      c = __real_fcntl (fd, F_DUPFD, SAVEFD);
      if (c < 0 || c > MAXFD)
	{
	  if (c >= 0)
	    __real_close (c);
	  for (c = 0; c < fd; c++)
	    if (L->saved[c])
	      {
		__real_close (L->saved[c]);
		reserved[L->saved[c]] = 0;
	      }
	  errno = EMFILE;
	  return -1;
	}
      L->saved[fd] = c;
      reserved[c] = 1;
    }
  if (getcwd (L->cwd, sizeof L->cwd) == 0)
    L->cwd[0] = 0;
  L->pid = pid;
  L->ppid = cur_pid;
  L->nblocks = count_blocks ();

  r = dos_fork_save (&L->regs);
  if (r == 0)
    {
      if (!snapshot (L, L->regs.esp - 128))
	{
	  for (fd = 0; fd <= MAXFD; fd++)
	    if (L->saved[fd])
	      {
		__real_close (L->saved[fd]);
		reserved[L->saved[fd]] = 0;
	      }
	  errno = ENOMEM;
	  return -1;
	}
      if (getenv ("DOSFORK_DEBUG"))
	{
	  struct piece pc[64];
	  int n = pieces (pc, 64, L->regs.esp - 128), i;
	  unsigned long t = 0;
	  for (i = 0; i < n; i++)
	    t += pc[i].len;
	  fprintf (stderr, "[fork: %d pieces, %lu bytes]\n", n, t);
	}
      /* the child */
      depth++;
      cur_pid = pid;
      ndone = 0;
      return 0;
    }

  /* The parent again, with the child's status.  Memory, handles and the
     current directory are as they were at the fork. */
  snapshot_free (L);
  for (fd = 0; fd <= MAXFD; fd++)
    if (L->saved[fd])
      reserved[L->saved[fd]] = 0;
  if (ndone < MAXDONE)
    {
      done[ndone].pid = pid;
      done[ndone].status = r & 0x3ffff;
      ndone++;
    }
  for (fd = 0; fd < MAXPIPES; fd++)
    if (pipes[fd].used)
      pipe_refresh (fd);
  /* Ctrl-C ended the child: on Unix the whole foreground group, the shell
     too, gets the SIGINT. */
  if ((r & 0x3ffff) == (SIGINT << 8))
    raise (SIGINT);
  return pid;
}

/* The child is done: put the parent back.  Never returns. */
static void __attribute__ ((noreturn))
child_end (int status)
{
  struct level *L = &lv[depth - 1];
  unsigned char isopen[MAXFD + 1];
  int fd, i;

  /* Handles: close the child's, put the parent's back. */
  open_fds (isopen);
  for (fd = 0; fd <= MAXFD; fd++)
    if (isopen[fd] && !reserved[fd])
      __real_close (fd);
  for (fd = 0; fd <= MAXFD; fd++)
    if (L->saved[fd])
      {
	__real_dup2 (L->saved[fd], fd);
	__real_close (L->saved[fd]);
      }
  if (L->cwd[0])
    chdir (L->cwd);
  /* Pipes the child made: their files go with it (the parent never knew) */
  for (i = 0; i < MAXPIPES; i++)
    if (pipes[i].used && pipes[i].depth >= depth)
      remove (pipes[i].name);

  /* Memory the child got from sbrk goes back to DPMI; the parent never
     knew it. */
  for (i = L->nblocks; i < 256; i++)
    if (__djgpp_memory_handle_list[i].handle)
      __dpmi_free_memory (__djgpp_memory_handle_list[i].handle);

  dos_resume (&L->regs, 0x40000000 | (status & 0x3ffff), L->sel,
	      (void *)((L->regs.esp - 1024) & ~15u));
}

void
__wrap_exit (int status)
{
  if (depth > 0)
    {
      fflush (NULL);
      child_end (status & 0xff);
    }
  {
    int i;
    for (i = 0; i < MAXPIPES; i++)
      if (pipes[i].used)
	remove (pipes[i].name);
  }
  __real_exit (status);
}

void
__wrap__exit (int status)
{
  if (depth > 0)
    child_end (status & 0xff);
  __real__exit (status);
}

/* A child whose signal action is the default ends with that signal. */
static void
child_sig (int sig)
{
  int i;
  if (depth > 0)
    child_end ((sig & 0x3ff) << 8);
  for (i = 0; i < MAXPIPES; i++)
    if (pipes[i].used)
      remove (pipes[i].name);
  __real__exit (sig == SIGINT ? 130 : 128 + 15);
}

static int
fatal_sig (int sig)
{
  return sig == SIGINT || sig == SIGQUIT || sig == SIGTERM || sig == SIGHUP
	 || sig == SIGPIPE || sig == SIGALRM || sig == SIGUSR1 || sig == SIGUSR2;
}

void (*__wrap_signal (int sig, void (*fn) (int))) (int)
{
  void (*old) (int);
  if (fn == SIG_DFL && (depth > 0 ? fatal_sig (sig) : sig == SIGINT))
    old = __real_signal (sig, child_sig);
  else
    old = __real_signal (sig, fn);
  return old == child_sig ? SIG_DFL : old;
}

int
__wrap_sigaction (int sig, const struct sigaction *act, struct sigaction *oact)
{
  struct sigaction a;
  int r;
  if (act && act->sa_handler == SIG_DFL && (depth > 0 ? fatal_sig (sig) : sig == SIGINT))
    {
      a = *act;
      a.sa_handler = child_sig;
      act = &a;
    }
  r = __real_sigaction (sig, act, oact);
  if (r == 0 && oact && oact->sa_handler == child_sig)
    oact->sa_handler = SIG_DFL;
  return r;
}

/* DJGPP blocks a signal by putting a handler of its own in its place,
   and on unblocking puts back the one it found.  readline blocks its
   signals, installs its handlers, and unblocks: and DJGPP puts the
   shell's back over them.  So: the mask is only kept, and signals (all
   of them set flags for later, in bash and readline) arrive at once. */
static sigset_t cur_mask;

int
__wrap_sigprocmask (int how, const sigset_t *set, sigset_t *oset)
{
  sigset_t old = cur_mask;
  int i;
  if (set)
    switch (how)
      {
      case SIG_BLOCK:
	for (i = 1; i < NSIG; i++)
	  if (sigismember (set, i))
	    sigaddset (&cur_mask, i);
	break;
      case SIG_UNBLOCK:
	for (i = 1; i < NSIG; i++)
	  if (sigismember (set, i))
	    sigdelset (&cur_mask, i);
	break;
      case SIG_SETMASK:
	cur_mask = *set;
	break;
      default:
	errno = EINVAL;
	return -1;
      }
  if (oset)
    *oset = old;
  return 0;
}

pid_t
__wrap_waitpid (pid_t pid, int *status, int options)
{
  int i;
  (void)options;
  for (i = 0; i < ndone; i++)
    if (pid == -1 || pid == 0 || done[i].pid == pid || done[i].pid == -pid)
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

pid_t
__wrap_wait (int *status)
{
  return __wrap_waitpid (-1, status, 0);
}

pid_t
__wrap_getpid (void)
{
  return cur_pid;
}

pid_t
__wrap_getppid (void)
{
  return depth > 0 ? lv[depth - 1].ppid : 1;
}

int
__wrap_kill (pid_t pid, int sig)
{
  int i;
  if (pid == 0 || pid == cur_pid || pid == -cur_pid)
    return sig ? raise (sig) : 0;
  for (i = 0; i < ndone; i++)
    if (done[i].pid == pid || done[i].pid == -pid)
      return 0;			/* finished, not yet waited for */
  errno = ESRCH;
  return -1;
}

/* ------------------------------------------------------------------ */
/* pipe                                                                 */

static void
tmpdir (char *buf, size_t n)
{
  const char *t = getenv ("TMPDIR");
  if (!t || !*t)
    t = getenv ("TMP");
  if (!t || !*t)
    t = getenv ("TEMP");
  if (t && *t && access (t, D_OK) == 0)
    snprintf (buf, n, "%s", t);
  else
    {
      snprintf (buf, n, "%c:/TMP", 'A' + getdisk ());
      mkdir (buf, 0777);
    }
}

int
__wrap_pipe (int fds[2])
{
  static unsigned seq;
  char dir[FILENAME_MAX];
  int p, w, r;

  for (p = 0; p < MAXPIPES && pipes[p].used; p++)
    ;
  if (p == MAXPIPES)
    {
      errno = EMFILE;
      return -1;
    }
  tmpdir (dir, sizeof dir);
  do
    snprintf (pipes[p].name, sizeof pipes[p].name, "%s/PI%02X%04X.$$$",
	      dir, (unsigned)cur_pid & 0xff, ++seq & 0xffff);
  while (access (pipes[p].name, F_OK) == 0);
  w = open (pipes[p].name, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0600);
  if (w < 0)
    return -1;
  r = open (pipes[p].name, O_RDONLY | O_BINARY);
  if (r < 0)
    {
      __real_close (w);
      remove (pipes[p].name);
      return -1;
    }
  if (w > MAXFD || r > MAXFD)
    {
      __real_close (w);
      __real_close (r);
      remove (pipes[p].name);
      errno = EMFILE;
      return -1;
    }
  pipes[p].used = 1;
  pipes[p].depth = depth;
  fdp[w].pipe = p + 1, fdp[w].wr = 1;
  fdp[r].pipe = p + 1, fdp[r].wr = 0;
  fds[0] = r;
  fds[1] = w;
  return 0;
}

/* What the writer has written, for the readers: DOS keeps a file's size
   per open handle, so a handle opened before the writing still sees an
   empty file.  Commit the writers, and reopen the readers where they
   were. */
static void
pipe_refresh (int p)
{
  int fd;
  for (fd = 0; fd <= MAXFD; fd++)
    if (fdp[fd].pipe == p + 1 && fdp[fd].wr)
      fsync (fd);
  for (fd = 0; fd <= MAXFD; fd++)
    if (fdp[fd].pipe == p + 1 && !fdp[fd].wr)
      {
	off_t at = lseek (fd, 0, SEEK_CUR);
	int mode = setmode (fd, O_BINARY);
	int n = open (pipes[p].name, O_RDONLY | O_BINARY);
	if (n < 0)
	  continue;
	lseek (n, at, SEEK_SET);
	__real_dup2 (n, fd);
	__real_close (n);
	if (mode != -1)
	  setmode (fd, mode);
      }
}

/* fd is no longer this pipe end.  The last of a pipe made in this process
   removes the file; the last writer lets the readers see what it wrote. */
static void
pipe_drop (int fd)
{
  int p, wr, i;
  if (fd < 0 || fd > MAXFD || fdp[fd].pipe == 0)
    return;
  p = fdp[fd].pipe - 1;
  wr = fdp[fd].wr;
  fdp[fd].pipe = 0;
  for (i = 0; i <= MAXFD; i++)
    if (fdp[i].pipe == p + 1)
      break;
  if (i > MAXFD)
    {
      if (pipes[p].depth == depth)
	{
	  remove (pipes[p].name);
	  pipes[p].used = 0;
	}
      return;
    }
  if (wr)
    {
      for (i = 0; i <= MAXFD; i++)
	if (fdp[i].pipe == p + 1 && fdp[i].wr)
	  return;
      pipe_refresh (p);
    }
}

int
__wrap_close (int fd)
{
  int r;
  if (fd >= 0 && fd <= MAXFD && reserved[fd])
    return 0;			/* a parent's: not the child's to close */
  r = __real_close (fd);
  if (r == 0)
    pipe_drop (fd);
  return r;
}

static int
dup_note (int from, int to)
{
  if (to >= 0 && to <= MAXFD && from >= 0 && from <= MAXFD)
    fdp[to] = fdp[from];
  return to;
}

int
__wrap_dup (int fd)
{
  return dup_note (fd, __real_dup (fd));
}

int
__wrap_dup2 (int fd, int to)
{
  int r;
  if (to >= 0 && to <= MAXFD && reserved[to])
    {
      errno = EBADF;
      return -1;
    }
  if (fd == to)
    return __real_dup2 (fd, to);
  r = __real_dup2 (fd, to);
  if (r >= 0)
    {
      if (fdp[to].pipe)
	{
	  /* to's old pipe end is gone: account for it as a close */
	  unsigned char keep_p = fdp[fd].pipe, keep_w = fdp[fd].wr;
	  pipe_drop (to);
	  fdp[fd].pipe = keep_p, fdp[fd].wr = keep_w;
	}
      dup_note (fd, r);
    }
  return r;
}

int
__wrap_fcntl (int fd, int cmd, ...)
{
  va_list ap;
  int arg, r;
  va_start (ap, cmd);
  arg = va_arg (ap, int);
  va_end (ap);
  if (cmd == F_DUPFD)
    {
      /* skip over the parents' copies */
      r = __real_fcntl (fd, F_DUPFD, arg);
      while (r >= 0 && r <= MAXFD && reserved[r])
	{
	  int n = __real_fcntl (fd, F_DUPFD, r + 1);
	  __real_close (r);
	  r = n;
	}
      return dup_note (fd, r);
    }
  return __real_fcntl (fd, cmd, arg);
}

/* ------------------------------------------------------------------ */
/* execve                                                               */

static char **
env_for_dos (char *const envp[])
{
  int n, i;
  char **e;
  for (n = 0; envp && envp[n]; n++)
    ;
  e = malloc ((n + 1) * sizeof *e);
  if (e == 0)
    return (char **)envp;
  for (i = 0; i < n; i++)
    {
      e[i] = envp[i];
      if (strncmp (envp[i], "PATH=", 5) == 0)
	{
	  char *d = path_to_dos (envp[i] + 5);
	  if (d && (e[i] = malloc (strlen (d) + 6)))
	    sprintf (e[i], "PATH=%s", d);
	  else
	    e[i] = envp[i];
	  free (d);
	}
    }
  e[n] = 0;
  return e;
}

static const char *
env_get (char *const envp[], const char *name)
{
  size_t n = strlen (name);
  int i;
  for (i = 0; envp && envp[i]; i++)
    if (strncmp (envp[i], name, n) == 0 && envp[i][n] == '=')
      return envp[i] + n + 1;
  return 0;
}

int
__wrap_execve (const char *path, char *const argv[], char *const envp[])
{
  char head[160], *prog = (char *)path, **av = (char **)argv, *ext;
  const char *dot;
  int fd, n, r;

  if ((ext = dos_exec_ext (path)))
    prog = ext;
  fd = open (prog, O_RDONLY | O_BINARY);
  if (fd < 0)
    return -1;
  n = read (fd, head, sizeof head - 1);
  __real_close (fd);
  if (n < 0)
    n = 0;
  head[n] = 0;
  dot = strrchr (prog, '.');
  if (dot && (strchr (dot, '/') || strchr (dot, '\\')))
    dot = 0;

  if (n > 2 && head[0] == '#' && head[1] == '!')
    {
      /* #!interpreter [arg] */
      char *s = head + 2, *interp, *arg = 0, *base, *found;
      int argc, i;
      s[strcspn (s, "\r\n")] = 0;
      while (*s == ' ' || *s == '\t')
	s++;
      interp = s;
      while (*s && *s != ' ' && *s != '\t')
	s++;
      if (*s)
	{
	  *s++ = 0;
	  while (*s == ' ' || *s == '\t')
	    s++;
	  if (*s)
	    {
	      arg = s;
	      s += strlen (s);
	      while (s > arg && (s[-1] == ' ' || s[-1] == '\t'))
		*--s = 0;
	    }
	}
      base = strrchr (interp, '/');
      base = base ? base + 1 : interp;
      /* `#!/usr/bin/env perl' */
      if (strcmp (base, "env") == 0 && arg)
	{
	  base = arg;
	  arg = 0;
	}
      if (strcasecmp (base, "sh") == 0 || strcasecmp (base, "bash") == 0
	  || strcasecmp (base, "sh.exe") == 0 || strcasecmp (base, "bash.exe") == 0)
	{
	  errno = ENOEXEC;	/* bash runs it itself */
	  return -1;
	}
      found = (base == interp) ? 0 : dos_exec_ext (interp);
      if (found == 0 && access (interp, F_OK) == 0 && has_ext (interp) && base != arg)
	found = strdup (interp);
      if (found == 0)
	found = search_path (base, env_get (envp, "PATH"));
      if (found == 0)
	{
	  errno = ENOENT;
	  return -1;
	}
      for (argc = 0; argv[argc]; argc++)
	;
      av = malloc ((argc + 3) * sizeof *av);
      if (av == 0)
	{
	  errno = ENOMEM;
	  return -1;
	}
      i = 0;
      av[i++] = found;
      if (arg)
	av[i++] = arg;
      av[i++] = prog;
      for (n = 1; n < argc; n++)
	av[i++] = argv[n];
      av[i] = 0;
      prog = found;
    }
  else if (!(n >= 2 && ((head[0] == 'M' && head[1] == 'Z') || (head[0] == 'Z' && head[1] == 'M')))
	   && !(dot && (strcasecmp (dot, ".com") == 0 || strcasecmp (dot, ".bat") == 0
			|| strcasecmp (dot, ".btm") == 0 || strcasecmp (dot, ".exe") == 0)))
    {
      errno = ENOEXEC;		/* a shell script without #! */
      return -1;
    }

  fflush (NULL);
  int23_fired ();
  r = spawnve (P_WAIT, prog, av, env_for_dos (envp));
  if (r == -1)
    return -1;
  if (int23_fired () && depth > 0)
    child_end (SIGINT << 8);	/* ended by Ctrl-C: as if by SIGINT */
  __wrap__exit (r);
}

/* ------------------------------------------------------------------ */
/* DJGPP has no wide characters; readline asks for these two anyway.   */
/* One byte is one character (DOS code pages).                          */

#include <wchar.h>
#include <wctype.h>

size_t
mbrtowc (wchar_t *pwc, const char *s, size_t n, mbstate_t *ps)
{
  (void)ps;
  if (s == 0)
    return 0;
  if (n == 0)
    return (size_t)-2;
  if (pwc)
    *pwc = (unsigned char)*s;
  return *s != 0;
}

wint_t
towlower (wint_t c)
{
  return c < 256 ? (wint_t)tolower ((int)c) : c;
}

/* ------------------------------------------------------------------ */
/* The keyboard and Ctrl-C                                              */

#include <pc.h>
#include <sys/exceptn.h>

/* A key for readline from the console, or -3 if fd isn't the console,
   or -2 after Ctrl-C (SIGINT raised).  Read through the BIOS, not DOS,
   so DOS never sees Ctrl-C; keys that make no character come as the
   sequences an ANSI terminal sends. */
int
dos_rl_getkey (int fd)
{
  static char pending[8];
  static int npend, ppos;
  __dpmi_regs r;
  int old, ax, al, ah;
  const char *seq = 0;

  if (ppos < npend)
    return (unsigned char)pending[ppos++];
  if (!isatty (fd) || (_get_dev_info (fd) & 0x81) != 0x81)
    return -3;			/* not console input */

  old = __djgpp_set_ctrl_c (0);	/* Ctrl-C is just a key while we wait */
  for (;;)
    {
      memset (&r, 0, sizeof r);
      r.h.ah = 0x11;		/* key waiting? */
      __dpmi_int (0x16, &r);
      if ((r.x.flags & 0x40) == 0)	/* ZF clear: yes */
	break;
      __dpmi_yield ();
    }
  memset (&r, 0, sizeof r);
  r.h.ah = 0x10;
  __dpmi_int (0x16, &r);
  __djgpp_set_ctrl_c (old);
  ax = r.x.ax, al = ax & 0xff, ah = ax >> 8;

  if (ax == 0 || al == 3)	/* Ctrl-Break, Ctrl-C */
    {
      raise (SIGINT);
      return -2;
    }
  if (al != 0 && !(al == 0xE0 && ah != 0))
    return al;
  switch (ah)
    {
    case 0x48: seq = "\033[A"; break;	/* up */
    case 0x50: seq = "\033[B"; break;	/* down */
    case 0x4D: seq = "\033[C"; break;	/* right */
    case 0x4B: seq = "\033[D"; break;	/* left */
    case 0x47: seq = "\033[H"; break;	/* home */
    case 0x4F: seq = "\033[F"; break;	/* end */
    case 0x53: seq = "\033[3~"; break;	/* delete */
    case 0x52: seq = "\033[2~"; break;	/* insert */
    case 0x49: seq = "\033[5~"; break;	/* page up */
    case 0x51: seq = "\033[6~"; break;	/* page down */
    case 0x73: seq = "\033[1;5D"; break;	/* ctrl-left */
    case 0x74: seq = "\033[1;5C"; break;	/* ctrl-right */
    case 0x93: seq = "\033[3;5~"; break;	/* ctrl-delete */
    case 0x0F: seq = "\033[Z"; break;	/* shift-tab */
    default: return dos_rl_getkey (fd);	/* F1 and such: nothing */
    }
  strcpy (pending, seq + 1);
  npend = strlen (pending);
  ppos = 0;
  return '\033';
}

/* Ctrl-C (or Ctrl-Break) typed while the shell is busy: it waits in the
   BIOS keyboard buffer, since DOS isn't asked for keys meanwhile.  bash
   looks here wherever it checks for interrupts (QUIT).  Like a Unix
   terminal, the interrupt throws away what was typed ahead of it. */
#include <sys/farptr.h>

void
dos_check_break (void)
{
  unsigned head = _farpeekw (_dos_ds, 0x41A), tail = _farpeekw (_dos_ds, 0x41C);
  unsigned start = _farpeekw (_dos_ds, 0x480), end = _farpeekw (_dos_ds, 0x482);
  unsigned p;
  int n;

  if (head == tail || start >= end || head < start || head >= end)
    return;
  for (p = head, n = 0; p != tail && n < 32; n++)
    {
      unsigned k = _farpeekw (_dos_ds, 0x400 + p);
      if (k == 0 || (k & 0xff) == 3)
	{
	  _farpokew (_dos_ds, 0x41A, tail);	/* flush */
	  raise (SIGINT);
	  return;
	}
      p += 2;
      if (p >= end)
	p = start;
    }
}
