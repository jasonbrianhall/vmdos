/*
   vmdos network tools: starting the stack, UDP over IPv4 or IPv6.
*/

#include <dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmnet.h"
#include "arp.h"
#include "eth.h"
#include "ip.h"
#include "packet.h"
#include "timer.h"
#include "udp.h"
#include "utils.h"


volatile uint8_t Net::breakFlag = 0;

static uint16_t  udpPort[ NET_UDP_PORTS ];
static NetUdpCb  udpCb[ NET_UDP_PORTS ];
static uint8_t   udpPorts = 0;
static bool      started = false;


#ifndef DHCP_CLIENT
int8_t Net::start( void __interrupt __far (*ctrlBreak)( ) ) {
  if ( Utils::parseEnv( ) != 0 ) return -1;
  if ( Utils::initStack( 0, 0, ctrlBreak, ctrlBreak ) ) {
    fprintf( stderr, "Could not start TCP/IP (is the packet driver loaded? VMPKT)\n" );
    return -1;
  }
  started = true;
  Ip6::init( );
  if ( !haveIpv4( ) && !Ip6::enabled ) {
    fprintf( stderr, "No IPv4 address (IPADDR) and IPv6 is off: run DHCP first.\n" );
    stop( );
    return -1;
  }
  return 0;
}


#endif


void Net::stop( void ) {
  if ( !started ) return;
  Ip6::stop( );
  Utils::endStack( );
  started = false;
}


bool Net::haveIpv4( void ) {
  return MyIpAddr_u != 0 && MyIpAddr_u != 0xfffffffful;   // mTCP's "not set" is 255.255.255.255
}


bool Net::haveIpv6( void ) {
  return Ip6::enabled;
}


void Net::poll( void ) {
  PACKET_PROCESS_MULT( 5 );
  #ifdef COMPILE_ARP
  Arp::driveArp( );
  #endif
}


// ---------------------------------------------------------------------------
// UDP

static int portSlot( uint16_t port ) {
  for ( int i=0; i < udpPorts; i++ ) if ( udpPort[i] == port ) return i;
  return -1;
}


static void udp4Handler( const unsigned char *packet, const UdpHeader *udp ) {
  const IpHeader *ip = (const IpHeader *)(packet + sizeof(EthHeader));
  uint16_t dport = ntohs( udp->dst );
  int i = portSlot( dport );
  uint16_t len = ntohs( udp->len );
  if ( i >= 0 && len >= 8 ) {
    NetAddr src;
    memset( &src, 0, sizeof(src) );
    src.family = 4;
    memcpy( src.a, ip->ip_src, 4 );
    udpCb[i]( &src, ntohs( udp->src ), dport, ((uint8_t *)udp) + 8, len - 8 );
  }
  Buffer_free( packet );
}


void Net::deliver6( const uint8_t *src, uint16_t srcPort, uint16_t dstPort, uint8_t *data, uint16_t len ) {
  int i = portSlot( dstPort );
  if ( i < 0 ) return;
  NetAddr a;
  a.family = 6;
  memcpy( a.a, src, 16 );
  udpCb[i]( &a, srcPort, dstPort, data, len );
}


int8_t Net::listen( uint16_t port, NetUdpCb cb ) {
  int i = portSlot( port );
  if ( i >= 0 ) { udpCb[i] = cb; return 0; }
  if ( udpPorts == NET_UDP_PORTS ) return -1;
  if ( Udp::registerCallback( port, udp4Handler ) ) return -1;
  udpPort[ udpPorts ] = port;
  udpCb[ udpPorts ] = cb;
  udpPorts++;
  return 0;
}


void Net::unlisten( uint16_t port ) {
  int i = portSlot( port );
  if ( i < 0 ) return;
  Udp::unregisterCallback( port );
  udpPorts--;
  udpPort[i] = udpPort[ udpPorts ];
  udpCb[i] = udpCb[ udpPorts ];
}


int8_t Net::sendUdp( const NetAddr *dst, uint16_t srcPort, uint16_t dstPort,
                     const uint8_t *data, uint16_t len ) {
  if ( dst->family == 4 ) {
    if ( !haveIpv4( ) ) return -1;
    int8_t rc = Udp::sendUdp( dst->a, srcPort, dstPort, len, (uint8_t *)data, 0 );
    return rc < 0 ? -1 : rc;
  }
  if ( dst->family == 6 ) {
    if ( !Ip6::enabled || len > Ip6::mtu - 48 ) return -1;
    EthAddr_t mac;
    int8_t rc = Ip6::resolve( dst->a, mac );
    if ( rc ) return rc;
    uint8_t *p = Ip6::txPayload( );
    *(uint16_t *)(p + 0) = htons( srcPort );
    *(uint16_t *)(p + 2) = htons( dstPort );
    *(uint16_t *)(p + 4) = htons( len + 8 );
    memcpy( p + 8, data, len );
    return Ip6::send( dst->a, 17, len + 8, 64 );
  }
  return -1;
}


int8_t Net::sendUdpWait( const NetAddr *dst, uint16_t srcPort, uint16_t dstPort,
                         const uint8_t *data, uint16_t len, uint16_t ms ) {
  if ( dst->family == 6 ) {
    if ( !Ip6::enabled ) return -1;
    EthAddr_t mac;
    if ( Ip6::resolveWait( dst->a, mac, ms ) ) return -1;
  }
  clockTicks_t start = TIMER_GET_CURRENT( );
  while ( 1 ) {
    int8_t rc = sendUdp( dst, srcPort, dstPort, data, len );
    if ( rc <= 0 ) return rc;
    if ( breakFlag ) return -1;
    if ( Timer_diff( start, TIMER_GET_CURRENT( ) ) > TIMER_MS_TO_TICKS( (uint32_t)ms ) ) return -1;
    poll( );
  }
}


// ---------------------------------------------------------------------------
// Packet driver receive mode (functions 20 and 21)

int Net::getRcvMode( void ) {
  union REGS r;
  r.h.ah = 21;
  r.x.bx = Packet_getHandle( );
  int86( Packet_getSoftwareInt( ), &r, &r );
  if ( r.x.cflag ) return -1;
  return r.x.ax;
}


int8_t Net::setRcvMode( int mode ) {
  union REGS r;
  r.h.ah = 20;
  r.x.bx = Packet_getHandle( );
  r.x.cx = mode;
  int86( Packet_getSoftwareInt( ), &r, &r );
  return r.x.cflag ? -1 : 0;
}
