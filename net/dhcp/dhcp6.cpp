/*
   vmdos DHCP: IPv6 autoconfiguration.

     1. Router solicitation (up to 3 tries), read the advertisement:
        prefixes, RDNSS (RFC 8106), the M and O flags.
     2. SLAAC (RFC 4862): an EUI-64 address in each autonomous /64 prefix,
        after duplicate address detection.
     3. DHCPv6 (RFC 8415): Solicit / Advertise / Request / Reply for an
        address when the router sets M (or when there is no router and no
        SLAAC address); Information-request for name servers when it sets
        O and gave none itself.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmnet.h"
#include "dhcp6.h"
#include "eth.h"
#include "timer.h"
#include "utils.h"


uint8_t Dhcp6::addr[16];
uint8_t Dhcp6::prefixLen = 64;
uint8_t Dhcp6::router[16];
uint8_t Dhcp6::dns[16];
char    Dhcp6::how[24] = "";

#define CLIENT_PORT 546
#define SERVER_PORT 547

#define SOLICIT    1
#define ADVERTISE  2
#define REQUEST    3
#define REPLY      7
#define INFO_REQ  11

static const uint8_t allServers[16] = { 0xff,2,0,0,0,0,0,0,0,0,0,0,0,1,0,2 };

static uint8_t  msg[ 300 ];
static uint8_t  in[ 600 ];
static uint16_t inLen;
static uint8_t  wantType;
static uint8_t  xid[3];
static bool     got;


static void handler( const NetAddr *src, uint16_t srcPort, uint16_t dstPort, uint8_t *data, uint16_t len ) {
  if ( got || len < 4 || data[0] != wantType || memcmp( data + 1, xid, 3 ) ) return;
  if ( len > sizeof(in) ) len = sizeof(in);
  memcpy( in, data, len );
  inLen = len;
  got = true;
}


static uint8_t *opt( uint8_t *p, uint16_t code, uint16_t len ) {
  p[0] = (uint8_t)(code >> 8); p[1] = (uint8_t)code;
  p[2] = (uint8_t)(len >> 8);  p[3] = (uint8_t)len;
  return p + 4;
}


// Common start: type, xid, client id (DUID-LL), elapsed time, ORO.
static uint8_t *begin( uint8_t type ) {
  uint8_t *p = msg;
  p[0] = type;
  memcpy( p + 1, xid, 3 );
  p += 4;
  uint8_t *d = opt( p, 1, 10 );                 // client identifier: DUID-LL, Ethernet
  d[0] = 0; d[1] = 3; d[2] = 0; d[3] = 1; memcpy( d + 4, MyEthAddr, 6 );
  p = d + 10;
  d = opt( p, 8, 2 ); d[0] = d[1] = 0;          // elapsed time
  p = d + 2;
  d = opt( p, 6, 4 ); d[0] = 0; d[1] = 23; d[2] = 0; d[3] = 24;   // ORO: DNS servers, domains
  return d + 4;
}


static uint8_t *iaNa( uint8_t *p ) {
  uint8_t *d = opt( p, 3, 12 );
  memcpy( d, MyEthAddr + 2, 4 );                // IAID
  memset( d + 4, 0, 8 );                        // T1, T2: server's choice
  return d + 12;
}


// Sends msg[0..len) up to tries times, waiting for a reply of type want.
static bool exchange( uint16_t len, uint8_t want, int tries ) {
  NetAddr dst;
  dst.family = 6;
  memcpy( dst.a, allServers, 16 );
  wantType = want;
  got = false;
  uint32_t wait = 1000;
  for ( int i=0; i < tries && !got && !Net::breakFlag; i++ ) {
    Net::sendUdp( &dst, CLIENT_PORT, SERVER_PORT, msg, len );
    clockTicks_t start = TIMER_GET_CURRENT( );
    while ( !got && !Net::breakFlag && Timer_diff( start, TIMER_GET_CURRENT( ) ) < TIMER_MS_TO_TICKS( wait ) ) {
      Net::poll( );
      if ( biosIsKeyReady( ) && (biosKeyRead( ) & 0xff) == 27 ) Net::breakFlag = 1;
    }
    wait *= 2;
  }
  return got;
}


// Finds option code at the top level of in[] (from off), returns its data.
static uint8_t *findOpt( uint8_t *buf, uint16_t len, uint16_t code, uint16_t *olen ) {
  uint16_t off = 0;
  while ( off + 4 <= len ) {
    uint16_t c = (buf[off] << 8) | buf[off+1];
    uint16_t l = (buf[off+2] << 8) | buf[off+3];
    if ( off + 4 + l > len ) break;
    if ( c == code ) { *olen = l; return buf + off + 4; }
    off += 4 + l;
  }
  return NULL;
}


static uint16_t statusOf( uint8_t *buf, uint16_t len ) {
  uint16_t l;
  uint8_t *s = findOpt( buf, len, 13, &l );
  if ( s && l >= 2 ) return (s[0] << 8) | s[1];
  return 0;
}


static void takeDns( void ) {
  uint16_t l;
  uint8_t *d = findOpt( in + 4, inLen - 4, 23, &l );
  if ( d && l >= 16 && Addr::isZero6( Dhcp6::dns ) ) memcpy( Dhcp6::dns, d, 16 );
}


static bool takeAddress( void ) {
  uint16_t l, al;
  uint8_t *ia = findOpt( in + 4, inLen - 4, 3, &l );
  if ( !ia || l < 12 ) return false;
  if ( statusOf( ia + 12, l - 12 ) ) return false;
  uint8_t *a = findOpt( ia + 12, l - 12, 5, &al );
  if ( !a || al < 24 ) return false;
  memcpy( Dhcp6::addr, a, 16 );
  // On-link prefix length from the router, if it covers the address.
  Dhcp6::prefixLen = 128;
  for ( int i=0; i < Ip6::raPrefixes; i++ )
    if ( (Ip6::raPrefix[i].flags & 0x80) && Addr::prefixMatch( Dhcp6::addr, Ip6::raPrefix[i].prefix, Ip6::raPrefix[i].len ) )
      Dhcp6::prefixLen = Ip6::raPrefix[i].len;
  return true;
}


static bool statefulDhcp( void ) {
  char buf[50];
  for ( int i=0; i < 3; i++ ) xid[i] = (uint8_t)rand( );
  printf( "IPv6: DHCPv6 solicit ... " );
  uint8_t *p = iaNa( begin( SOLICIT ) );
  if ( !exchange( (uint16_t)(p - msg), ADVERTISE, 3 ) ) { puts( "no server" ); return false; }
  if ( statusOf( in + 4, inLen - 4 ) ) { puts( "server has no addresses" ); return false; }

  // Request: server id and the IA_NA from the advertisement.
  uint16_t sl, il;
  uint8_t *sid = findOpt( in + 4, inLen - 4, 2, &sl );
  uint8_t *ia = findOpt( in + 4, inLen - 4, 3, &il );
  if ( !sid || !ia || sl > 128 || il > 128 ) { puts( "bad advertisement" ); return false; }
  uint8_t sidCopy[128], iaCopy[128];
  memcpy( sidCopy, sid, sl ); memcpy( iaCopy, ia, il );
  takeDns( );

  for ( int i=0; i < 3; i++ ) xid[i] = (uint8_t)rand( );
  p = begin( REQUEST );
  memcpy( opt( p, 2, sl ), sidCopy, sl ); p += 4 + sl;
  memcpy( opt( p, 3, il ), iaCopy, il ); p += 4 + il;
  if ( !exchange( (uint16_t)(p - msg), REPLY, 4 ) ) { puts( "no reply to the request" ); return false; }
  takeDns( );
  if ( !takeAddress( ) ) { puts( "no address in the reply" ); return false; }
  printf( "%s\n", Addr::format6( Dhcp6::addr, buf ) );
  strcpy( Dhcp6::how, "DHCPv6" );
  return true;
}


static void infoRequest( int tries ) {
  for ( int i=0; i < 3; i++ ) xid[i] = (uint8_t)rand( );
  printf( "IPv6: DHCPv6 information request ... " );
  uint8_t *p = begin( INFO_REQ );
  if ( exchange( (uint16_t)(p - msg), REPLY, tries ) ) { takeDns( ); puts( Addr::isZero6( Dhcp6::dns ) ? "no name server" : "ok" ); }
  else puts( "no server" );
}


// Duplicate address detection for a, 1 s. True if nobody else has it.
static bool dad( const uint8_t *a ) {
  memcpy( Ip6::tentative, a, 16 );
  Ip6::joinGroups( );
  Ip6::sendDad( );
  clockTicks_t start = TIMER_GET_CURRENT( );
  while ( !Ip6::dadFailed && Timer_diff( start, TIMER_GET_CURRENT( ) ) < TIMER_MS_TO_TICKS( 1000ul ) ) Net::poll( );
  bool ok = !Ip6::dadFailed;
  memset( Ip6::tentative, 0, 16 );
  return ok;
}


int8_t Dhcp6::run( void ) {
  char buf[50];
  memset( addr, 0, 16 ); memset( router, 0, 16 ); memset( dns, 0, 16 );
  how[0] = 0;
  Net::listen( CLIENT_PORT, handler );

  printf( "IPv6: link-local %s\n", Addr::format6( Ip6::linkLocal, buf ) );
  if ( !dad( Ip6::linkLocal ) ) printf( "IPv6: warning, another machine uses %s\n", buf );

  printf( "IPv6: looking for a router ... " );
  bool ra = false;
  memset( Ip6::router, 0, 16 );
  for ( int i=0; i < 3 && !ra && !Net::breakFlag; i++ ) ra = Ip6::discoverRouter( 1500 );
  if ( Net::breakFlag ) { puts( "aborted" ); return -1; }

  if ( ra ) {
    printf( "%s%s%s\n", Addr::format6( Ip6::router, buf ),
            (Ip6::raFlags & 0x80) ? ", managed (DHCPv6)" : "", (Ip6::raFlags & 0x40) ? ", other config (DHCPv6)" : "" );
    if ( Ip6::raLifetime ) memcpy( router, Ip6::router, 16 );
    if ( Ip6::raDnsCount ) memcpy( dns, Ip6::raDns[0], 16 );
    for ( int i=0; i < Ip6::raPrefixes && Addr::isZero6( addr ); i++ ) {
      Ip6Prefix *pr = &Ip6::raPrefix[i];
      if ( !(pr->flags & 0x40) || pr->len != 64 || pr->valid == 0 ) continue;
      uint8_t a[16];
      Ip6::makeEui64( pr->prefix, a );
      printf( "IPv6: SLAAC %s/64 ... ", Addr::format6( a, buf ) );
      if ( dad( a ) ) {
        puts( "ok" );
        memcpy( addr, a, 16 );
        prefixLen = 64;
        strcpy( how, "SLAAC" );
      } else puts( "in use by another machine" );
    }
  } else puts( "none answered" );

  if ( (ra && (Ip6::raFlags & 0x80)) || (!ra && Addr::isZero6( addr )) ) {
    uint8_t slaac[16];
    uint8_t slaacLen = prefixLen;
    memcpy( slaac, addr, 16 );
    if ( statefulDhcp( ) ) {
      if ( !dad( addr ) ) { puts( "IPv6: that address is in use by another machine" ); memcpy( addr, slaac, 16 ); prefixLen = slaacLen; }
    } else { memcpy( addr, slaac, 16 ); prefixLen = slaacLen; }
  }
  // Name servers: the RA's RDNSS, else DHCPv6 (asked for by the O flag, but
  // tried anyway: some networks only hand them out that way).
  if ( Addr::isZero6( dns ) && !Net::breakFlag ) infoRequest( (ra && (Ip6::raFlags & 0x40)) ? 3 : 2 );
  Net::unlisten( CLIENT_PORT );
  if ( Net::breakFlag ) return -1;

  puts( "" );
  if ( !Addr::isZero6( addr ) ) printf( "IPV6ADDR %s/%u (%s)\n", Addr::format6( addr, buf ), prefixLen, how );
  else puts( "IPv6: no global address (link-local only)" );
  if ( !Addr::isZero6( router ) ) printf( "IPV6GATEWAY %s\n", Addr::format6( router, buf ) );
  if ( !Addr::isZero6( dns ) ) printf( "IPV6NAMESERVER %s\n", Addr::format6( dns, buf ) );
  return Addr::isZero6( addr ) ? 1 : 0;
}


int8_t Dhcp6::writeCfg( void ) {
  static const char * const drop[] = { "IPV6ADDR", "IPV6GATEWAY", "IPV6NAMESERVER", NULL };
  char l1[64], l2[64], l3[64], buf[50];
  const char *add[4];
  int n = 0;
  if ( !Addr::isZero6( addr ) ) { sprintf( l1, "IPV6ADDR %s/%u", Addr::format6( addr, buf ), prefixLen ); add[n++] = l1; }
  if ( !Addr::isZero6( router ) ) { sprintf( l2, "IPV6GATEWAY %s", Addr::format6( router, buf ) ); add[n++] = l2; }
  if ( !Addr::isZero6( dns ) ) { sprintf( l3, "IPV6NAMESERVER %s", Addr::format6( dns, buf ) ); add[n++] = l3; }
  add[n] = NULL;
  return Cfg::update( drop, add );
}
