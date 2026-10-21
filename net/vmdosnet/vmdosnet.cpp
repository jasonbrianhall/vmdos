/*
   VMDOSNET: a folder shared over 9P as a DOS drive. vmdos does the work
   (src/fs9p.cpp, src/tcp9p.cpp); nothing stays resident in DOS.

     VMDOSNET                          list the shares and their drives
     VMDOSNET V: [share] [options]     QEMU's virtio-9p share ("share": its
                                       mount tag; default the first)
     VMDOSNET V: host[:port][/export] [options]
                                       a 9P2000.L server on the network
                                       (diod; port 564 by default), IPv4 or
                                       IPv6 ([2001:db8::1]:5640/srv/dos)
       /LFN     long names for programs that ask for them (INT 21h 71xxh)
       /WT      write straight through (no gathering of writes)
       /RO      read-only
       /C:kb    file cache size, KiB (default 4096; the first drive sets it)
       /T:ms    how long listings are trusted before checking (default 1000)
       /UID:n   the user to attach as on a network server (default 0)
     VMDOSNET V: /D                    take the share out of V:
     VMDOSNET /F                       write out what is gathered now

   For a network server this uses the TCP/IP settings (MTCPCFG, filled in by
   DHCP): it looks the server up and finds the next hop, then hands that to
   vmdos, which runs the TCP connection itself underneath DOS (as IPXNET).
   (INT 2Fh AX=5652h; BX=0 list to ES:DI, 1 connect (ES:DI the block below),
   2 disconnect DL, 3 flush; returns AX = 0 or an error, BX = 'VM'.)
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
struct NetCfg {                    // as Tcp9pCfg in src/tcp9p.hpp
  uint8_t  family;
  uint16_t port;
  uint8_t  server[16];
  uint8_t  hopMac[6];
  uint8_t  my4[4];
  uint8_t  my6[16], my6ll[16];
  uint16_t localPort;
  uint8_t  hop4[4];
  uint8_t  hop6[16];
};
struct Block {                     // as mount() in src/fs9p.cpp
  uint16_t sdaOff, sdaSeg, lolOff, lolSeg;
  uint8_t  drive, flags;
  uint16_t cacheKb, ttl;
  uint8_t  kind;                   // 0 virtio-9p, 1 TCP
  uint32_t uid;
  char     tag[32];
  NetCfg   net;
  char     aname[128];
};
#pragma pack(pop)

static Block blk;
static char listBuf[1200];

static void __interrupt __far ctrlBreakHandler( ) { Net::breakFlag = 1; }


static uint16_t api( uint16_t fn, void __far *buf, uint8_t dl, uint16_t *bxOut ) {
  union REGS r;
  struct SREGS sr;
  segread( &sr );
  r.x.ax = 0x5652; r.x.bx = fn; r.h.dl = dl;
  sr.es = FP_SEG( buf ); r.x.di = FP_OFF( buf );
  int86x( 0x2f, &r, &r, &sr );
  if ( bxOut ) *bxOut = r.x.bx;
  return r.x.ax;
}

static bool vmdosHere( void ) {
  uint16_t bx;
  api( 0, (void __far *)listBuf, 0, &bx );
  return bx == 0x564d;
}

static void showList( void ) {
  api( 0, (void __far *)listBuf, 0, NULL );
  char *e = strchr( listBuf, '$' );
  if ( e ) *e = 0;
  printf( "9P shares:\n%s", listBuf );
}

static const char *errText( uint16_t e ) {
  switch ( e ) {
    case 1:  return "no virtio-9p device (QEMU: -virtfs local,path=DIR,mount_tag=share,security_model=none)";
    case 2:  return "no virtio-9p share with that tag (VMDOSNET lists them)";
    case 3:  return "that drive letter is in use or past LASTDRIVE";
    case 4:  return "the 9P handshake failed (Tversion / Tattach: the export or the user?)";
    case 5:  return "out of memory";
    case 7:  return "that drive is not a share";
    case 8:  return "files are open on that drive";
    case 9:  return "not in VMHV's virtual machine (boot menu choice 1)";
    case 10: return "the server didn't answer (address, port, firewall?)";
    case 11: return "the server refused the connection (nothing listening on that port?)";
    case 12: return "too many shares";
  }
  return "failed";
}

static void usage( void ) {
  puts( "VMDOSNET: a folder shared over 9P as a DOS drive\n\n"
        "  VMDOSNET V: [share] [opts]           QEMU's virtio-9p share (its mount tag)\n"
        "  VMDOSNET V: host[:port][/export] [opts]   a 9P server (diod), port 564\n"
        "  VMDOSNET V: /D    take it out     VMDOSNET /F    write out now\n"
        "  VMDOSNET          list\n\n"
        "  /LFN long names  /WT write-through  /RO read-only  /C:kb cache  /T:ms\n"
        "  listings trusted  /UID:n user on a network server\n"
        "  e.g. VMDOSNET V: 192.168.1.10/home/jason/dos /LFN" );
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

// "host[:port][/export]", "[v6]:port/export", or a bare IPv6 address.
static void splitTarget( char *t, char **host, uint16_t *port, char **aname ) {
  *port = 564;
  *aname = NULL;
  char *rest;
  if ( *t == '[' ) {
    *host = t + 1;
    rest = strchr( t, ']' );
    if ( !rest ) usage( );
    *rest++ = 0;
  } else {
    *host = t;
    char *c1 = strchr( t, ':' );
    if ( c1 && strchr( c1 + 1, ':' ) ) {            // an IPv6 address without []
      char *sl = strchr( t, '/' );
      if ( sl ) { *aname = sl; }
      return;
    }
    rest = t;
    while ( *rest && *rest != ':' && *rest != '/' ) rest++;
  }
  if ( *rest == ':' ) {
    *rest++ = 0;
    char *e;
    long p = strtol( rest, &e, 10 );
    if ( p < 1 || p > 65535 ) usage( );
    *port = (uint16_t)p;
    rest = e;
  }
  if ( *rest == '/' ) { *aname = rest; }
  else if ( *rest ) usage( );
}

static int mountIt( void ) {
  union REGS r;
  struct SREGS sr;
  segread( &sr );
  r.x.ax = 0x5d06;                                 // DOS's swappable data area: DS:SI
  int86x( 0x21, &r, &r, &sr );
  if ( r.x.cflag ) { puts( "VMDOSNET: this DOS has no swappable data area (INT 21h 5D06h)" ); return 1; }
  blk.sdaSeg = sr.ds; blk.sdaOff = r.x.si;
  segread( &sr );
  r.h.ah = 0x52;                                   // list of lists: ES:BX
  int86x( 0x21, &r, &r, &sr );
  blk.lolSeg = sr.es; blk.lolOff = r.x.bx;
  return api( 1, (void __far *)&blk, 0, NULL );
}


int main( int argc, char *argv[] ) {
  if ( !vmdosHere( ) ) { fprintf( stderr, "VMDOSNET: not running under vmdos\n" ); return 1; }
  if ( argc < 2 ) { showList( ); return 0; }

  if ( argv[1][0] == '/' ) {
    if ( toupper( argv[1][1] ) != 'F' ) usage( );
    api( 3, (void __far *)listBuf, 0, NULL );
    puts( "Written out." );
    return 0;
  }
  char *a = argv[1];
  if ( !isalpha( a[0] ) || a[1] != ':' || a[2] ) usage( );
  blk.drive = (uint8_t)(toupper( a[0] ) - 'A');

  char *target = NULL;
  bool unmount = false;
  for ( int i = 2; i < argc; i++ ) {
    char *o = argv[i];
    if ( o[0] != '/' ) { if ( target ) usage( ); target = o; continue; }
    char *v = strchr( o, ':' );
    if ( !stricmp( o, "/D" ) ) unmount = true;
    else if ( !stricmp( o, "/LFN" ) ) blk.flags |= 1;
    else if ( !stricmp( o, "/WT" ) ) blk.flags |= 2;
    else if ( !stricmp( o, "/RO" ) ) blk.flags |= 4;
    else if ( v && toupper( o[1] ) == 'C' && v == o + 2 ) blk.cacheKb = (uint16_t)atoi( v + 1 );
    else if ( v && toupper( o[1] ) == 'T' && v == o + 2 ) blk.ttl = (uint16_t)atoi( v + 1 );
    else if ( v && !strnicmp( o, "/UID:", 5 ) ) blk.uid = (uint32_t)atol( v + 1 );
    else usage( );
  }

  if ( unmount ) {
    uint16_t e = api( 2, (void __far *)listBuf, blk.drive, NULL );
    if ( e ) { printf( "VMDOSNET: %s\n", errText( e ) ); return 1; }
    printf( "%c: disconnected.\n", a[0] );
    return 0;
  }

  // A virtio-9p share first (no target, or a plain name that is a tag).
  bool netty = target && (strpbrk( target, ".:[/" ) != NULL);
  if ( !netty ) {
    blk.kind = 0;
    if ( target ) { strncpy( blk.tag, target, 31 ); }
    uint16_t e = mountIt( );
    if ( !e ) { showList( ); return 0; }
    if ( !target || (e != 1 && e != 2) ) { printf( "VMDOSNET: %s\n", errText( e ) ); return 1; }
    // not a tag: a host name, then
  }

  char *host, *aname;
  uint16_t port;
  splitTarget( target, &host, &port, &aname );
  if ( Net::start( ctrlBreakHandler ) ) {
    puts( "VMDOSNET: no TCP/IP settings (run STARTNET, or DHCP, first)" );
    return 1;
  }
  Resolv::loadConfig( );
  NetAddr srv;
  char buf[50];
  printf( "Looking up %s ... ", host );
  if ( Resolv::resolve( host, 0, false, &srv ) ) { puts( "not found" ); Net::stop( ); return 1; }
  printf( "%s, port %u\n", Addr::format( &srv, buf ), port );

  NetCfg &n = blk.net;
  n.family = srv.family;
  n.port = port;
  memcpy( n.server, srv.a, 16 );
  if ( Net::haveIpv4( ) ) memcpy( n.my4, MyIpAddr, 4 );
  if ( Ip6::enabled ) { memcpy( n.my6, Ip6::global, 16 ); memcpy( n.my6ll, Ip6::linkLocal, 16 ); }
  bool ok;
  if ( srv.family == 4 ) ok = Net::haveIpv4( ) && nextHop4( srv.a, n.hopMac, n.hop4 );
  else {
    ok = Ip6::enabled && Ip6::resolveWait( srv.a, n.hopMac, 4000 ) == 0;
    if ( ok ) memcpy( n.hop6, Ip6::routerMacKnown && !memcmp( n.hopMac, Ip6::routerMac, 6 ) ? Ip6::router : srv.a, 16 );
  }
  if ( !ok ) { puts( "No route to it (no address, or the gateway didn't answer)." ); Net::stop( ); return 1; }
  srand( (unsigned)time( NULL ) );
  n.localPort = (uint16_t)(0xc000 | (rand( ) & 0x3fff));
  blk.kind = 1;
  if ( srv.family == 6 ) sprintf( blk.tag, "[%.20s]:%u", Addr::format( &srv, buf ), port );
  else sprintf( blk.tag, "%s:%u", Addr::format( &srv, buf ), port );
  if ( aname ) strncpy( blk.aname, aname, 127 );
  Net::stop( );                                   // vmdos takes it from here

  uint16_t e = mountIt( );
  if ( e ) { printf( "VMDOSNET: %s\n", errText( e ) ); return 1; }
  showList( );
  return 0;
}
