/* mbedTLS for CURL.EXE (MBEDTLS_USER_CONFIG_FILE): WGET's configuration
   (third_party/wget/wget_mbedtls_config.h), and stdio's buffering left on.
   mbedTLS turns it off to read CA files and keys; DJGPP then reads them a
   byte at a time, a DOS call each, and curl loads its CA bundle after
   connecting: servers gave up waiting (as WGET's did, before it read the
   bundle itself). */
#include "wget_mbedtls_config.h"
#define MBEDTLS_PLATFORM_SETBUF_MACRO(stream, buf) ((void)(stream), (void)(buf))
