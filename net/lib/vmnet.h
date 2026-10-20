/*
   vmdos network tools: IPv6 and dual-stack support on top of mTCP.

   mTCP (third_party/mtcp) gives the packet driver interface, ARP, IPv4,
   ICMP and UDP. This adds:
     NetAddr / Addr     an IPv4 or IPv6 address, parsing and printing
     Cfg                the MTCPCFG file: reading keys, rewriting it
     Ip6                IPv6: link-local and global addresses, neighbor
                        discovery, router discovery (SLAAC, RDNSS), MLD,
                        ICMPv6 echo, UDP over IPv6
     Net                UDP over either family; start and stop of the stack
     Resolv             DNS queries (A, AAAA, PTR, ...) over IPv4 or IPv6

   vmdos is GPL-2.0-or-later; with mTCP (GPL-3.0-or-later) linked in, the
   programs are GPL-3.0-or-later.
*/

#ifndef VMNET_H
#define VMNET_H

#include CFG_H
#include "types.h"

#pragma pack(push, 1)

struct NetAddr {
  uint8_t family;                  // 0 none, 4, 6
  uint8_t a[16];                   // IPv4 in a[0..3]
};

#pragma pack(pop)

class Addr {
  public:
    static bool  parse( const char *s, NetAddr *out );            // 1.2.3.4, 2001:db8::1, fe80::1%1
    static bool  parse6( const char *s, uint8_t *out );
    static bool  parsePrefix6( const char *s, uint8_t *out, uint8_t *len );  // addr[/len] (len 64 if none)
    static char *format( const NetAddr *a, char *buf );           // buf: 46 bytes
    static char *format6( const uint8_t *a, char *buf );
    static char *format4( const uint8_t *a, char *buf );
    static bool  isZero6( const uint8_t *a );
    static bool  same6( const uint8_t *a, const uint8_t *b );
    static bool  isLinkLocal6( const uint8_t *a ) { return a[0] == 0xfe && (a[1] & 0xc0) == 0x80; }
    static bool  isMulticast6( const uint8_t *a ) { return a[0] == 0xff; }
    static bool  prefixMatch( const uint8_t *a, const uint8_t *b, uint8_t bits );
};


// The configuration file named by MTCPCFG (shared with mTCP's own programs).

#define CFG_VAL_LEN 80

class Cfg {
  public:
    static const char *fileName( void );                           // NULL if MTCPCFG isn't set
    static bool get( const char *key, char *val, uint16_t len );   // false if not there
    // Rewrite the file: lines whose key is in drop[] (NULL-terminated) go,
    // then the lines in add[] (NULL-terminated, "KEY value") are appended.
    static int8_t update( const char * const *drop, const char * const *add );
};


// ---------------------------------------------------------------------------
// IPv6

#define IP6_NC_ENTRIES 8

typedef void (*Ip6EchoCb)( const uint8_t *src, const uint8_t *icmp, uint16_t len, uint8_t hopLimit );
typedef void (*Ip6RaCb)( const uint8_t *router, const uint8_t *icmp, uint16_t len );

struct Ip6Prefix {
  uint8_t  prefix[16];
  uint8_t  len;
  uint8_t  flags;                  // 0x80 on-link, 0x40 autonomous
  uint32_t valid, preferred;
};

class Ip6 {
  public:
    // Set up after Utils::initStack: reads IPV6, IPV6ADDR, IPV6GATEWAY,
    // IPV6NAMESERVER, registers EtherType 86DD, receives multicast.
    // Returns -1 if IPv6 is turned off (IPV6 OFF) or can't start.
    static int8_t init( void );
    static void   stop( void );
    static bool   enabled;

    static uint8_t linkLocal[16];
    static uint8_t global[16];      // all zero: none
    static uint8_t globalLen;
    static uint8_t router[16];      // all zero: none known
    static EthAddr_t routerMac;
    static bool    routerMacKnown;
    static uint8_t nameServer[16];  // all zero: none
    static uint16_t mtu;

    // Router discovery (RA): what the last advertisement said.
    static bool      raSeen;
    static uint8_t   raFlags;       // 0x80 managed (DHCPv6 address), 0x40 other (DHCPv6 info)
    static uint16_t  raLifetime;
    static Ip6Prefix raPrefix[4];
    static uint8_t   raPrefixes;
    static uint8_t   raDns[2][16];
    static uint8_t   raDnsCount;
    static Ip6RaCb   raCallback;

    // Duplicate address detection: set tentative, send DAD, check dadFailed.
    static uint8_t tentative[16];
    static bool    dadFailed;
    static void    sendDad( void );
    static void    sendRouterSolicit( void );
    static void    joinGroups( void );         // MLDv2 report for our solicited-node groups
    static bool    discoverRouter( uint16_t ms );   // RS, wait for an RA

    static void makeEui64( const uint8_t *prefix, uint8_t *out );

    static void process( uint8_t *packet, uint16_t len );

    // Next hop resolution: 0 known (mac filled), 1 pending (an NS went
    // out; call again), -1 no route.
    static int8_t resolve( const uint8_t *dest, EthAddr_t mac );
    static int8_t resolveWait( const uint8_t *dest, EthAddr_t mac, uint16_t ms );

    // Build in Ip6::txPayload( ), then send. nh 58 (ICMPv6) and 17 (UDP)
    // get their checksums filled in. Returns 0 sent, 1 neighbor pending,
    // -1 no route.
    static uint8_t *txPayload( void );
    static int8_t   send( const uint8_t *dest, uint8_t nh, uint16_t len, uint8_t hopLimit );
    static const uint8_t *sourceFor( const uint8_t *dest );

    static Ip6EchoCb echoCallback;  // echo replies and ICMPv6 errors

    static uint16_t checksum( const uint8_t *src, const uint8_t *dst, uint8_t nh,
                              const uint8_t *data, uint16_t len );

    static uint32_t packetsIn, packetsOut, badChecksum;
};


// ---------------------------------------------------------------------------
// UDP over IPv4 or IPv6

typedef void (*NetUdpCb)( const NetAddr *src, uint16_t srcPort, uint16_t dstPort,
                          uint8_t *data, uint16_t len );

#define NET_UDP_PORTS 16

class Net {
  public:
    // parseEnv + initStack + Ip6::init. Prints and returns -1 on failure.
    static int8_t start( void __interrupt __far (*ctrlBreak)( ) );
    static void   stop( void );
    static bool   haveIpv4( void );
    static bool   haveIpv6( void );     // a global address, or a link-local peer
    static int8_t listen( uint16_t port, NetUdpCb cb );
    static void   unlisten( uint16_t port );
    // 0 sent, 1 address resolution pending (try again), -1 error
    static int8_t sendUdp( const NetAddr *dst, uint16_t srcPort, uint16_t dstPort,
                           const uint8_t *data, uint16_t len );
    // Waits up to ms for address resolution. 0 sent, -1 not.
    static int8_t sendUdpWait( const NetAddr *dst, uint16_t srcPort, uint16_t dstPort,
                               const uint8_t *data, uint16_t len, uint16_t ms );
    static void   poll( void );          // process received packets, drive ARP
    static void   deliver6( const uint8_t *src, uint16_t srcPort, uint16_t dstPort,
                            uint8_t *data, uint16_t len );
    // Packet driver receive mode (1..6), -1 on error
    static int    getRcvMode( void );
    static int8_t setRcvMode( int mode );
    static volatile uint8_t breakFlag;
};


// ---------------------------------------------------------------------------
// DNS

#define DNS_TYPE_A      1
#define DNS_TYPE_NS     2
#define DNS_TYPE_CNAME  5
#define DNS_TYPE_SOA    6
#define DNS_TYPE_PTR   12
#define DNS_TYPE_MX    15
#define DNS_TYPE_TXT   16
#define DNS_TYPE_AAAA  28
#define DNS_TYPE_ANY  255

#define RESOLV_MAX_RR   16
#define RESOLV_NAME_LEN 128
#define RESOLV_DATA_LEN 128

struct ResolvRR {
  char     name[RESOLV_NAME_LEN];
  uint16_t type;
  uint32_t ttl;
  uint8_t  section;                // 1 answer, 2 authority, 3 additional
  uint8_t  pad;
  uint8_t  raw[16];                // A / AAAA
  char     text[RESOLV_DATA_LEN];  // printable rdata
};

struct ResolvResult {
  int8_t   rcode;                  // DNS rcode; -1 timeout, -2 no server, -3 bad reply
  bool     authoritative;
  bool     truncated;
  uint8_t  count;
  ResolvRR rr[RESOLV_MAX_RR];
  NetAddr  server;
  uint8_t  pad;
};

class Resolv {
  public:
    static NetAddr servers[3];
    static uint8_t serverCount;
    static char    domain[64];
    static void    loadConfig( void );   // NAMESERVER, IPV6NAMESERVER, DOMAIN (after Net::start)
    static int8_t  query( const NetAddr *server, const char *name, uint16_t qtype, ResolvResult *res );
    // Tries each server. Returns the rcode as query( ).
    static int8_t  queryAny( const char *name, uint16_t qtype, ResolvResult *res );
    // Name or literal to an address. family 0: either (IPv4 first, unless
    // prefer6), 4 or 6 only. 0 on success.
    static int8_t  resolve( const char *name, uint8_t family, bool prefer6, NetAddr *out );
    static void    reverseName( const NetAddr *a, char *out );
    static const char *typeName( uint16_t t );
    static uint16_t typeFromName( const char *s );
};

#endif
