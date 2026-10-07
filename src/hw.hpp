#pragma once
// Glue for the C++ drivers shared with baremetaldoom (usb.cpp, pci.cpp).
extern "C" {
#include "kernel.h"
}
#define printf kprintf
