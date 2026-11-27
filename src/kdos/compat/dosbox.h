/* vmdos kdos: a minimal stand-in for DOSBox-X's dosbox.h, for the DOS
   kernel and shell taken from DOSBox-X (src/kdos). Only what those files
   use; everything about emulated hardware, SDL and the GUI is gone. */
#ifndef DOSBOX_DOSBOX_H
#define DOSBOX_DOSBOX_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <string>
#include <vector>
#include <list>
#include <map>
#include <iosfwd>
#include <algorithm>

#include "config.h"

typedef uintptr_t Bitu;
typedef intptr_t Bits;
typedef uint32_t Bit32u;
typedef int32_t Bit32s;
typedef uint16_t Bit16u;
typedef int16_t Bit16s;
typedef uint8_t Bit8u;
typedef int8_t Bit8s;
typedef uint64_t Bit64u;
typedef int64_t Bit64s;

#define INLINE inline __attribute__((always_inline))
#define GCC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define GCC_LIKELY(x) __builtin_expect(!!(x), 1)
#define GCC_ATTRIBUTE(x) __attribute__((x))

extern "C" void kprintf(const char *fmt, ...);
extern "C" void panic(const char *fmt, ...) __attribute__((noreturn));
[[noreturn]] void E_Exit(const char *fmt, ...);
void kdos_log(const char *fmt, ...);
#define LOG_MSG kdos_log

enum MachineType { MCH_HERC, MCH_CGA, MCH_TANDY, MCH_PCJR, MCH_EGA, MCH_VGA, MCH_AMSTRAD, MCH_PC98, MCH_FM_TOWNS, MCH_MCGA, MCH_MDA, MCH_OLIVETTI, MCH_3270PC };
enum SVGACards { SVGA_None, SVGA_S3Trio, SVGA_TsengET4K, SVGA_TsengET3K, SVGA_ParadisePVGA1A, SVGA_DOSBoxIG };
extern MachineType machine;
extern SVGACards svgaCard;
#define IS_TANDY_ARCH (0)
#define IS_EGAVGA_ARCH (1)
#define IS_VGA_ARCH (1)
#define IS_PC98_ARCH (0)
#define IS_FM_TOWNS (0)
#define IS_J3100 (0)
#define IS_JEGA_ARCH (0)
#define IS_DOSV (0)
#define IS_DOS_JAPANESE (0)
#define IS_DOS_CJK (0)
#define IS_JDOSV (0)
#define IS_PDOSV (0)
#define IS_CDOSV (0)
#define IS_TDOSV (0)
#define IS_KDOSV (0)
#define IS_AX_ARCH (0)
#define IS_DOSV_ARCH (0)
#define TANDY_ARCH_CASE MCH_TANDY: case MCH_PCJR
#define EGAVGA_ARCH_CASE MCH_EGA: case MCH_VGA

extern bool dos_kernel_disabled;

extern bool mainline_compatible_mapping;
extern bool enable_dbcs_tables, enable_filenamechar, enable_share_exe;
extern bool uselfn;
extern int lfn_filefind_handle;

#define MAX_PATH_LEN 512
#ifndef CROSS_LEN
#define CROSS_LEN 512
#endif


/* config: DOSBox-X reads its [dos] and [dosbox] sections; kdos answers
   from a small table of its own (glue.cpp) */
class Section;
class CDROM_Interface;
class Section_prop;
class Property;

void GFX_SetTitle(int32_t, int32_t, int32_t, bool);
static inline bool CPU_IsDynamicCore(void) { return false; }



void MSG_Add(const char *, const char *);
const char *MSG_Get(char const *);
std::string formatString(const char *format, ...);
int ascii_toupper(int c);
extern uint32_t guest_msdos_LoL;
extern uint32_t guest_msdos_dev_chain;
extern int boothax;
enum { BOOTHAX_NONE = 0, BOOTHAX_MSDOS };
extern bool mono_cga;
typedef char utf8_t;
typedef uint16_t utf16_t;
#ifndef UNICODE_BOM
#define UNICODE_BOM 0xFEFF
#endif
typedef unsigned long long bytecount_t;
static inline constexpr bytecount_t operator""_bytes(const bytecount_t x) { return x; }
static inline constexpr bytecount_t operator""_parabytes(const bytecount_t x) { return x << 4u; }
static inline constexpr bytecount_t operator""_kibibytes(const bytecount_t x) { return x << 10u; }
static inline constexpr bytecount_t operator""_pagebytes(const bytecount_t x) { return x << 12u; }
static inline constexpr bytecount_t operator""_mibibytes(const bytecount_t x) { return x << 20u; }
static inline constexpr bytecount_t _bytes(const bytecount_t x) { return x; }
static inline constexpr bytecount_t _parabytes(const bytecount_t x) { return x << 4u; }
static inline constexpr bytecount_t _kibibytes(const bytecount_t x) { return x << 10u; }
static inline constexpr bytecount_t _pagebytes(const bytecount_t x) { return x << 12u; }
static inline constexpr bytecount_t _mibibytes(const bytecount_t x) { return x << 20u; }
#include "kdos_stubs.h"
#endif
