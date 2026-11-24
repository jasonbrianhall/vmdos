/* dosbash.h -- included first in every bash source file when building for
   DOS with DJGPP (-include dos/dosbash.h).  See dos/dosproc.c. */
#ifndef DOSBASH_H
#define DOSBASH_H

#include <sys/types.h>
#include <signal.h>

/* DOS has no child processes that run alongside the shell, so it has no
   SIGCHLD.  bash names it in a few places; give it a number of its own,
   below NSIG, that nothing ever raises. */
#ifndef SIGCHLD
#  define SIGCHLD 305
#endif

/* dos/dosproc.c */
extern char *dos_exec_ext (const char *);
extern int dos_path_drive_colon (const char *, int, int);

#endif
