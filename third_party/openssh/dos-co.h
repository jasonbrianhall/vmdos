/* dos-co.h: coroutines and in-memory pipes (dos-co.c). */
#ifndef DOS_CO_H
#define DOS_CO_H

#include <sys/types.h>

#define CO_MAIN 0
#define CO_SSH 1
#define CO_NONE 0
#define CO_RUNNING 1
#define CO_DONE 2
#define CO_SSH_PID 0x5353	/* ssh's "process id" inside sftp/scp */

#define PIPE_FD0 200		/* in-memory pipes: fds 200-231 */
#define NPIPE_FDS 32

extern int co_current, co_ssh_state, co_ssh_in, co_ssh_out;

int co_start(void (*)(void));
unsigned dos_alarm(unsigned);
void dos_alarm_check(void);
void co_yield(void);

void dos_exit(int) __attribute__((noreturn));
void dos__exit(int) __attribute__((noreturn));
int dos_stdin_fd(void);
int dos_stdout_fd(void);
pid_t dos_waitpid(pid_t, int *, int);
int dos_kill(pid_t, int);

int dos_pipe(int[2]);
int dos_close(int);
int dos_dup(int);
int dos_dup_owner(int, int);
int dos_dup2(int, int);
int dos_is_pipe(int);
int dos_isatty_pipe(int);
int dos_pipe_setnb(int, int);
int dos_pipe_wr(int);
int dos_pipe_ready(int, int);
ssize_t dos_pipe_read(int, void *, size_t);
ssize_t dos_pipe_write(int, const void *, size_t);

#endif
