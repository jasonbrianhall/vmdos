/*

   mTCP Ping.cpp
   Copyright (C) 2009-2026 Michael B. Brutman (mbbrutman@gmail.com)
   mTCP web page: http://www.brutman.com/mTCP

   This file is part of mTCP.

   mTCP is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   mTCP is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with mTCP.  If not, see <http://www.gnu.org/licenses/>.

   Description: Ping application

   vmdos: mTCP's PING (APPS/PING/PING.CPP) with IPv6: ICMPv6 echo, names
   resolved to A or AAAA (vmdos resolver), -4 / -6 to pick the family.

   Changes:

   2012-04-29: Fix a small bug; outgoing packets were too large
   2011-05-27: Initial release as open source software
   2015-01-18: Minor change to Ctrl-Break and Ctrl-C handling.
   2015-02-03: Code cleanup
   2026-09-07: Change to read the 8253 counter instead of reprogramming it.

*/

// Return codes
//   0 - All good, and all sent packets received a response
//   1 - Usage error
//   2 - DNS error
//   3 - No responses received
//   4 - Some responses received
//

#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "types.h"
#include "trace.h"
#include "utils.h"
#include "packet.h"
#include "arp.h"
#include "udp.h"
#include "vmnet.h"
#include "timer.h"
#include "trace.h"

#define PACKET_COUNT_DEFAULT    (4)
#define PACKET_PAYLOAD_DEFAULT (32)

char    *ServerName;
IpAddr_t ServerAddr;
NetAddr  Target;
uint8_t  Family = 0;            // 0 either, 4, 6
bool     Prefer6 = false;
uint16_t PacketCount = PACKET_COUNT_DEFAULT;
uint16_t PacketPayload = PACKET_PAYLOAD_DEFAULT;

uint16_t TimeoutSecs = 1;

bool Audible = false;

uint16_t PacketsSent = 0;
uint16_t RepliesReceived = 0;
uint16_t RepliesLost = 0;

uint32_t ReplyTime = 0;
uint32_t ReplyMin = 0xffffffff;
uint32_t ReplyMax = 0;

uint16_t icmpLen;

// Ctrl-Break and Ctrl-C handler.  Check the flag once in a while to see if
// the user wants out.

volatile uint8_t CtrlBreakDetected = 0;

void __interrupt __far ctrlBreakHandler( ) {
  CtrlBreakDetected = 1;
  Net::breakFlag = 1;
}

IcmpEchoPacket_t icmpEchoPacket;

// Flag that indicates a new response has arrived.
bool ResponseReceived = false;

// TTL and IP of the last response
uint8_t LastTTL = 0;
IpAddr_t LastIp = { 0, 0, 0, 0 };
uint8_t LastIp6[16];
char    ErrorText[100] = "";

// Function prototypes

void timerModeDetector( void );
void icmpHandler( const unsigned char *packet, const IcmpHeader *icmp );
void sendAndWait( void );
void icmp6Handler( const uint8_t *src, const uint8_t *icmp, uint16_t len, uint8_t hopLimit );
int8_t send6( void );
void parseArgs( int argc, char *argv[] );
void shutdown( int rc );

static char *CopyrightMsg= "\nvmdos PING (IPv4 + IPv6), from mTCP Ping by M Brutman\n(mbbrutman@gmail.com) (C)opyright 2009-2026\nVersion: " __DATE__ "\n";

int main( int argc, char *argv[] ) {

  puts( CopyrightMsg );

  parseArgs( argc, argv );

  // Initialize TCP/IP (IPv4 and IPv6)
  if ( Net::start( ctrlBreakHandler ) ) {
    exit( 1 );
  }

  // From this point forward you have to call the shutdown( ) routine to
  // exit because we have the timer interrupt hooked.

  timerModeDetector( );

  // Resolve the name: A or AAAA.
  Resolv::loadConfig( );
  if ( Resolv::resolve( ServerName, Family, Prefer6, &Target ) != 0 ) {
    if ( !CtrlBreakDetected ) printf( "Error resolving server name: %s\n", ServerName );
    shutdown( 2 );
  }

  if ( Target.family == 6 ) {
    char buf[50];
    if ( !Ip6::enabled ) { puts( "IPv6 is off (VMTCP IPV6 ON)" ); shutdown( 1 ); }
    static const uint8_t loop6[16] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 };
    if ( Addr::same6( Target.a, Ip6::linkLocal ) || Addr::same6( Target.a, Ip6::global ) ||
         Addr::isZero6( Target.a ) || Addr::same6( Target.a, loop6 ) ) {
      puts( "Pinging your own address or loopback addresses is not supported." );
      shutdown( 1 );
    }
    if ( PacketPayload > Ip6::mtu - 48 ) PacketPayload = Ip6::mtu - 48;
    printf( "Sending ICMPv6 packets to %s\n"
            "ICMP Packet payload is %u bytes.\n\n", Addr::format6( Target.a, buf ), PacketPayload );
    Ip6::echoCallback = icmp6Handler;
    for ( int i=0; i < PacketPayload; i++ ) icmpEchoPacket.data[i] = (i % 26) + 'A';
    icmpEchoPacket.seq = 0;
    EthAddr_t mac;
    if ( Ip6::resolveWait( Target.a, mac, 4000 ) ) {
      puts( (Addr::isZero6( Ip6::router ) && !Addr::isLinkLocal6( Target.a )) ? "No IPv6 route (no router: run DHCP)." : "Timeout waiting for neighbor discovery." );
      shutdown( 3 );
    }
  }
  else {

  Ip::copy( ServerAddr, Target.a );
  if ( !Net::haveIpv4( ) ) { puts( "No IPv4 address (run DHCP, or ping an IPv6 address)." ); shutdown( 1 ); }

  if ( Ip::isSame( ServerAddr, MyIpAddr ) || (ServerAddr[0] == 127) ) {
    puts( "Pinging your own address or loopback addresses is not supported." );
    shutdown( 1 );
  }

  printf( "Sending ICMP packets to %u.%u.%u.%u\n"
          "ICMP Packet payload is %u bytes.\n\n",
          ServerAddr[0], ServerAddr[1], ServerAddr[2], ServerAddr[3],
          PacketPayload);

  // Register Icmp handler
  Icmp::icmpCallback = icmpHandler;

  // Build up our packet

  icmpLen = PacketPayload + sizeof( IcmpHeader ) + sizeof( uint16_t ) * 2;

  // Eth header source and type
  icmpEchoPacket.eh.setSrc( MyEthAddr );
  icmpEchoPacket.eh.setType( 0x0800 );

  // Ip Header
  icmpEchoPacket.ip.set( IP_PROTOCOL_ICMP, ServerAddr, icmpLen, 0, 0 );

  // Icmp header
  icmpEchoPacket.icmp.type = ICMP_ECHO_REQUEST;
  icmpEchoPacket.icmp.code = 0;
  icmpEchoPacket.icmp.checksum = 0;

  // Icmp data

  icmpEchoPacket.ident = 0x6048; // Will be 4860 when transmitted; the PCjr model number
  icmpEchoPacket.seq = 0;

  for ( int i=0; i < PacketPayload; i++ ) {
    icmpEchoPacket.data[i] = (i % 26) + 'A';
  }

  // Icmp checksum
  //icmpEchoPacket.icmp.checksum = Ip::genericChecksum( (uint16_t *)&icmpEchoPacket.icmp, icmpLen );
  icmpEchoPacket.icmp.checksum = ipchksum( (uint16_t *)&icmpEchoPacket.icmp, icmpLen );

  // Eth header dest - has to be called after Ip header is set.
  // Should return a 1 the first time because we haven't ARPed yet.
  int8_t arpRc = icmpEchoPacket.ip.setDestEth( icmpEchoPacket.eh.dest );

  // Wait for ARP response

  clockTicks_t startTime = TIMER_GET_CURRENT( );

  while ( arpRc ) {

    if ( Timer_diff( startTime, TIMER_GET_CURRENT( ) ) > TIMER_MS_TO_TICKS( 4000 ) ) {
      break;
    }

    Utils::processPackets( );

    arpRc = icmpEchoPacket.ip.setDestEth( icmpEchoPacket.eh.dest );

  }

  if ( arpRc ) {
    puts( "Timeout waiting for ARP response." );
    shutdown( 3 );
  }

  } // IPv4

  // At this point we ARPed and got a response.  Time to start sending
  // packets.

  sendAndWait( );

  if ( PacketCount > 1 ) {

    for ( uint16_t l = 1; l < PacketCount; l++ ) {

      if ( CtrlBreakDetected ) break;

      // Setup the packet.
      icmpEchoPacket.icmp.checksum = 0;
      icmpEchoPacket.seq = htons( l );
      if ( Target.family == 6 ) { sendAndWait( ); continue; }
      //icmpEchoPacket.icmp.checksum = Ip::genericChecksum( (uint16_t *)&icmpEchoPacket.icmp, icmpLen );
      icmpEchoPacket.icmp.checksum = ipchksum( (uint16_t *)&icmpEchoPacket.icmp, icmpLen );

      sendAndWait( );

    }

  }

  printf( "\nPackets sent: %u, Replies received: %u, Replies lost: %u\n",
          PacketsSent, RepliesReceived, RepliesLost );

  if ( RepliesReceived ) {

    uint32_t big = ( ReplyTime / RepliesReceived ) / 10lu;
    uint32_t small = ( ReplyTime / RepliesReceived ) % 10;

    printf( "Round trip time min / avg / max = %lu.%01lu / %lu.%01lu / %lu.%01lu\n",
             ReplyMin/10, ReplyMin%10, big, small, ReplyMax/10, ReplyMax%10 );
  }

  int rc = 4;
  if ( PacketsSent == RepliesReceived ) {
    rc = 0;
  } else if ( RepliesReceived == 0 ) {
    rc = 3;
  }

  shutdown( rc );

  return 0;
}

// ---------------------------------------------------------------------------
//
// Timer stuff
//
// Instead of reprogramming the timer to increment the BIOS tick faster and
// counting those fast ticks, directly read the timer again.  This is nicer
// because we don't have to reprogram the 8253 or hook the hardware timer
// interrupt, and we can still get great precision.  (Even better than our
// previous code, which with reprogramming the timer was only getting 0.85ms
// granularity.

// These constants assume the machine is using 8253 timer mode 2.

uint16_t TimerOneMs = TIMER_8253_ONE_MS;
uint16_t TimerTenthMs = TIMER_8253_ONE_MS / 10;

void timerModeDetector( void ) {

  int modeDetected = Timer_modeDetector( );

  if ( modeDetected == 3 ) {
    TimerTenthMs = TimerTenthMs << 1;
    TimerOneMs = TimerOneMs << 1;
  }

  TRACE(("Ping: Timer mode %d detected\n", modeDetected));

}

// Called by the low level Icmp Handler.  Check to see if this is the
// reply we are waiting for, and if so set a flag to indicate victory.

void icmpHandler( const unsigned char *packet, const IcmpHeader *icmp ) {

  if ( icmp->type == ICMP_ECHO_REPLY ) {

    IcmpEchoPacket_t *icmpReply = (IcmpEchoPacket_t *)packet;

    if ( icmpReply->ident == 0x6048 && icmpReply->seq == icmpEchoPacket.seq ) {

      // More checks ..

      uint16_t icmpPayloadLen = icmpReply->ip.payloadLen( ) - sizeof(IcmpHeader) - sizeof(uint16_t) * 2;

      if ( icmpPayloadLen == PacketPayload && memcmp(icmpReply->data, icmpEchoPacket.data, PacketPayload) == 0 ) {
        ResponseReceived = true;
        LastTTL = icmpReply->ip.ttl;
        Ip::copy( LastIp, icmpReply->ip.ip_src );
      }

    }

  }

}

void sendAndWait( ) {

  PacketsSent++;

  ResponseReceived = false;

  // You can get multiple responses when doing a broadcast ping.
  int16_t RepliesThisIteration = 0;

  // Don't have to worry about a minimul length - icmpEchoPacket is guaranteed to be
  // bigger than 60 bytes.
  ErrorText[0] = 0;
  if ( Target.family == 6 ) {
    if ( send6( ) ) {
      printf( "Packet sequence number %u: no route or neighbor\n", ntohs( icmpEchoPacket.seq ) );
      RepliesLost++;
      return;
    }
  }
  else Packet_send_pkt( &icmpEchoPacket, icmpLen + sizeof(EthHeader) + sizeof(IpHeader) );

  // Precompute the timeout value to avoid an expensive 32 multiply each
  // time through the loop.  It needs to be 32 bits because 16 bits only covers
  // up to 6 seconds.
  uint32_t timeout_10ths = TimeoutSecs * 10000l;

  uint32_t tenths_ms = 0;
  uint16_t startTime = readTimer_8253( );

  while ( 1 ) {

    if ( CtrlBreakDetected ) return;

    Utils::processPackets( );

    uint16_t c = readTimer_8253( );
    uint16_t e = startTime - c;

    // More than one tenth of a millisecond?
    if ( e > (TimerTenthMs) ) {

      while ( e > TimerTenthMs ) {
        tenths_ms += 1;
        e = e - (TimerTenthMs);
      }

      // Add the remainder back onto the starting time so we include it next time.
      startTime = c + e;

    }

    if ( ErrorText[0] ) {
      printf( "Packet sequence number %u: %s\n", ntohs( icmpEchoPacket.seq ), ErrorText );
      ErrorText[0] = 0;
    }

    if ( ResponseReceived ) {

      if ( Target.family == 6 ) {
        char buf[50];
        printf( "Packet sequence number %u received from %s in %lu.%01lu ms, hlim=%u\n",
                ntohs( icmpEchoPacket.seq ), Addr::format6( LastIp6, buf ),
                tenths_ms/10, tenths_ms%10, LastTTL );
      } else
      printf( "Packet sequence number %u received from %u.%u.%u.%u in %lu.%01lu ms, ttl=%u\n",
              ntohs( icmpEchoPacket.seq ),
              LastIp[0], LastIp[1], LastIp[2], LastIp[3],
              tenths_ms/10, tenths_ms%10, LastTTL );

      RepliesThisIteration++;
      RepliesReceived++;
      ReplyTime += tenths_ms;
      if ( ReplyMin > tenths_ms ) ReplyMin = tenths_ms;
      if ( ReplyMax < tenths_ms ) ReplyMax = tenths_ms;

      // Playing this sound costs 50ms, so be careful where you place this code.
      if (Audible) { sound(500); delay(50); nosound( ); }

      ResponseReceived = 0;
    }

    // Wait a full second for replies to come in.  Reset the timer each time we
    // get a response so we break the loop one second after the last response
    // appears, or after one second if no responses appear.

    if ( tenths_ms > timeout_10ths ) {

      if ( RepliesThisIteration == 0 ) {
        printf( "Packet sequence number %u: timeout!\n", ntohs( icmpEchoPacket.seq ) );
        RepliesLost++;
      }

      break;
    }

  }

}

void usage( void ) {
  printf( "\nping [options] <name or IPv4/IPv6 address>\n\n"
        "Options:\n"
        "  -help        Shows this help\n"
        "  -4 / -6      IPv4 / IPv6 only (a name: A / AAAA record)\n"
        "  -a           Audible ping\n"
        "  -count <n>   Number of packets to send, default is %u\n"
        "  -size <n>    Size of ICMP payload to send, default is %u, max is %u\n"
        "  -timeout <n> Number of seconds to wait for a response packet\n",
        PACKET_COUNT_DEFAULT, PACKET_PAYLOAD_DEFAULT, ICMP_ECHO_OPT_DATA);
  exit( 1 );
}

void parseArgs( int argc, char *argv[] ) {

  int i=1;
  for ( ; i<argc; i++ ) {

    if ( argv[i][0] != '-' ) break;

    if ( stricmp( argv[i], "-help" ) == 0 ) {
      usage( );
    }
    else if ( stricmp( argv[i], "-count" ) == 0 ) {
      i++;
      if ( i == argc ) {
        usage( );
      }
      PacketCount = atoi( argv[i] );
      if ( PacketCount == 0 ) {
        puts( "Bad parameter for -count" );
        usage( );
      }
    }
    else if ( stricmp( argv[i], "-size" ) == 0 ) {
      i++;
      if ( i == argc ) {
        usage( );
      }
      PacketPayload = atoi( argv[i] );
      if ( PacketPayload > ICMP_ECHO_OPT_DATA ) {
        printf( "Bad parameter for -size: Limit is %u\n", ICMP_ECHO_OPT_DATA );
        usage( );
      }
    }
    else if ( stricmp( argv[i], "-timeout" ) == 0 ) {
      i++;
      if ( i == argc ) {
        usage( );
      }
      TimeoutSecs = atoi( argv[i] );
      if ( TimeoutSecs == 0 ) {
        puts( "Bad parameter for -timeout: Should be greater than 0" );
        usage( );
      }
    }
    else if ( strcmp( argv[i], "-4" ) == 0 ) {
      Family = 4;
    }
    else if ( strcmp( argv[i], "-6" ) == 0 ) {
      Family = 6; Prefer6 = true;
    }
    else if ( strcmp( argv[i], "-a" ) == 0 ) {
      Audible = true;
    }
    else {
      printf( "Unknown option: %s\n", argv[i] );
      usage( );
    }

  }

  if ( i == argc ) {
    puts( "A machine name or IP address is required." );
    usage( );
  }

  ServerName = argv[i];

}

void shutdown( int rc ) {
  Net::stop( );
  exit( rc );
}



// vmdos: ICMPv6 echo

#define ICMP6_IDENT 0x6048

int8_t send6( void ) {
  uint8_t *p = Ip6::txPayload( );
  p[0] = 128; p[1] = 0; p[2] = p[3] = 0;
  *(uint16_t *)(p + 4) = ICMP6_IDENT;
  *(uint16_t *)(p + 6) = icmpEchoPacket.seq;
  memcpy( p + 8, icmpEchoPacket.data, PacketPayload );
  return Ip6::send( Target.a, 58, 8 + PacketPayload, 64 ) ? -1 : 0;
}


static const char *icmp6Error( uint8_t type, uint8_t code ) {
  switch ( type ) {
    case 1:
      switch ( code ) {
        case 0: return "destination unreachable (no route)";
        case 1: return "destination unreachable (prohibited)";
        case 3: return "destination unreachable (address)";
        case 4: return "destination unreachable (port)";
        default: return "destination unreachable";
      }
    case 2: return "packet too big";
    case 3: return "time exceeded";
    default: return "parameter problem";
  }
}


void icmp6Handler( const uint8_t *src, const uint8_t *icmp, uint16_t len, uint8_t hopLimit ) {
  if ( icmp[0] == 129 ) {
    if ( len == 8 + PacketPayload && *(uint16_t *)(icmp + 4) == ICMP6_IDENT &&
         *(uint16_t *)(icmp + 6) == icmpEchoPacket.seq && memcmp( icmp + 8, icmpEchoPacket.data, PacketPayload ) == 0 ) {
      ResponseReceived = true;
      LastTTL = hopLimit;
      memcpy( LastIp6, src, 16 );
    }
    return;
  }
  // An error about our echo request: the invoking packet follows (IPv6
  // header, then our ICMPv6 header).
  if ( len >= 8 + 40 + 8 && icmp[8 + 40] == 128 && *(uint16_t *)(icmp + 8 + 40 + 4) == ICMP6_IDENT ) {
    char buf[50];
    sprintf( ErrorText, "%s from %s", icmp6Error( icmp[0], icmp[1] ), Addr::format6( src, buf ) );
  }
}
