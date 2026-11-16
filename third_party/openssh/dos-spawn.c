/*
 * dos-spawn.c: what sftp.c and scp.c (patched) call instead of fork() and
 * execvp(ssh): ssh's main(), renamed ssh_main, run as a coroutine
 * (dos-co.c) with the given pipe ends as its stdin and stdout. Linked into
 * SFTP.EXE and SCP.EXE only, with ssh's objects (tools/build-openssh.sh
 * renames ssh.o's main and cleanup_exit, and the program's cleanup_exit).
 */
#include "includes.h"

#include <sys/types.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "xmalloc.h"
#include "dos-co.h"

int ssh_main(int, char **);		/* ssh.c's main() */
void ssh_cleanup_exit(int);		/* clientloop.c's cleanup_exit() */
void prog_cleanup_exit(int);		/* scp.c's, or the one below */

static int ssh_argc;
static char **ssh_argv;

static void
run_ssh(void)
{
	optind = 1;			/* sftp/scp used getopt() first */
	optreset = 1;
	dos_exit(ssh_main(ssh_argc, ssh_argv));
}

pid_t
dos_spawn(const char *path, char **args, int in, int out)
{
	const char *base;
	char lower[64];
	int i, n;

	if (co_ssh_state == CO_RUNNING) {
		errno = EAGAIN;		/* one ssh at a time (no scp -3) */
		return -1;
	}
	base = strrchr(path, '/');
	if (base == NULL)
		base = strrchr(path, '\\');
	base = base != NULL ? base + 1 : path;
	for (i = 0; base[i] != '\0' && i < (int)sizeof lower - 1; i++)
		lower[i] = tolower((unsigned char)base[i]);
	lower[i] = '\0';
	if (strstr(lower, "ssh") == NULL) {
		errno = ENOEXEC;	/* only the built-in ssh can run */
		return -1;
	}
	if (ssh_argv != NULL) {
		for (i = 0; i < ssh_argc; i++)
			free(ssh_argv[i]);
		free(ssh_argv);
	}
	for (n = 0; args[n] != NULL; n++)
		;
	ssh_argv = xcalloc(n + 1, sizeof(char *));
	for (i = 0; i < n; i++)
		ssh_argv[i] = xstrdup(i == 0 ? "ssh" : args[i]);
	ssh_argc = n;
	co_ssh_in = dos_dup_owner(in, CO_SSH);
	co_ssh_out = dos_dup_owner(out, CO_SSH);
	if (co_ssh_in == -1 || co_ssh_out == -1 || co_start(run_ssh) == -1)
		return -1;
	return CO_SSH_PID;
}

/* log.c's fatal() and friends: ssh's or the program's, by who is running. */
void
cleanup_exit(int i)
{
	if (co_current == CO_SSH)
		ssh_cleanup_exit(i);
	prog_cleanup_exit(i);
}

/* sftp has none of its own: libssh's (cleanup.c) is just this. */
__attribute__((weak)) void
prog_cleanup_exit(int i)
{
	_exit(i);
}
