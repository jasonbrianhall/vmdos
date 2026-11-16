/*
 * curl_config.h for CURL.EXE (tools/build-curl.sh): DJGPP, mbedTLS, and
 * vmdos's own TCP through ssh's socket layer (third_party/openssh/
 * dos-sock.c) where curl expects Watt-32's library: dos/curl-dos.c gives
 * curl the few Watt-32 calls its DOS code makes (select_s, close_s,
 * ioctlsocket) and recv/send. Written by hand from a CMake configuration
 * of the same features on Linux, less what DJGPP or dos-sock.c lack.
 */
#ifndef CURL_CONFIG_DOS_H
#define CURL_CONFIG_DOS_H

#define CURL_OS "MSDOS"
#define CURL_CA_BUNDLE "C:/VMDOS/NET/CACERT.PEM"

/* HTTP(S), FTP(S), FILE; the rest off, as in the CMake configuration */
#define CURL_DISABLE_DICT 1
#define CURL_DISABLE_GOPHER 1
#define CURL_DISABLE_IMAP 1
#define CURL_DISABLE_LDAP 1
#define CURL_DISABLE_LDAPS 1
#define CURL_DISABLE_MQTT 1
#define CURL_DISABLE_POP3 1
#define CURL_DISABLE_RTSP 1
#define CURL_DISABLE_SMB 1
#define CURL_DISABLE_SMTP 1
#define CURL_DISABLE_TELNET 1
#define CURL_DISABLE_TFTP 1
#define CURL_DISABLE_WEBSOCKETS 1

#define USE_MBEDTLS 1
#define HAVE_MBEDTLS_DES_CRYPT_ECB 1
#define USE_IPV6 1			/* dos-sock.c does IPv6 (VMTCP.CFG's) */

/* sockets: Watt-32's headers, dos-sock.c's and curl-dos.c's functions */
#define HAVE_ARPA_INET_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SOCKET 1
#define HAVE_RECV 1
#define HAVE_SEND 1
#define HAVE_GETADDRINFO 1
#define HAVE_FREEADDRINFO 1
#define HAVE_GETPEERNAME 1
#define HAVE_GETSOCKNAME 1
#define HAVE_IOCTL_FIONBIO 1		/* curl_setup.h: ioctl -> ioctlsocket (curl-dos.c) */
#define HAVE_STRUCT_SOCKADDR_STORAGE 1
#define HAVE_SOCKADDR_IN6_SIN6_SCOPE_ID 1
#define HAVE_SA_FAMILY_T 1

/* DJGPP's C library */
#define HAVE_BOOL_T 1
#define HAVE_STDBOOL_H 1
#define HAVE_DIRENT_H 1
#define HAVE_OPENDIR 1
#define HAVE_FCNTL_H 1
#define HAVE_LOCALE_H 1
#define HAVE_SETLOCALE 1
#define HAVE_SIGNAL 1
#define HAVE_STRCASECMP 1
#define HAVE_STRINGS_H 1
#define HAVE_STRUCT_TIMEVAL 1
#define HAVE_GETTIMEOFDAY 1		/* curl-dos.h: from DOS's date and time */
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_TERMIOS_H 1
#define HAVE_UNISTD_H 1
#define HAVE_UTIME 1
#define HAVE_UTIME_H 1

#define SIZEOF_INT 4
#define SIZEOF_LONG 4
#define SIZEOF_OFF_T 4
#define SIZEOF_CURL_OFF_T 8
#define SIZEOF_CURL_SOCKET_T 4
#define SIZEOF_SIZE_T 4
#define SIZEOF_TIME_T 4

#endif
