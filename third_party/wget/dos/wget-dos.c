/*
 * wget-dos.c: what ssh's dos-sock.c needs from the rest of the ssh port
 * (third_party/openssh), for WGET: no coroutines (sftp/scp run ssh in
 * one; WGET doesn't), no alarms, calloc that exits when there's none.
 */
#include <stdio.h>
#include <stdlib.h>
#include "dos-co.h"

int co_current, co_ssh_state = CO_NONE, co_ssh_in = -1, co_ssh_out = -1;

void co_yield(void) { }
void dos_alarm_check(void) { }

void *
xcalloc(size_t n, size_t size)
{
	void *p = calloc(n ? n : 1, size ? size : 1);

	if (p == NULL) {
		fputs("wget: out of memory\n", stderr);
		exit(1);
	}
	return p;
}
