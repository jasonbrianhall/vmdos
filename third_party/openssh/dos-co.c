/*
 * dos-co.c: ssh inside sftp and scp, for DOS (vmdos).
 *
 * sftp and scp normally fork ssh and talk to it through pipes; DOS runs one
 * program at a time. So SFTP.EXE and SCP.EXE carry ssh within them and run
 * it as a coroutine: two stacks in one program, switching whenever one side
 * would wait (an empty pipe, nothing to do in ssh's poll loop). The pipes
 * are in memory, with fd numbers of their own (PIPE_FD0...), handled by
 * the read/write/close/dup/fcntl/isatty/poll wrappers (dos-compat.h).
 * ssh's stdin and stdout are the pipe ends it was given (STDIN_FILENO and
 * STDOUT_FILENO follow the running coroutine); exit() inside ssh ends the
 * coroutine, not the program; waitpid() and kill() on its "pid" work.
 * dos-spawn.c starts it (only SFTP.EXE and SCP.EXE link that).
 */
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>

#include "dos-co.h"

#undef read
#undef write
#undef close
#undef dup
#undef dup2
#undef isatty
#undef exit
#undef _exit
#undef kill
#undef waitpid
#undef pipe

/* --- coroutines --- */

int co_current = CO_MAIN;
int co_ssh_state = CO_NONE;
int co_ssh_in = -1, co_ssh_out = -1;
static int co_ssh_status;
static int co_ssh_killed;
static void *co_sp[2];		/* saved stack pointers */
static void *co_stack;		/* ssh's stack */
static void (*co_func)(void);

/* Save callee-saved registers and the stack pointer; switch stacks. */
void co_switch(void **save, void *to);
__asm__(
	".text\n"
	".globl _co_switch\n"
	"_co_switch:\n"
	"	pushl %ebp\n"
	"	pushl %ebx\n"
	"	pushl %esi\n"
	"	pushl %edi\n"
	"	movl 20(%esp), %eax\n"
	"	movl %esp, (%eax)\n"
	"	movl 24(%esp), %esp\n"
	"	popl %edi\n"
	"	popl %esi\n"
	"	popl %ebx\n"
	"	popl %ebp\n"
	"	ret\n");

static void
co_entry(void)
{
	co_func();
	dos_exit(0);
}

#define CO_STACK (1024 * 1024)

int
co_start(void (*fn)(void))
{
	unsigned long *sp;

	free(co_stack);			/* an earlier ssh's, done */
	if ((co_stack = malloc(CO_STACK)) == NULL)
		return -1;
	sp = (unsigned long *)(((unsigned long)co_stack + CO_STACK) & ~15UL);
	*--sp = 0;			/* co_entry's return address: none */
	*--sp = (unsigned long)co_entry;
	*--sp = 0;			/* ebp */
	*--sp = 0;			/* ebx */
	*--sp = 0;			/* esi */
	*--sp = 0;			/* edi */
	co_sp[CO_SSH] = sp;
	co_func = fn;
	co_ssh_state = CO_RUNNING;
	co_ssh_killed = 0;
	return 0;
}

/*
 * alarm(), synchronously: DJGPP delivers SIGALRM from the timer interrupt
 * by trapping the next memory access, which ends the program if that
 * happens on ssh's coroutine stack. So the deadline is kept here and
 * SIGALRM raised from where the program waits (poll, pipes). Only the
 * progress meter uses it.
 */
static unsigned long alarm_at;		/* BIOS tick, 0: none */

unsigned
dos_alarm(unsigned secs)
{
	unsigned long now = _farpeekl(_dos_ds, 0x46c);
	unsigned left = alarm_at ? (alarm_at > now ? (alarm_at - now) / 18 : 0) : 0;

	alarm_at = secs ? now + secs * 182 / 10 : 0;
	if (alarm_at == 0 && secs)
		alarm_at = 1;
	return left;
}

void
dos_alarm_check(void)
{
	if (alarm_at != 0 && co_current == CO_MAIN &&
	    _farpeekl(_dos_ds, 0x46c) >= alarm_at) {
		alarm_at = 0;
		raise(SIGALRM);
	}
}

/* Let the other side run; back here when it waits. */
void
co_yield(void)
{
	dos_alarm_check();
	if (co_ssh_state != CO_RUNNING)
		return;
	if (co_current == CO_MAIN) {
		co_current = CO_SSH;
		co_switch(&co_sp[CO_MAIN], co_sp[CO_SSH]);
	} else {
		co_current = CO_MAIN;
		co_switch(&co_sp[CO_SSH], co_sp[CO_MAIN]);
		/* back in ssh: killed meanwhile? */
		if (co_ssh_killed)
			dos_exit(255);
	}
}

static void pipe_close_owned(int owner);

/* ssh is done (exit() or return from main): back to the main side for good. */
static void
co_ssh_done(int status)
{
	co_ssh_status = status & 0xff;
	co_ssh_state = CO_DONE;
	pipe_close_owned(CO_SSH);
	co_current = CO_MAIN;
	co_switch(&co_sp[CO_SSH], co_sp[CO_MAIN]);
	abort();			/* never resumed */
}

void
dos_exit(int status)
{
	if (co_current == CO_SSH)
		co_ssh_done(status);
	exit(status);
}

/*
 * _exit() as exit(): on DOS the atexit handlers must run, or Watt-32 stays
 * hooked into the packet driver and the next frame calls into freed memory
 * (a crash in DOS). There are no forked children whose stdio could be
 * flushed twice.
 */
void
dos__exit(int status)
{
	if (co_current == CO_SSH)
		co_ssh_done(status);
	exit(status);
}

int
dos_stdin_fd(void)
{
	return co_current == CO_SSH ? co_ssh_in : 0;
}

int
dos_stdout_fd(void)
{
	return co_current == CO_SSH ? co_ssh_out : 1;
}

pid_t
dos_waitpid(pid_t pid, int *status, int options)
{
	if (pid != CO_SSH_PID && pid != -1)
		return waitpid(pid, status, options);
	if (co_ssh_state == CO_NONE) {
		errno = ECHILD;
		return -1;
	}
	while (co_ssh_state == CO_RUNNING) {
		if (options & WNOHANG)
			return 0;
		co_yield();
	}
	if (status != NULL)
		*status = co_ssh_status;
	return CO_SSH_PID;
}

int
dos_kill(pid_t pid, int sig)
{
	if (pid == CO_SSH_PID) {
		if (co_ssh_state == CO_RUNNING &&
		    (sig == SIGTERM || sig == SIGINT || sig == SIGKILL ||
		    sig == SIGHUP))
			co_ssh_killed = 1;
		return 0;
	}
	if (sig > SIGPROF)	/* dos-compat.h's made-up signals */
		return 0;
	return kill(pid, sig);
}

/* --- pipes in memory --- */

#define PIPE_SIZE (64 * 1024)
#define NPIPES 8

struct mpipe {
	int used;
	unsigned char *buf;
	size_t head, len;	/* data at buf[head], len bytes (circular) */
	int rrefs, wrefs;	/* open read and write ends */
};

struct pfd {
	int used;
	int pipe;
	int wr;			/* the write end */
	int owner;		/* CO_MAIN or CO_SSH */
	int nonblock;
};

static struct mpipe pipes[NPIPES];
static struct pfd pfds[NPIPE_FDS];

static struct pfd *
pfd_get(int fd)
{
	if (fd < PIPE_FD0 || fd >= PIPE_FD0 + NPIPE_FDS ||
	    !pfds[fd - PIPE_FD0].used)
		return NULL;
	return &pfds[fd - PIPE_FD0];
}

int
dos_is_pipe(int fd)
{
	return pfd_get(fd) != NULL;
}

static int
pfd_new(int pipe, int wr, int owner)
{
	int i;

	for (i = 0; i < NPIPE_FDS; i++)
		if (!pfds[i].used) {
			pfds[i].used = 1;
			pfds[i].pipe = pipe;
			pfds[i].wr = wr;
			pfds[i].owner = owner;
			pfds[i].nonblock = 0;
			if (wr)
				pipes[pipe].wrefs++;
			else
				pipes[pipe].rrefs++;
			return PIPE_FD0 + i;
		}
	errno = EMFILE;
	return -1;
}

int
dos_pipe(int fds[2])
{
	int p;

	for (p = 0; p < NPIPES; p++)
		if (!pipes[p].used)
			break;
	if (p == NPIPES) {
		errno = EMFILE;
		return -1;
	}
	if ((pipes[p].buf = malloc(PIPE_SIZE)) == NULL) {
		errno = ENOMEM;
		return -1;
	}
	pipes[p].used = 1;
	pipes[p].head = pipes[p].len = 0;
	pipes[p].rrefs = pipes[p].wrefs = 0;
	if ((fds[0] = pfd_new(p, 0, co_current)) == -1 ||
	    (fds[1] = pfd_new(p, 1, co_current)) == -1)
		return -1;
	return 0;
}

static void
pfd_close(struct pfd *f)
{
	struct mpipe *p = &pipes[f->pipe];

	if (f->wr)
		p->wrefs--;
	else
		p->rrefs--;
	f->used = 0;
	if (p->rrefs == 0 && p->wrefs == 0) {
		free(p->buf);
		p->buf = NULL;
		p->used = 0;
	}
}

static void
pipe_close_owned(int owner)
{
	int i;

	for (i = 0; i < NPIPE_FDS; i++)
		if (pfds[i].used && pfds[i].owner == owner)
			pfd_close(&pfds[i]);
}

int
dos_close(int fd)
{
	struct pfd *f = pfd_get(fd);

	if (f == NULL)
		return close(fd);
	pfd_close(f);
	return 0;
}

/* A new fd for the same pipe end, owned by whoever runs (or 'owner'). */
int
dos_dup_owner(int fd, int owner)
{
	struct pfd *f = pfd_get(fd);

	if (f == NULL)
		return dup(fd);
	return pfd_new(f->pipe, f->wr, owner);
}

int
dos_dup(int fd)
{
	return dos_dup_owner(fd, co_current);
}

/*
 * dup2(): inside sftp/scp, ssh's stdin and stdout are its pipe ends and
 * fds 0-2 are the program's console, so ssh's redirections (stdout to the
 * null device once the session has its own copies) swap its own ends and
 * leave 0-2 alone.
 */
int
dos_dup2(int fd, int to)
{
	if (co_current == CO_SSH && co_ssh_state == CO_RUNNING) {
		int nfd;

		if (to <= 2 && to != co_ssh_in && to != co_ssh_out)
			return to;
		if (to == co_ssh_in || to == co_ssh_out) {
			if ((nfd = dos_dup_owner(fd, CO_SSH)) == -1)
				return -1;
			if (pfd_get(to) != NULL)
				dos_close(to);
			if (to == co_ssh_in)
				co_ssh_in = nfd;
			if (to == co_ssh_out)
				co_ssh_out = nfd;
			return to;
		}
	}
	if (pfd_get(fd) != NULL || pfd_get(to) != NULL) {
		errno = EBADF;
		return -1;
	}
	return dup2(fd, to);
}

int
dos_isatty_pipe(int fd)
{
	return pfd_get(fd) != NULL ? 0 : -1;
}

int
dos_pipe_setnb(int fd, int nb)
{
	struct pfd *f = pfd_get(fd);

	if (f == NULL)
		return -1;
	if (nb >= 0)
		f->nonblock = nb;
	return f->nonblock;
}

int
dos_pipe_wr(int fd)
{
	struct pfd *f = pfd_get(fd);

	return f != NULL && f->wr;
}

/* For poll: 1 readable/writable, 0 not, with POLLHUP-like ends as ready. */
int
dos_pipe_ready(int fd, int for_write)
{
	struct pfd *f = pfd_get(fd);
	struct mpipe *p;

	if (f == NULL)
		return 0;
	p = &pipes[f->pipe];
	if (for_write)
		return f->wr && (p->rrefs == 0 || p->len < PIPE_SIZE);
	return !f->wr && (p->len > 0 || p->wrefs == 0);
}

ssize_t
dos_pipe_read(int fd, void *buf, size_t len)
{
	struct pfd *f = pfd_get(fd);
	struct mpipe *p;
	size_t n, first;

	if (f == NULL || f->wr) {
		errno = EBADF;
		return -1;
	}
	p = &pipes[f->pipe];
	while (p->len == 0) {
		if (p->wrefs == 0)
			return 0;			/* end of file */
		if (f->nonblock || co_ssh_state != CO_RUNNING) {
			errno = EAGAIN;
			return -1;
		}
		co_yield();
	}
	n = len < p->len ? len : p->len;
	first = PIPE_SIZE - p->head;
	if (first > n)
		first = n;
	memcpy(buf, p->buf + p->head, first);
	memcpy((char *)buf + first, p->buf, n - first);
	p->head = (p->head + n) % PIPE_SIZE;
	p->len -= n;
	return n;
}

ssize_t
dos_pipe_write(int fd, const void *buf, size_t len)
{
	struct pfd *f = pfd_get(fd);
	struct mpipe *p;
	size_t n, tail, first;

	if (f == NULL || !f->wr) {
		errno = EBADF;
		return -1;
	}
	p = &pipes[f->pipe];
	for (;;) {
		if (p->rrefs == 0) {
			errno = EPIPE;
			return -1;
		}
		if (p->len < PIPE_SIZE)
			break;
		if (f->nonblock || co_ssh_state != CO_RUNNING) {
			errno = EAGAIN;
			return -1;
		}
		co_yield();
	}
	n = PIPE_SIZE - p->len;
	if (n > len)
		n = len;
	tail = (p->head + p->len) % PIPE_SIZE;
	first = PIPE_SIZE - tail;
	if (first > n)
		first = n;
	memcpy(p->buf + tail, buf, first);
	memcpy(p->buf, (const char *)buf + first, n - first);
	p->len += n;
	return n;
}
