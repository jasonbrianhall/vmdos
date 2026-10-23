/* 86Box's machine.h, as much as the video code asks of it: a PCI machine. */
#include <voodoo_shim.h>
#ifndef VMDOS_MACHINE_H
#define VMDOS_MACHINE_H
typedef struct { uint32_t bus_flags; } machine_t;
extern machine_t machines[];
extern int machine;
#define MACHINE_BUS_MCA   0x01
#define MACHINE_BUS_MCA32 0x02
#define MACHINE_BUS_PCI   0x04
#define MACHINE_BUS_AGP   0x08
static inline int machine_has_bus(int m, int b) { (void)m; return b == MACHINE_BUS_PCI; }
extern double isa_timing;
extern int suppress_overscan;
#endif
