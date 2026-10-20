/*
   vmdos network tools: IPv6 for mTCP.

   One interface, one link-local address (EUI-64 from the MAC), at most one
   global address (configured, SLAAC or DHCPv6), a default router. Handles
   neighbor discovery (RFC 4861: NS/NA, RS/RA), duplicate address detection,
   router advertisements with prefix, MTU and RDNSS (RFC 8106) options,
   MLDv2 reports for the solicited-node groups, ICMPv6 echo (answering
   requests, passing replies and errors to a callback) and UDP.

   The packet driver has no multicast lists, so this puts it in receive
   mode 5 (all multicast) while running and back afterwards.
*/

#include <dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmnet.h"
#include "eth.h"
#include "packet.h"
#include "timer.h"
#include "utils.h"
#include "trace.h"


bool      Ip6::enabled = false;
uint8_t   Ip6::linkLocal[16];
uint8_t   Ip6::global[16];
uint8_t   Ip6::globalLen = 64;
uint8_t   Ip6::router[16];
EthAddr_t Ip6::routerMac;
bool      Ip6::routerMacKnown = false;
uint8_t   Ip6::nameServer[16];
uint16_t  Ip6::mtu = 1500;

bool      Ip6::raSeen = false;
uint8_t   Ip6::raFlags = 0;
uint16_t  Ip6::raLifetime = 0;
Ip6Prefix Ip6::raPrefix[4];
uint8_t   Ip6::raPrefixes = 0;
uint8_t   Ip6::raDns[2][16];
uint8_t   Ip6::raDnsCount = 0;
Ip6RaCb   Ip6::raCallback = NULL;

uint8_t   Ip6::tentative[16];
bool      Ip6::dadFailed = false;

Ip6EchoCb Ip6::echoCallback = NULL;

uint32_t  Ip6::packetsIn = 0, Ip6::packetsOut = 0, Ip6::badChecksum = 0;

static int oldRcvMode = -1;

#define ETH_HLEN   14
#define IP6_HLEN   40
#define HDR        (ETH_HLEN + IP6_HLEN)

#pragma pack(push, 1)
struct Ip6Hdr {
  uint8_t  vtf[4];
  uint16_t plen;
  uint8_t  nh;
  uint8_t  hlim;
  uint8_t  src[16];
  uint8_t  dst[16];
};
#pragma pack(pop)

static uint8_t txbuf[ HDR + 1500 ];
static uint8_t ndbuf[ HDR + 96 ];          // NS / NA / RS / MLD, so txbuf survives

static const uint8_t allNodes[16]   = { 0xff,2,0,0,0,0,0,0,0,0,0,0,0,0,0,1 };
static const uint8_t allRouters[16] = { 0xff,2,0,0,0,0,0,0,0,0,0,0,0,0,0,2 };
static const uint8_t mldv2Routers[16] = { 0xff,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0x16 };
static const uint8_t unspecified[16] = { 0 };


// ---------------------------------------------------------------------------
// Neighbor cache

struct NcEntry {
  uint8_t      ip[16];
  EthAddr_t    mac;
  uint8_t      state;                // 0 free, 1 incomplete, 2 reachable
  uint8_t      pad;
  clockTicks_t when;
};

static NcEntry nc[ IP6_NC_ENTRIES ];

static NcEntry *ncFind( const uint8_t *ip ) {
  for ( int i=0; i < IP6_NC_ENTRIES; i++ )
    if ( nc[i].state && Addr::same6( nc[i].ip, ip ) ) return &nc[i];
  return NULL;
}

static NcEntry *ncNew( const uint8_t *ip ) {
  NcEntry *e = ncFind( ip );
  if ( e ) return e;
  NcEntry *old = &nc[0];
  for ( int i=0; i < IP6_NC_ENTRIES; i++ ) {
    if ( !nc[i].state ) { old = &nc[i]; break; }
    if ( nc[i].when < old->when ) old = &nc[i];
  }
  memcpy( old->ip, ip, 16 );
  old->state = 1;
  old->when = TIMER_GET_CURRENT( );
  return old;
}

static void ncLearn( const uint8_t *ip, const uint8_t *mac, bool create ) {
  if ( Addr::isZero6( ip ) || Addr::isMulticast6( ip ) ) return;
  NcEntry *e = create ? ncNew( ip ) : ncFind( ip );
  if ( !e ) return;
  memcpy( e->mac, mac, 6 );
  e->state = 2;
  e->when = TIMER_GET_CURRENT( );
  if ( Addr::same6( ip, Ip6::router ) ) { memcpy( Ip6::routerMac, mac, 6 ); Ip6::routerMacKnown = true; }
}


// ---------------------------------------------------------------------------
// Helpers

uint16_t Ip6::checksum( const uint8_t *src, const uint8_t *dst, uint8_t nh,
                        const uint8_t *data, uint16_t len ) {
  uint32_t sum = 0;
  const uint16_t *w;
  int i;
  w = (const uint16_t *)src; for ( i=0; i < 8; i++ ) sum += w[i];
  w = (const uint16_t *)dst; for ( i=0; i < 8; i++ ) sum += w[i];
  sum += htons( len );
  sum += htons( (uint16_t)nh );
  w = (const uint16_t *)data;
  uint16_t n = len;
  while ( n > 1 ) { sum += *w++; n -= 2; }
  if ( n ) sum += *(const uint8_t *)w;
  while ( sum >> 16 ) sum = (sum & 0xffff) + (sum >> 16);
  return (uint16_t)~sum;
}


void Ip6::makeEui64( const uint8_t *prefix, uint8_t *out ) {
  memcpy( out, prefix, 8 );
  out[8]  = MyEthAddr[0] ^ 0x02;
  out[9]  = MyEthAddr[1];
  out[10] = MyEthAddr[2];
  out[11] = 0xff;
  out[12] = 0xfe;
  out[13] = MyEthAddr[3];
  out[14] = MyEthAddr[4];
  out[15] = MyEthAddr[5];
}


static void solicitedNode( const uint8_t *ip, uint8_t *out ) {
  static const uint8_t sn[13] = { 0xff,2,0,0,0,0,0,0,0,0,0,1,0xff };
  memcpy( out, sn, 13 );
  out[13] = ip[13]; out[14] = ip[14]; out[15] = ip[15];
}


static void multicastMac( const uint8_t *ip, uint8_t *mac ) {
  mac[0] = 0x33; mac[1] = 0x33;
  mac[2] = ip[12]; mac[3] = ip[13]; mac[4] = ip[14]; mac[5] = ip[15];
}


static bool isMine( const uint8_t *a ) {
  if ( Addr::same6( a, Ip6::linkLocal ) ) return true;
  if ( !Addr::isZero6( Ip6::global ) && Addr::same6( a, Ip6::global ) ) return true;
  return false;
}


static bool acceptDest( const uint8_t *d ) {
  if ( isMine( d ) ) return true;
  if ( Addr::same6( d, allNodes ) ) return true;
  if ( d[0] == 0xff && d[1] == 0x02 && d[11] == 1 && d[12] == 0xff ) {
    // Solicited-node: any of ours (also the tentative one, for DAD)
    uint8_t sn[16];
    solicitedNode( Ip6::linkLocal, sn ); if ( Addr::same6( d, sn ) ) return true;
    if ( !Addr::isZero6( Ip6::global ) ) { solicitedNode( Ip6::global, sn ); if ( Addr::same6( d, sn ) ) return true; }
    if ( !Addr::isZero6( Ip6::tentative ) ) { solicitedNode( Ip6::tentative, sn ); if ( Addr::same6( d, sn ) ) return true; }
  }
  return false;
}


static bool onLink( const uint8_t *d ) {
  if ( Addr::isLinkLocal6( d ) || Addr::isMulticast6( d ) ) return true;
  for ( int i=0; i < Ip6::raPrefixes; i++ )
    if ( (Ip6::raPrefix[i].flags & 0x80) && Addr::prefixMatch( d, Ip6::raPrefix[i].prefix, Ip6::raPrefix[i].len ) ) return true;
  if ( !Addr::isZero6( Ip6::global ) && Ip6::globalLen < 128 && Addr::prefixMatch( d, Ip6::global, Ip6::globalLen ) ) return true;
  return false;
}


const uint8_t *Ip6::sourceFor( const uint8_t *d ) {
  if ( Addr::isLinkLocal6( d ) || (d[0] == 0xff && (d[1] & 0x0f) <= 2) ) return linkLocal;
  if ( !Addr::isZero6( global ) ) return global;
  return linkLocal;
}


static void fillHeader( uint8_t *buf, const uint8_t *dmac, const uint8_t *src,
                        const uint8_t *dst, uint8_t nh, uint16_t len, uint8_t hlim ) {
  EthHeader *eh = (EthHeader *)buf;
  eh->setDest( dmac );
  eh->setSrc( MyEthAddr );
  eh->setType( 0x86dd );
  Ip6Hdr *ip = (Ip6Hdr *)(buf + ETH_HLEN);
  ip->vtf[0] = 0x60; ip->vtf[1] = 0; ip->vtf[2] = 0; ip->vtf[3] = 0;
  ip->plen = htons( len );
  ip->nh = nh;
  ip->hlim = hlim;
  memcpy( ip->src, src, 16 );
  memcpy( ip->dst, dst, 16 );
}


static void sendFrame( uint8_t *buf, uint16_t len ) {
  if ( len < 60 ) { memset( buf + len, 0, 60 - len ); len = 60; }
  Packet_send_pkt( buf, len );
  Ip6::packetsOut++;
}


// ICMPv6 in ndbuf: payload at ndbuf+HDR of len bytes, checksum filled here.
static void sendNd( const uint8_t *dmac, const uint8_t *src, const uint8_t *dst, uint16_t len ) {
  fillHeader( ndbuf, dmac, src, dst, 58, len, 255 );
  uint8_t *icmp = ndbuf + HDR;
  icmp[2] = icmp[3] = 0;
  *(uint16_t *)(icmp + 2) = Ip6::checksum( src, dst, 58, icmp, len );
  sendFrame( ndbuf, HDR + len );
}


static void sendNs( const uint8_t *target, const uint8_t *src, bool dad ) {
  uint8_t dst[16], mac[6];
  solicitedNode( target, dst );
  multicastMac( dst, mac );
  uint8_t *p = ndbuf + HDR;
  memset( p, 0, 32 );
  p[0] = 135;
  memcpy( p + 8, target, 16 );
  uint16_t len = 24;
  if ( !dad ) {                                 // source link-layer address option
    p[24] = 1; p[25] = 1; memcpy( p + 26, MyEthAddr, 6 );
    len = 32;
  }
  sendNd( mac, src, dst, len );
}


static void sendNa( const uint8_t *target, const uint8_t *dst, const uint8_t *dmac, bool solicited ) {
  uint8_t *p = ndbuf + HDR;
  memset( p, 0, 32 );
  p[0] = 136;
  p[4] = (solicited ? 0x40 : 0) | 0x20;         // solicited, override
  memcpy( p + 8, target, 16 );
  p[24] = 2; p[25] = 1; memcpy( p + 26, MyEthAddr, 6 );   // target link-layer address
  sendNd( dmac, target, dst, 32 );
}


void Ip6::sendRouterSolicit( void ) {
  uint8_t mac[6];
  multicastMac( allRouters, mac );
  uint8_t *p = ndbuf + HDR;
  memset( p, 0, 16 );
  p[0] = 133;
  p[8] = 1; p[9] = 1; memcpy( p + 10, MyEthAddr, 6 );
  sendNd( mac, linkLocal, allRouters, 16 );
}


void Ip6::sendDad( void ) {
  dadFailed = false;
  sendNs( tentative, unspecified, true );
}


// MLDv2 report (RFC 3810), joining the solicited-node groups: some switches
// only forward multicast to ports that asked for it.
void Ip6::joinGroups( void ) {
  uint8_t groups[3][16];
  int n = 0;
  solicitedNode( linkLocal, groups[n++] );
  if ( !Addr::isZero6( global ) ) solicitedNode( global, groups[n++] );
  if ( !Addr::isZero6( tentative ) && !Addr::same6( tentative, global ) ) solicitedNode( tentative, groups[n++] );

  uint8_t mac[6];
  multicastMac( mldv2Routers, mac );
  uint8_t *p = ndbuf + HDR;
  // Hop-by-hop: router alert
  p[0] = 58; p[1] = 0; p[2] = 5; p[3] = 2; p[4] = 0; p[5] = 0; p[6] = 1; p[7] = 0;
  uint8_t *m = p + 8;
  memset( m, 0, 8 );
  m[0] = 143;
  m[7] = (uint8_t)n;
  for ( int i=0; i < n; i++ ) {
    uint8_t *r = m + 8 + i * 20;
    r[0] = 4; r[1] = 0; r[2] = 0; r[3] = 0;     // CHANGE_TO_EXCLUDE, no sources: join
    memcpy( r + 4, groups[i], 16 );
  }
  uint16_t mlen = 8 + n * 20;
  const uint8_t *src = linkLocal;
  fillHeader( ndbuf, mac, src, mldv2Routers, 0, 8 + mlen, 1 );
  *(uint16_t *)(m + 2) = checksum( src, mldv2Routers, 58, m, mlen );
  sendFrame( ndbuf, HDR + 8 + mlen );
}


// ---------------------------------------------------------------------------
// Receive

static void handleRa( const Ip6Hdr *ip, const uint8_t *eth, uint8_t *p, uint16_t len ) {
  if ( ip->hlim != 255 || !Addr::isLinkLocal6( ip->src ) || len < 16 ) return;
  Ip6::raSeen = true;
  Ip6::raFlags = p[5];
  Ip6::raLifetime = ntohs( *(uint16_t *)(p + 6) );
  if ( Ip6::raLifetime ) {
    if ( Addr::isZero6( Ip6::router ) || Addr::same6( Ip6::router, ip->src ) ) {
      memcpy( Ip6::router, ip->src, 16 );
      memcpy( Ip6::routerMac, eth + 6, 6 );
      Ip6::routerMacKnown = true;
    }
  }
  ncLearn( ip->src, eth + 6, true );
  Ip6::raPrefixes = 0;
  Ip6::raDnsCount = 0;
  uint16_t off = 16;
  while ( off + 8 <= len ) {
    uint8_t type = p[off], olen = p[off+1];
    if ( olen == 0 ) break;
    uint16_t bytes = olen * 8;
    if ( off + bytes > len ) break;
    uint8_t *o = p + off;
    if ( type == 3 && olen == 4 && Ip6::raPrefixes < 4 ) {
      Ip6Prefix *pr = &Ip6::raPrefix[ Ip6::raPrefixes++ ];
      pr->len = o[2];
      pr->flags = o[3];
      pr->valid = ntohl( *(uint32_t *)(o + 4) );
      pr->preferred = ntohl( *(uint32_t *)(o + 8) );
      memcpy( pr->prefix, o + 16, 16 );
    } else if ( type == 5 && olen == 1 ) {
      uint32_t m = ntohl( *(uint32_t *)(o + 4) );
      if ( m >= 1280 && m <= 1500 ) Ip6::mtu = (uint16_t)m;
    } else if ( type == 25 && olen >= 3 ) {           // RDNSS
      for ( uint16_t a = 8; a + 16 <= bytes && Ip6::raDnsCount < 2; a += 16 )
        memcpy( Ip6::raDns[ Ip6::raDnsCount++ ], o + a, 16 );
    }
    off += bytes;
  }
  if ( Ip6::raCallback ) Ip6::raCallback( ip->src, p, len );
}


static void handleIcmp( Ip6Hdr *ip, uint8_t *eth, uint8_t *p, uint16_t len ) {
  if ( len < 4 ) return;
  if ( Ip6::checksum( ip->src, ip->dst, 58, p, len ) != 0 ) { Ip6::badChecksum++; return; }
  uint8_t type = p[0];

  switch ( type ) {

    case 128: {                                   // echo request
      // Answered in the received buffer (so an application's txbuf is left
      // alone), straight to the sender's MAC: no resolution needed.
      if ( p != eth + HDR ) return;               // had extension headers
      uint8_t src[16], dst[16], mac[6];
      memcpy( src, isMine( ip->dst ) ? ip->dst : Ip6::sourceFor( ip->src ), 16 );
      memcpy( dst, ip->src, 16 );
      memcpy( mac, eth + 6, 6 );
      ncLearn( dst, mac, false );
      p[0] = 129;
      fillHeader( eth, mac, src, dst, 58, len, 64 );
      p[2] = p[3] = 0;
      *(uint16_t *)(p + 2) = Ip6::checksum( src, dst, 58, p, len );
      sendFrame( eth, HDR + len );
      break;
    }

    case 129: case 1: case 2: case 3: case 4:
      if ( Ip6::echoCallback ) Ip6::echoCallback( ip->src, p, len, ip->hlim );
      break;

    case 134:
      handleRa( ip, eth, p, len );
      break;

    case 135: {                                   // neighbor solicitation
      if ( ip->hlim != 255 || len < 24 ) return;
      uint8_t *target = p + 8;
      if ( !Addr::isZero6( Ip6::tentative ) && Addr::same6( target, Ip6::tentative ) ) {
        if ( Addr::isZero6( ip->src ) ) Ip6::dadFailed = true;   // someone else's DAD for it
        return;
      }
      if ( !isMine( target ) ) return;
      if ( Addr::isZero6( ip->src ) ) {           // DAD for our address: defend it
        uint8_t mac[6];
        multicastMac( allNodes, mac );
        sendNa( target, allNodes, mac, false );
        return;
      }
      // Source link-layer address option
      for ( uint16_t off = 24; off + 8 <= len; ) {
        if ( p[off+1] == 0 ) break;
        if ( p[off] == 1 ) ncLearn( ip->src, p + off + 2, true );
        off += p[off+1] * 8;
      }
      sendNa( target, ip->src, eth + 6, true );
      break;
    }

    case 136: {                                   // neighbor advertisement
      if ( ip->hlim != 255 || len < 24 ) return;
      uint8_t *target = p + 8;
      if ( !Addr::isZero6( Ip6::tentative ) && Addr::same6( target, Ip6::tentative ) ) {
        Ip6::dadFailed = true;
        return;
      }
      const uint8_t *mac = eth + 6;
      for ( uint16_t off = 24; off + 8 <= len; ) {
        if ( p[off+1] == 0 ) break;
        if ( p[off] == 2 ) mac = p + off + 2;
        off += p[off+1] * 8;
      }
      ncLearn( target, mac, false );
      break;
    }
  }
}


void Ip6::process( uint8_t *packet, uint16_t len ) {
  packetsIn++;
  Ip6Hdr *ip = (Ip6Hdr *)(packet + ETH_HLEN);
  if ( len < HDR || (ip->vtf[0] >> 4) != 6 ) { Buffer_free( packet ); return; }
  uint16_t plen = ntohs( ip->plen );
  if ( plen > len - HDR ) { Buffer_free( packet ); return; }
  if ( !acceptDest( ip->dst ) || Eth::isSame( packet + 6, MyEthAddr ) ) { Buffer_free( packet ); return; }

  uint8_t nh = ip->nh;
  uint8_t *p = packet + HDR;
  // Skip hop-by-hop / routing / destination options headers.
  while ( (nh == 0 || nh == 43 || nh == 60) && plen >= 8 ) {
    uint16_t hl = (p[1] + 1) * 8;
    if ( hl > plen ) { Buffer_free( packet ); return; }
    nh = p[0];
    p += hl; plen -= hl;
  }

  if ( nh == 58 ) {
    handleIcmp( ip, packet, p, plen );
  }
  else if ( nh == 17 && plen >= 8 ) {
    uint16_t ulen = ntohs( *(uint16_t *)(p + 4) );
    if ( ulen >= 8 && ulen <= plen && checksum( ip->src, ip->dst, 17, p, ulen ) == 0 ) {
      ncLearn( ip->src, packet + 6, false );
      Net::deliver6( ip->src, ntohs( *(uint16_t *)p ), ntohs( *(uint16_t *)(p + 2) ), p + 8, ulen - 8 );
    } else badChecksum++;
  }
  Buffer_free( packet );
}


// ---------------------------------------------------------------------------
// Send

int8_t Ip6::resolve( const uint8_t *dest, EthAddr_t mac ) {
  if ( Addr::isMulticast6( dest ) ) { multicastMac( dest, mac ); return 0; }
  const uint8_t *hop = dest;
  if ( !onLink( dest ) ) {
    if ( Addr::isZero6( router ) ) return -1;
    hop = router;
  }
  NcEntry *e = ncFind( hop );
  if ( e && e->state == 2 ) { memcpy( mac, e->mac, 6 ); return 0; }
  if ( e && Timer_diff( e->when, TIMER_GET_CURRENT( ) ) < TIMER_MS_TO_TICKS( 1000 ) ) return 1;
  e = ncNew( hop );
  e->state = 1;
  e->when = TIMER_GET_CURRENT( );
  sendNs( hop, sourceFor( hop ), false );
  return 1;
}


bool Ip6::discoverRouter( uint16_t ms ) {
  raSeen = false;
  sendRouterSolicit( );
  clockTicks_t start = TIMER_GET_CURRENT( );
  while ( !raSeen && Timer_diff( start, TIMER_GET_CURRENT( ) ) < TIMER_MS_TO_TICKS( (uint32_t)ms ) ) {
    if ( Net::breakFlag ) break;
    Net::poll( );
  }
  return raSeen;
}


int8_t Ip6::resolveWait( const uint8_t *dest, EthAddr_t mac, uint16_t ms ) {
  // Off link with no router known yet: ask for one first.
  if ( !onLink( dest ) && Addr::isZero6( router ) ) discoverRouter( 1500 );
  clockTicks_t start = TIMER_GET_CURRENT( );
  while ( 1 ) {
    int8_t rc = resolve( dest, mac );
    if ( rc <= 0 ) return rc;
    if ( Net::breakFlag ) return 1;
    if ( Timer_diff( start, TIMER_GET_CURRENT( ) ) > TIMER_MS_TO_TICKS( (uint32_t)ms ) ) return 1;
    Net::poll( );
  }
}


uint8_t *Ip6::txPayload( void ) {
  return txbuf + HDR;
}


int8_t Ip6::send( const uint8_t *dest, uint8_t nh, uint16_t len, uint8_t hopLimit ) {
  EthAddr_t mac;
  int8_t rc = resolve( dest, mac );
  if ( rc ) return rc;
  const uint8_t *src = sourceFor( dest );
  fillHeader( txbuf, mac, src, dest, nh, len, hopLimit );
  uint8_t *p = txbuf + HDR;
  if ( nh == 58 ) {
    p[2] = p[3] = 0;
    *(uint16_t *)(p + 2) = checksum( src, dest, 58, p, len );
  } else if ( nh == 17 ) {
    p[6] = p[7] = 0;
    uint16_t c = checksum( src, dest, 17, p, len );
    *(uint16_t *)(p + 6) = c ? c : 0xffff;
  }
  sendFrame( txbuf, HDR + len );
  return 0;
}


// ---------------------------------------------------------------------------
// Start / stop

int8_t Ip6::init( void ) {
  char v[ CFG_VAL_LEN ];
  if ( Cfg::get( "IPV6", v, sizeof(v) ) && (stricmp( v, "OFF" ) == 0 || stricmp( v, "NO" ) == 0 || strcmp( v, "0" ) == 0) )
    return -1;

  static const uint8_t llPrefix[8] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0 };
  makeEui64( llPrefix, linkLocal );
  memset( global, 0, 16 );
  memset( router, 0, 16 );
  memset( nameServer, 0, 16 );
  memset( tentative, 0, 16 );
  memset( nc, 0, sizeof(nc) );

  if ( Cfg::get( "IPV6ADDR", v, sizeof(v) ) && v[0] ) {
    if ( !Addr::parsePrefix6( v, global, &globalLen ) ) {
      fprintf( stderr, "IPV6ADDR: can't read '%s'\n", v );
      memset( global, 0, 16 );
    }
  }
  if ( Cfg::get( "IPV6GATEWAY", v, sizeof(v) ) && v[0] ) {
    if ( !Addr::parse6( v, router ) ) { fprintf( stderr, "IPV6GATEWAY: can't read '%s'\n", v ); memset( router, 0, 16 ); }
  }
  if ( Cfg::get( "IPV6NAMESERVER", v, sizeof(v) ) && v[0] ) {
    if ( !Addr::parse6( v, nameServer ) ) { fprintf( stderr, "IPV6NAMESERVER: can't read '%s'\n", v ); memset( nameServer, 0, 16 ); }
  }

  if ( Packet_registerEtherType( 0x86dd, Ip6::process ) ) {
    fprintf( stderr, "IPv6: no room for its EtherType handler\n" );
    return -1;
  }
  oldRcvMode = Net::getRcvMode( );
  if ( oldRcvMode >= 0 && oldRcvMode < 5 ) Net::setRcvMode( 5 );
  enabled = true;
  joinGroups( );
  return 0;
}


void Ip6::stop( void ) {
  if ( !enabled ) return;
  if ( oldRcvMode >= 0 && oldRcvMode < 5 ) Net::setRcvMode( oldRcvMode );
  enabled = false;
}
