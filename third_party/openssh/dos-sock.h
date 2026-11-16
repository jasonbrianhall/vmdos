/* dos-sock.h: sockets on vmdos's own TCP (dos-sock.c). */
#ifndef DOS_SOCK_H
#define DOS_SOCK_H

#include <sys/types.h>

#define SOCK_FD0 240		/* sockets: fds 240-247 */
#define NSOCK_FDS 8

void dos_sock_config(void);
int dos_is_sock(int);
ssize_t dos_sock_read(int, void *, size_t);
ssize_t dos_sock_write(int, const void *, size_t);
int dos_sock_poll(int, int);
int dos_sock_close(int);
int dos_sock_setnb(int, int);

#endif
