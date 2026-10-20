/*
   vmdos DHCP: IPv6 autoconfiguration (router discovery, SLAAC, DHCPv6).
*/

#ifndef DHCP6_H
#define DHCP6_H

#include "types.h"

class Dhcp6 {
  public:
    // Router discovery, then SLAAC and/or DHCPv6 as the router says.
    // 0: a global address; 1: link-local only (still worth writing:
    // router / name server may be known); -1 user abort.
    static int8_t run( void );
    static int8_t writeCfg( void );

    static uint8_t addr[16];
    static uint8_t prefixLen;
    static uint8_t router[16];
    static uint8_t dns[16];
    static char    how[24];          // "SLAAC", "DHCPv6", ...
};

#endif
