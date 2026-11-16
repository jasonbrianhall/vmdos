/* poll() constants for ssh's dos-sock.c in WGET, if DJGPP has none */
#ifndef WGET_POLL_H
#define WGET_POLL_H
#if defined(__has_include_next) && __has_include_next(<poll.h>)
#include_next <poll.h>
#else
#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020
struct pollfd { int fd; short events, revents; };
#endif
#endif
