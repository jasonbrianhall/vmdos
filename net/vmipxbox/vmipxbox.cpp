/*
   VMIPXBOX: sets up vmdos's IPXNET, DOSBox's IPX over UDP, which vmdos then
   runs by itself underneath DOS (src/ipxnet.cpp). Load VMIPX and play: this
   PC's IPX games reach DOSBox players, and the LAN's IPX machines are
   bridged too.

     VMIPXBOX SERVER [port]        be the IPXNET server: DOSBox players run
                                   IPXNET CONNECT <this PC> [port]; default 213
     VMIPXBOX CONNECT host [port]  join a DOSBox IPXNET server (DOSBox's own
                                   IPXNET STARTSERVER, or another vmdos)
     VMIPXBOX                      how it's doing
     VMIPXBOX STOP

   IPv4 and IPv6 (DOSBox itself only speaks IPv4). This program uses the
   TCP/IP settings (MTCPCFG) for the addresses, finds the server and the
   next hop to it, hands that to vmdos (INT 2Fh AX=564Eh BL=8) and exits.
*/

#include <ctype.h>
#include <dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vmnet.h"
#include "arp.h"
#include "eth.h"
#include "ip.h"
#include "timer.h"
#include "utils.h"


#pragma pack(push, 1)
struct Config {                    // as src/ipxnet.cpp
  uint8_t  mode;                   // 0 off, 1 connect, 2 server
  uint8_t  family;
  uint16_t port;
  uint8_t  server[16];
  uint8_t  hopMac[6];
  uint8_t  my4[4];
  uint8_t  my6[16];
  uint8_t  my6ll[16];
  uint16_t localPort;
  uint8_t  hop4[4];                // IPv4: the next hop, so vmdos can find it again
};
struct Entry {
  uint8_t  used, family;
  uint16_t port;
  uint8_t  node[6], mac[6], addr[16];
};
struct Status {
  uint8_t  mode, registered, entries, frame;
  uint16_t port;
  uint8_t  node[6];
  uint32_t udpIn, udpOut, ipxIn, ipxOut, dropped;
  Entry    e[16];
};
#pragma pack(pop)

static Config cfg;
static Status st;

static void __interrupt __far ctrlBreakHandler( ) { Net::breakFlag = 1; }


static bool vmdosHere( void ) {
  union REGS r;
  r.x.ax = 0x564e; r.x.bx = 0; r.x.cx = 0; r.x.dx = 0;
  struct SREGS sr;
  segread( &sr );
  static uint8_t mac[6];
  sr.es = FP_SEG( (void __far *)mac ); r.x.di = FP_OFF( (void __far *)mac );
  int86x( 0x2f, &r, &r, &sr );
  return r.x.bx == 0x564d;
}

static int api( uint8_t fn, void *buf ) {
  union REGS r;
  struct SREGS sr;
  segread( &sr );
  r.x.ax = 0x564e; r.h.bl = fn;
  sr.es = FP_SEG( (void __far *)buf ); r.x.di = FP_OFF( (void __far *)buf );
  int86x( 0x2f, &r, &r, &sr );
  return r.x.ax;
}

static bool ipxLoaded( void ) {
  union REGS r;
  r.x.ax = 0x7a00;
  int86( 0x2f, &r, &r );
  return r.h.al == 0xff;
}

static char *nodeText( const uint8_t *n, char *buf ) {
  sprintf( buf, "%02X%02X%02X%02X%02X%02X", n[0], n[1], n[2], n[3], n[4], n[5] );
  return buf;
}


static int showStatus( void ) {
  static const char *frames[] = { "Ethernet II", "802.3", "802.2", "SNAP" };
  char nb[14], ab[50];
  api( 9, &st );
  if ( st.mode == 0 ) { puts( "IPXNET is off (VMIPXBOX SERVER, or VMIPXBOX CONNECT host)" ); return 1; }
  if ( st.mode == 2 ) {
    printf( "IPXNET server on UDP port %u, %u client(s)\n", st.port, st.entries );
    for ( int i=0; i < st.entries; i++ ) {
      NetAddr a; a.family = st.e[i].family; memcpy( a.a, st.e[i].addr, 16 );
      printf( "  node %s  %s port %u\n", nodeText( st.e[i].node, nb ), Addr::format( &a, ab ), st.e[i].port );
    }
  } else {
    printf( "IPXNET connected to a server on port %u: %s", st.port, st.registered ? "this PC is node " : "registering (vmdos keeps trying)" );
    if ( st.registered ) printf( "%s", nodeText( st.node, nb ) );
    puts( "" );
    for ( int i=0; i < st.entries; i++ ) {
      char mb[14];
      printf( "  LAN machine %s is node %s\n", nodeText( st.e[i].mac, mb ), nodeText( st.e[i].node, nb ) );
    }
  }
  printf( "UDP in %lu out %lu, IPX in %lu out %lu, dropped %lu; LAN framing %s\n",
          st.udpIn, st.udpOut, st.ipxIn, st.ipxOut, st.dropped, frames[ st.frame & 3 ] );
  if ( !ipxLoaded( ) ) puts( "Load VMIPX to play on this PC." );
  return 0;
}


static void usage( void ) {
  puts( "VMIPXBOX: IPX with DOSBox players (DOSBox's IPXNET, IPX over UDP)\n\n"
        "  VMIPXBOX SERVER [port]        DOSBox players: IPXNET CONNECT <this PC> [port]\n"
        "  VMIPXBOX CONNECT host [port]  join a DOSBox IPXNET server (IPv4 or IPv6)\n"
        "  VMIPXBOX                      how it's doing\n"
        "  VMIPXBOX STOP\n\n"
        "vmdos keeps it running underneath DOS: load VMIPX and play. The LAN's IPX\n"
        "machines are bridged too. The default port is 213 (DOSBox's)." );
  exit( 1 );
}


// The MAC of the next hop to an IPv4 address: it, or the gateway.
static bool nextHop4( const uint8_t *dst, uint8_t *mac, uint8_t *hop ) {
  if ( ((*(uint32_t *)dst) & Netmask_u) == (MyIpAddr_u & Netmask_u) ) Ip::copy( hop, dst );
  else Ip::copy( hop, Gateway );
  clockTicks_t start = TIMER_GET_CURRENT( );
  while ( Timer_diff( start, TIMER_GET_CURRENT( ) ) < TIMER_MS_TO_TICKS( 4000ul ) && !Net::breakFlag ) {
    if ( Arp::resolve( hop, mac ) == 0 ) return true;
    Net::poll( );
  }
  return false;
}


int main( int argc, char *argv[] ) {
  if ( !vmdosHere( ) ) { fprintf( stderr, "This needs vmdos (its network interface isn't there).\n" ); return 1; }
  if ( argc < 2 ) return showStatus( );

  if ( !stricmp( argv[1], "STOP" ) ) {
    memset( &cfg, 0, sizeof(cfg) );
    api( 8, &cfg );
    puts( "IPXNET stopped" );
    return 0;
  }

  bool server;
  const char *host = NULL;
  uint16_t port = 213;
  if ( !stricmp( argv[1], "SERVER" ) ) {
    server = true;
    if ( argc > 2 ) port = (uint16_t)atoi( argv[2] );
  } else if ( !stricmp( argv[1], "CONNECT" ) ) {
    if ( argc < 3 ) usage( );
    server = false;
    host = argv[2];
    if ( argc > 3 ) port = (uint16_t)atoi( argv[3] );
  } else usage( );
  if ( !port ) usage( );

  if ( Net::start( ctrlBreakHandler ) ) return 1;
  Resolv::loadConfig( );

  memset( &cfg, 0, sizeof(cfg) );
  cfg.port = port;
  if ( Net::haveIpv4( ) ) memcpy( cfg.my4, MyIpAddr, 4 );
  if ( Ip6::enabled ) {
    memcpy( cfg.my6, Ip6::global, 16 );
    memcpy( cfg.my6ll, Ip6::linkLocal, 16 );
  }

  char buf[50];
  int rc = 0;
  if ( server ) {
    cfg.mode = 2;
    api( 8, &cfg );
    printf( "IPXNET server on UDP port %u. DOSBox players: IPXNET CONNECT", port );
    if ( Net::haveIpv4( ) ) printf( " %s", Addr::format4( MyIpAddr, buf ) );
    else printf( " <this PC>" );
    if ( port != 213 ) printf( " %u", port );
    puts( "" );
    if ( !Net::haveIpv4( ) ) puts( "(No IPv4 address: DOSBox can't reach it, only vmdos over IPv6.)" );
  } else {
    NetAddr srv;
    printf( "Looking up %s ... ", host );
    if ( Resolv::resolve( host, 0, false, &srv ) ) { puts( "not found" ); Net::stop( ); return 1; }
    printf( "%s, port %u\n", Addr::format( &srv, buf ), port );
    cfg.mode = 1;
    cfg.family = srv.family;
    memcpy( cfg.server, srv.a, 16 );
    bool ok;
    if ( srv.family == 4 ) ok = Net::haveIpv4( ) && nextHop4( srv.a, cfg.hopMac, cfg.hop4 );
    else ok = Ip6::enabled && Ip6::resolveWait( srv.a, cfg.hopMac, 4000 ) == 0;
    if ( !ok ) { puts( "No route to it (no address, or the gateway didn't answer)." ); Net::stop( ); return 1; }
    srand( (unsigned)time( NULL ) );
    cfg.localPort = 0x6000 + (rand( ) & 0x1f00);
    api( 8, &cfg );
    // vmdos registers with the server; wait for the answer.
    clockTicks_t start = TIMER_GET_CURRENT( );
    do {
      Net::poll( );
      api( 9, &st );
    } while ( !st.registered && !Net::breakFlag && Timer_diff( start, TIMER_GET_CURRENT( ) ) < TIMER_SECS_TO_TICKS( 6ul ) );
    char nb[14];
    if ( st.registered ) printf( "Connected: this PC is node %s.\n", nodeText( st.node, nb ) );
    else { puts( "The server hasn't answered yet (vmdos keeps trying; VMIPXBOX shows when it does)." ); rc = 1; }
  }
  if ( !ipxLoaded( ) ) puts( "Now load VMIPX and start the game." );
  Net::stop( );
  return rc;
}
