/*
   VMIPXBOX: a bridge between IPX on the LAN and DOSBox's IPX-over-UDP
   ("IPXNET"), so DOS games on real or vmdos PCs (VMIPX, NetWare drivers)
   and DOSBox players share one IPX network.

     VMIPXBOX SERVER [port]         be the IPXNET server: DOSBox players run
                                    IPXNET CONNECT <this PC> [port]; default 213
     VMIPXBOX CONNECT host [port]   join a DOSBox IPXNET server (DOSBox's own
                                    IPXNET STARTSERVER, or another VMIPXBOX)

   IPv4 and IPv6: the server listens on both; CONNECT takes either. DOSBox
   itself only speaks IPv4; IPv6 is for VMIPXBOX to VMIPXBOX links.

   The DOSBox protocol: each IPX packet (30-byte header + data) is one UDP
   datagram. A packet to socket 2 with destination node 0 registers a
   client; the server answers with the client's node in the destination
   (its IPv4 address and UDP port, 6 bytes). The server forwards by the
   destination node, and a destination host of FFFFFFFF is broadcast.

   SERVER: DOSBox clients keep their IP:port nodes and LAN machines their
   MACs, so nothing is rewritten. The card runs promiscuous, as LAN
   machines send to a DOSBox node's "MAC".
   CONNECT: each LAN machine gets its own UDP port and registration, so the
   server sees one client per LAN machine; node addresses in the IPX
   headers are translated (LAN MAC <-> that client's node).

   The PC running this is the bridge, not a player: VMIPX must not be
   loaded on it (it would take the IPX frames).
*/

#include <conio.h>
#include <ctype.h>
#include <dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vmnet.h"
#include "eth.h"
#include "ip.h"
#include "packet.h"
#include "timer.h"
#include "utils.h"


#define IPX_HLEN   30
#define MAX_IPX    1500
#define MAX_PEERS  12             // CONNECT: LAN machines (one UDP port each)
#define MAX_CLIENTS 16            // SERVER: DOSBox clients
#define KEEPALIVE_SECS 20

#pragma pack(push, 1)
struct IpxHdr {
  uint16_t checksum;
  uint16_t length;                // big endian
  uint8_t  tc;
  uint8_t  type;
  uint8_t  dnet[4];
  uint8_t  dnode[6];
  uint16_t dsock;                 // big endian
  uint8_t  snet[4];
  uint8_t  snode[6];
  uint16_t ssock;
};
#pragma pack(pop)

static void __interrupt __far ctrlBreakHandler( ) { Net::breakFlag = 1; }

static const uint8_t bcastNode[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static bool     serverMode;
static uint16_t udpPort = 213;
static NetAddr  remote;            // CONNECT: the server
static bool     verbose = false;
static int      frameType = 0;     // LAN framing to send: 0 Ethernet II, 1 802.3, 2 802.2, 3 SNAP
static bool     frameFixed = false;
static int      oldMode = -1;

static uint32_t lanIn, lanOut, udpIn, udpOut, dropped;
static bool     statusShown = false;

// Before printing an event: end the /V status line.
static void eventLine( void ) {
  if ( statusShown ) { putchar( '\n' ); statusShown = false; }
}

static uint8_t  frame[ 14 + 8 + MAX_IPX + 4 ];


// ---------------------------------------------------------------------------
// LAN side

static void lanSend( const uint8_t *ipx, uint16_t len, const uint8_t *dmac ) {
  if ( len > MAX_IPX ) return;
  EthHeader *eh = (EthHeader *)frame;
  eh->setDest( dmac );
  eh->setSrc( MyEthAddr );
  uint16_t hl = 14;
  switch ( frameType ) {
    case 0: eh->setType( 0x8137 ); break;
    case 1: eh->type = htons( len ); break;
    case 2: eh->type = htons( len + 3 ); frame[14] = 0xe0; frame[15] = 0xe0; frame[16] = 0x03; hl = 17; break;
    case 3: eh->type = htons( len + 8 );
            frame[14] = 0xaa; frame[15] = 0xaa; frame[16] = 0x03; frame[17] = frame[18] = frame[19] = 0;
            frame[20] = 0x81; frame[21] = 0x37; hl = 22; break;
  }
  memcpy( frame + hl, ipx, len );
  uint16_t total = hl + len;
  if ( total < 60 ) { memset( frame + total, 0, 60 - total ); total = 60; }
  Packet_send_pkt( frame, total );
  lanOut++;
}


static void fromLan( uint8_t *ipx, uint16_t len, const uint8_t *smac );

// Ethernet II (8137)
static void lanHandler( uint8_t *packet, uint16_t len ) {
  if ( len > 14 + IPX_HLEN ) {
    if ( !frameFixed ) frameType = 0;
    fromLan( packet + 14, len - 14, packet + 6 );
  }
  Buffer_free( packet );
}

// 802.3 raw, 802.2, SNAP: length in the type field.
static void lanOtherHandler( uint8_t *packet, uint16_t len ) {
  uint16_t t = ntohs( ((EthHeader *)packet)->type );
  if ( t <= 1500 && len >= 14 + IPX_HLEN ) {
    uint8_t *p = packet + 14;
    int ft = -1, off = 0;
    if ( p[0] == 0xff && p[1] == 0xff ) { ft = 1; off = 0; }
    else if ( p[0] == 0xe0 && p[1] == 0xe0 && p[2] == 0x03 ) { ft = 2; off = 3; }
    else if ( p[0] == 0xaa && p[1] == 0xaa && p[2] == 0x03 && p[6] == 0x81 && p[7] == 0x37 ) { ft = 3; off = 8; }
    if ( ft >= 0 && len >= 14 + off + IPX_HLEN ) {
      if ( !frameFixed ) frameType = ft;
      fromLan( p + off, len - 14 - off, packet + 6 );
    }
  }
  Buffer_free( packet );
}


// ---------------------------------------------------------------------------
// Helpers

static bool isBroadcast( const uint8_t *node ) {
  return node[0] == 0xff && node[1] == 0xff && node[2] == 0xff && node[3] == 0xff;
}

static uint16_t ipxLen( const uint8_t *ipx, uint16_t avail ) {
  uint16_t l = ntohs( ((const IpxHdr *)ipx)->length );
  if ( l < IPX_HLEN || l > avail ) return 0;
  return l;
}

static char *nodeText( const uint8_t *n, char *buf ) {
  sprintf( buf, "%02X%02X%02X%02X%02X%02X", n[0], n[1], n[2], n[3], n[4], n[5] );
  return buf;
}

// A node for a client: DOSBox's convention (IPv4 address, port) for IPv4;
// a made-up locally administered one for IPv6.
static void nodeFor( const NetAddr *a, uint16_t port, uint8_t *node ) {
  if ( a->family == 4 ) {
    memcpy( node, a->a, 4 );
  } else {
    uint8_t h = 0, l = 0;
    for ( int i=0; i < 16; i += 2 ) { h ^= a->a[i]; l ^= a->a[i+1]; }
    node[0] = 0x02; node[1] = 0x00; node[2] = h; node[3] = l;
  }
  node[4] = (uint8_t)(port >> 8); node[5] = (uint8_t)port;
}

static bool sameAddr( const NetAddr *a, const NetAddr *b ) {
  return a->family == b->family && memcmp( a->a, b->a, a->family == 4 ? 4 : 16 ) == 0;
}

// Datagrams waiting for ARP / neighbor discovery (sent from the main loop,
// not from inside a receive handler).
#define PENDING 4
struct Pending {
  bool         used;
  NetAddr      dst;
  uint16_t     sport, dport, len;
  clockTicks_t since;
  uint8_t      data[ MAX_IPX ];
};
static Pending pending[ PENDING ];

static void udpSend( const NetAddr *dst, uint16_t sport, uint16_t dport, const uint8_t *data, uint16_t len ) {
  int8_t rc = Net::sendUdp( dst, sport, dport, data, len );
  if ( rc == 0 ) { udpOut++; return; }
  if ( rc > 0 && len <= MAX_IPX ) {
    for ( int i=0; i < PENDING; i++ ) {
      if ( pending[i].used ) continue;
      Pending *q = &pending[i];
      q->used = true; q->dst = *dst; q->sport = sport; q->dport = dport; q->len = len;
      q->since = TIMER_GET_CURRENT( );
      memcpy( q->data, data, len );
      return;
    }
  }
  dropped++;
}

static void sendPending( void ) {
  for ( int i=0; i < PENDING; i++ ) {
    Pending *q = &pending[i];
    if ( !q->used ) continue;
    int8_t rc = Net::sendUdp( &q->dst, q->sport, q->dport, q->data, q->len );
    if ( rc == 0 ) { udpOut++; q->used = false; }
    else if ( rc < 0 || Timer_diff( q->since, TIMER_GET_CURRENT( ) ) > TIMER_MS_TO_TICKS( 3000ul ) ) { dropped++; q->used = false; }
  }
}


// ---------------------------------------------------------------------------
// SERVER

struct Client {
  bool         used;
  NetAddr      addr;
  uint16_t     port;
  uint8_t      node[6];
  clockTicks_t last;
};

static Client clients[ MAX_CLIENTS ];

static Client *clientByNode( const uint8_t *node ) {
  for ( int i=0; i < MAX_CLIENTS; i++ )
    if ( clients[i].used && memcmp( clients[i].node, node, 6 ) == 0 ) return &clients[i];
  return NULL;
}

static Client *clientByAddr( const NetAddr *a, uint16_t port ) {
  for ( int i=0; i < MAX_CLIENTS; i++ )
    if ( clients[i].used && clients[i].port == port && sameAddr( &clients[i].addr, a ) ) return &clients[i];
  return NULL;
}

static void ackClient( Client *c ) {
  IpxHdr h;
  memset( &h, 0, sizeof(h) );
  h.checksum = 0xffff;
  h.length = htons( IPX_HLEN );
  memcpy( h.dnode, c->node, 6 );
  h.dsock = htons( 2 );
  h.snet[3] = 1;
  if ( c->addr.family == 4 && Net::haveIpv4( ) ) memcpy( h.snode, MyIpAddr, 4 );
  h.snode[4] = (uint8_t)(udpPort >> 8); h.snode[5] = (uint8_t)udpPort;
  h.ssock = htons( 2 );
  udpSend( &c->addr, udpPort, c->port, (uint8_t *)&h, IPX_HLEN );
}

static void serverUdp( const NetAddr *src, uint16_t srcPort, uint16_t dstPort, uint8_t *data, uint16_t len ) {
  udpIn++;
  uint16_t l = ipxLen( data, len );
  if ( !l ) { dropped++; return; }
  IpxHdr *h = (IpxHdr *)data;
  char buf[50], nb[14];

  // Registration
  if ( ntohs( h->dsock ) == 2 && h->dnode[0] == 0 && h->dnode[1] == 0 && h->dnode[2] == 0 && h->dnode[3] == 0 ) {
    Client *c = clientByAddr( src, srcPort );
    if ( !c ) {
      Client *old = &clients[0];
      for ( int i=0; i < MAX_CLIENTS; i++ ) {
        if ( !clients[i].used ) { old = &clients[i]; break; }
        if ( clients[i].last < old->last ) old = &clients[i];
      }
      c = old;
      memset( c, 0, sizeof(*c) );
      c->used = true;
      c->addr = *src;
      c->port = srcPort;
      nodeFor( src, srcPort, c->node );
      eventLine( );
      printf( "Client %s port %u connected, node %s\n", Addr::format( src, buf ), srcPort, nodeText( c->node, nb ) );
    }
    c->last = TIMER_GET_CURRENT( );
    ackClient( c );
    return;
  }

  Client *from = clientByAddr( src, srcPort );
  if ( !from ) { dropped++; return; }      // not registered
  from->last = TIMER_GET_CURRENT( );

  if ( isBroadcast( h->dnode ) ) {
    for ( int i=0; i < MAX_CLIENTS; i++ )
      if ( clients[i].used && &clients[i] != from ) udpSend( &clients[i].addr, udpPort, clients[i].port, data, l );
    if ( ntohs( h->dsock ) != 2 ) lanSend( data, l, bcastNode );   // not IPXNET's pings
    return;
  }
  Client *to = clientByNode( h->dnode );
  if ( to ) { udpSend( &to->addr, udpPort, to->port, data, l ); return; }
  lanSend( data, l, h->dnode );                                   // a LAN machine: node = MAC
}

static void serverLan( uint8_t *ipx, uint16_t len, const uint8_t *smac ) {
  IpxHdr *h = (IpxHdr *)ipx;
  if ( isBroadcast( h->dnode ) ) {
    for ( int i=0; i < MAX_CLIENTS; i++ )
      if ( clients[i].used ) udpSend( &clients[i].addr, udpPort, clients[i].port, ipx, len );
    return;
  }
  Client *to = clientByNode( h->dnode );
  if ( to ) udpSend( &to->addr, udpPort, to->port, ipx, len );
}


// ---------------------------------------------------------------------------
// CONNECT

struct Peer {
  bool         used;
  bool         registered;
  bool         isLan;             // false: the bridge's own (slot 0)
  uint8_t      pad;
  uint8_t      mac[6];            // the LAN machine
  uint8_t      node[6];           // its node on the DOSBox side
  uint16_t     port;              // our UDP port for it
  clockTicks_t last, lastReg, lastKeep;
};

static Peer peers[ MAX_PEERS ];
static uint16_t basePort;

static void sendRegistration( Peer *p ) {
  IpxHdr h;
  memset( &h, 0, sizeof(h) );
  h.checksum = 0xffff;
  h.length = htons( IPX_HLEN );
  h.dsock = htons( 2 );
  h.ssock = htons( 2 );
  udpSend( &remote, p->port, udpPort, (uint8_t *)&h, IPX_HLEN );
  p->lastReg = TIMER_GET_CURRENT( );
}

// Keeps NAT mappings open: a packet to our own node, which the server
// sends straight back (and we drop).
static void sendKeepalive( Peer *p ) {
  IpxHdr h;
  memset( &h, 0, sizeof(h) );
  h.checksum = 0xffff;
  h.length = htons( IPX_HLEN );
  memcpy( h.dnode, p->node, 6 ); h.dsock = htons( 2 );
  memcpy( h.snode, p->node, 6 ); h.ssock = htons( 2 );
  udpSend( &remote, p->port, udpPort, (uint8_t *)&h, IPX_HLEN );
  p->lastKeep = TIMER_GET_CURRENT( );
}

static Peer *peerByPort( uint16_t port ) {
  for ( int i=0; i < MAX_PEERS; i++ ) if ( peers[i].used && peers[i].port == port ) return &peers[i];
  return NULL;
}

static Peer *peerByMac( const uint8_t *mac ) {
  for ( int i=1; i < MAX_PEERS; i++ ) if ( peers[i].used && memcmp( peers[i].mac, mac, 6 ) == 0 ) return &peers[i];
  return NULL;
}

static Peer *peerByNode( const uint8_t *node ) {
  for ( int i=0; i < MAX_PEERS; i++ ) if ( peers[i].used && peers[i].registered && memcmp( peers[i].node, node, 6 ) == 0 ) return &peers[i];
  return NULL;
}

static void connectUdp( const NetAddr *src, uint16_t srcPort, uint16_t dstPort, uint8_t *data, uint16_t len );

static Peer *newPeer( const uint8_t *mac ) {
  int slot = -1;
  for ( int i=1; i < MAX_PEERS; i++ ) if ( !peers[i].used ) { slot = i; break; }
  if ( slot < 0 ) {                       // the longest quiet one goes
    slot = 1;
    for ( int i=2; i < MAX_PEERS; i++ ) if ( peers[i].last < peers[slot].last ) slot = i;
  }
  Peer *p = &peers[slot];
  uint16_t port = p->used ? p->port : (uint16_t)(basePort + slot);
  memset( p, 0, sizeof(*p) );
  p->used = true;
  p->isLan = true;
  memcpy( p->mac, mac, 6 );
  p->port = port;
  p->last = TIMER_GET_CURRENT( );
  Net::listen( port, connectUdp );
  sendRegistration( p );
  return p;
}

static void connectUdp( const NetAddr *src, uint16_t srcPort, uint16_t dstPort, uint8_t *data, uint16_t len ) {
  udpIn++;
  if ( !sameAddr( src, &remote ) || srcPort != udpPort ) { dropped++; return; }
  Peer *p = peerByPort( dstPort );
  uint16_t l = ipxLen( data, len );
  if ( !p || !l ) { dropped++; return; }
  IpxHdr *h = (IpxHdr *)data;
  char nb[14];

  if ( ntohs( h->dsock ) == 2 && ntohs( h->ssock ) == 2 && !isBroadcast( h->dnode ) &&
       (!p->registered || memcmp( h->snode, p->node, 6 ) != 0) && l == IPX_HLEN && h->snet[3] == 1 ) {
    // Registration answer
    bool first = !p->registered;
    memcpy( p->node, h->dnode, 6 );
    p->registered = true;
    p->lastKeep = TIMER_GET_CURRENT( );
    if ( first ) {
      char nb2[14];
      eventLine( );
      if ( p->isLan ) printf( "LAN machine %s is node %s on the server\n", nodeText( p->mac, nb ), nodeText( p->node, nb2 ) );
      else printf( "Connected: node %s\n", nodeText( p->node, nb ) );
    }
    return;
  }
  if ( ntohs( h->dsock ) == 2 && memcmp( h->snode, p->node, 6 ) == 0 ) return;   // our keepalive back

  if ( isBroadcast( h->dnode ) ) {
    if ( ntohs( h->dsock ) == 2 ) {
      // IPXNET PING: answer for this client, as DOSBox does
      IpxHdr r;
      memset( &r, 0, sizeof(r) );
      r.checksum = 0xffff; r.length = htons( IPX_HLEN );
      memcpy( r.dnode, h->snode, 6 ); r.dsock = htons( 2 );
      memcpy( r.snode, p->node, 6 ); r.ssock = htons( 2 );
      udpSend( &remote, p->port, udpPort, (uint8_t *)&r, IPX_HLEN );
      return;
    }
    // Every one of our clients gets each broadcast: pass it on once (from
    // slot 0), and not those from our own LAN machines.
    if ( p != &peers[0] || peerByNode( h->snode ) ) return;
    lanSend( data, l, bcastNode );
    return;
  }
  if ( memcmp( h->dnode, p->node, 6 ) != 0 || !p->isLan ) { dropped++; return; }
  memcpy( h->dnode, p->mac, 6 );
  lanSend( data, l, p->mac );
}

static void connectLan( uint8_t *ipx, uint16_t len, const uint8_t *smac ) {
  IpxHdr *h = (IpxHdr *)ipx;
  // Only what's for the other side: broadcasts and DOSBox nodes; never one
  // LAN machine to another (the LAN does that).
  if ( !isBroadcast( h->dnode ) && peerByMac( h->dnode ) ) return;
  if ( !isBroadcast( h->dnode ) && memcmp( h->dnode, MyEthAddr, 6 ) == 0 ) return;
  Peer *p = peerByMac( smac );
  if ( !p ) p = newPeer( smac );
  p->last = TIMER_GET_CURRENT( );
  if ( !p->registered ) { dropped++; return; }      // games resend
  if ( memcmp( h->snode, smac, 6 ) == 0 ) memcpy( h->snode, p->node, 6 );
  udpSend( &remote, p->port, udpPort, ipx, len );
}


static void fromLan( uint8_t *ipx, uint16_t len, const uint8_t *smac ) {
  lanIn++;
  uint16_t l = ipxLen( ipx, len );
  if ( !l ) { dropped++; return; }
  if ( serverMode ) serverLan( ipx, l, smac );
  else connectLan( ipx, l, smac );
}


// ---------------------------------------------------------------------------

static void usage( void ) {
  puts( "VMIPXBOX: bridges the LAN's IPX and DOSBox's IPX over UDP (IPXNET)\n\n"
        "  VMIPXBOX SERVER [port]        DOSBox players: IPXNET CONNECT <this PC> [port]\n"
        "  VMIPXBOX CONNECT host [port]  join a DOSBox IPXNET server (IPv4 or IPv6)\n\n"
        "  /V        show each client and packet counts as they change\n"
        "  /F:type   LAN framing to send: II, 802.3, 802.2, SNAP (default: as seen)\n\n"
        "The default port is 213 (DOSBox's). Esc quits. This PC is the bridge,\n"
        "not a player: don't load VMIPX on it." );
  exit( 1 );
}


static void status( void ) {
  printf( "\rLAN in %lu out %lu, UDP in %lu out %lu, dropped %lu   ", lanIn, lanOut, udpIn, udpOut, dropped );
  statusShown = true;
}


int main( int argc, char *argv[] ) {
  const char *host = NULL;
  int a = 1;
  if ( argc < 2 ) usage( );
  for ( int i=1; i < argc; i++ ) {
    if ( argv[i][0] == '/' || argv[i][0] == '-' ) {
      char o = (char)toupper( argv[i][1] );
      if ( o == 'V' ) verbose = true;
      else if ( o == 'F' && argv[i][2] == ':' ) {
        const char *t = argv[i] + 3;
        frameFixed = true;
        if ( !stricmp( t, "II" ) ) frameType = 0;
        else if ( !stricmp( t, "802.3" ) ) frameType = 1;
        else if ( !stricmp( t, "802.2" ) ) frameType = 2;
        else if ( !stricmp( t, "SNAP" ) ) frameType = 3;
        else usage( );
      }
      else usage( );
      continue;
    }
    argv[a++] = argv[i];
  }
  argc = a;
  if ( argc < 2 ) usage( );
  if ( !stricmp( argv[1], "SERVER" ) ) {
    serverMode = true;
    if ( argc > 2 ) udpPort = (uint16_t)atoi( argv[2] );
  } else if ( !stricmp( argv[1], "CONNECT" ) ) {
    if ( argc < 3 ) usage( );
    host = argv[2];
    if ( argc > 3 ) udpPort = (uint16_t)atoi( argv[3] );
  } else usage( );
  if ( udpPort == 0 ) usage( );

  // An IPX driver on this PC would get the IPX frames instead of us.
  union REGS r;
  r.x.ax = 0x7a00;
  int86( 0x2f, &r, &r );
  if ( r.h.al == 0xff ) {
    fprintf( stderr, "An IPX driver (VMIPX) is loaded: it takes the IPX frames, so this PC\n"
                     "can't be the bridge. Start it without VMIPX (leave it out of STARTNET.BAT).\n" );
    return 1;
  }

  if ( Net::start( ctrlBreakHandler ) ) return 1;
  Resolv::loadConfig( );

  if ( Packet_registerEtherType( 0x8137, lanHandler ) ) {
    fprintf( stderr, "No room for the IPX EtherType handler\n" );
    Net::stop( );
    return 1;
  }
  Packet_registerDefault( lanOtherHandler );

  char buf[50];
  srand( (unsigned)time( NULL ) );

  if ( serverMode ) {
    if ( Net::listen( udpPort, serverUdp ) ) { fprintf( stderr, "Can't listen on UDP port %u\n", udpPort ); Net::stop( ); return 1; }
    printf( "IPXNET server on UDP port %u:", udpPort );
    if ( Net::haveIpv4( ) ) printf( " %s", Addr::format4( MyIpAddr, buf ) );
    if ( Ip6::enabled ) {
      if ( !Addr::isZero6( Ip6::global ) ) printf( " %s", Addr::format6( Ip6::global, buf ) );
      printf( " %s", Addr::format6( Ip6::linkLocal, buf ) );
    }
    puts( "" );
  } else {
    printf( "Looking up %s ... ", host );
    if ( Resolv::resolve( host, 0, false, &remote ) ) { puts( "not found" ); Net::stop( ); return 1; }
    printf( "%s, port %u\n", Addr::format( &remote, buf ), udpPort );
    basePort = 0x6000 + (rand( ) & 0x0f00);
    Peer *p = &peers[0];
    p->used = true;
    p->port = basePort;
    Net::listen( p->port, connectUdp );
    // The server's address needs resolving (ARP / neighbor discovery) first.
    sendRegistration( p );
    clockTicks_t start = TIMER_GET_CURRENT( );
    while ( !p->registered && !Net::breakFlag && Timer_diff( start, TIMER_GET_CURRENT( ) ) < TIMER_SECS_TO_TICKS( 6ul ) ) {
      Net::poll( );
      sendPending( );
      if ( Timer_diff( p->lastReg, TIMER_GET_CURRENT( ) ) > TIMER_MS_TO_TICKS( 1000ul ) ) sendRegistration( p );
    }
    if ( !p->registered ) { puts( "The server didn't answer." ); Net::stop( ); return 1; }
  }

  // Promiscuous: LAN machines send to DOSBox nodes, which aren't our MAC.
  oldMode = Net::getRcvMode( );
  if ( Net::setRcvMode( 6 ) ) puts( "Warning: the card can't be put in promiscuous mode; unicast from LAN to DOSBox may not get through." );

  puts( "Bridging. Esc quits." );
  uint32_t lastTotal = 0;
  clockTicks_t lastShow = TIMER_GET_CURRENT( );
  while ( !Net::breakFlag ) {
    Net::poll( );
    sendPending( );
    if ( biosIsKeyReady( ) ) {
      uint16_t k = biosKeyRead( );
      if ( (k & 0xff) == 27 ) break;
    }
    clockTicks_t now = TIMER_GET_CURRENT( );
    if ( !serverMode ) {
      for ( int i=0; i < MAX_PEERS; i++ ) {
        Peer *p = &peers[i];
        if ( !p->used ) continue;
        if ( !p->registered ) {
          if ( Timer_diff( p->lastReg, now ) > TIMER_MS_TO_TICKS( 1000ul ) ) sendRegistration( p );
        } else if ( Timer_diff( p->lastKeep, now ) > TIMER_SECS_TO_TICKS( (uint32_t)KEEPALIVE_SECS ) ) {
          sendKeepalive( p );
        }
      }
    }
    if ( verbose && Timer_diff( lastShow, now ) > TIMER_TICKS_PER_SEC ) {
      uint32_t total = lanIn + lanOut + udpIn + udpOut + dropped;
      if ( total != lastTotal ) { status( ); lastTotal = total; }
      lastShow = now;
    }
  }

  if ( oldMode > 0 ) Net::setRcvMode( oldMode );
  puts( "" );
  status( );
  puts( "" );
  Net::stop( );
  return 0;
}
