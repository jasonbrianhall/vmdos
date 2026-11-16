/* mbedTLS for WGET (MBEDTLS_USER_CONFIG_FILE, over its default
   configuration): no sockets or timing of its own (dos-sock.c is the
   network), no x86 AES or PadLock code, entropy from wget.c. */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_AESNI_C
#undef MBEDTLS_AESCE_C
#undef MBEDTLS_PADLOCK_C
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
/* DJGPP has no clock_gettime(): wget.c gives the milliseconds */
#define MBEDTLS_PLATFORM_MS_TIME_ALT
/* WGET -d: mbedTLS's debugging output */
#define MBEDTLS_DEBUG_C
