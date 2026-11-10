/* Little-endian accessors (x86 only: DOS and the Linux build host). */
#ifndef COMPAT_ENDIAN_H
#define COMPAT_ENDIAN_H
#include <stdint.h>
static inline uint16_t le16dec(const void *p) { const uint8_t *b = p; return b[0] | b[1] << 8; }
static inline uint32_t le32dec(const void *p) { const uint8_t *b = p; return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static inline void le16enc(void *p, uint16_t v) { uint8_t *b = p; b[0] = v; b[1] = v >> 8; }
static inline void le32enc(void *p, uint32_t v) { uint8_t *b = p; b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
#endif
