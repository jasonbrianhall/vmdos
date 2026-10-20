/*
   VMTCP: the vmdos TCP/IP configuration, IPv4 and IPv6.

     VMTCP                 show the settings and what the network offers
     VMTCP /Q              settings only (no router solicitation)
     VMTCP KEY value       set a value in the config file (MTCPCFG)
     VMTCP KEY -           remove it

   The config file is mTCP's (MTCPCFG), so mTCP's own programs (FTP,
   TELNET, HTGET, ...) use the same settings; the IPv6 keys are vmdos's:
     IPV6 ON|OFF, IPV6ADDR addr/len, IPV6GATEWAY addr, IPV6NAMESERVER addr
*/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmnet.h"
#include "eth.h"
#include "ip.h"
#include "packet.h"
#include "utils.h"


static void __interrupt __far ctrlBreakHandler( ) { Net::breakFlag = 1; }


static void usage( void ) {
  puts( "VMTCP: TCP/IP settings (IPv4 and IPv6) for vmdos's network tools\n\n"
        "  VMTCP               show the settings and what the network offers\n"
        "  VMTCP /Q            show the settings only\n"
        "  VMTCP KEY value     set a value      VMTCP KEY -   remove it\n\n"
        "Keys: PACKETINT HOSTNAME DOMAIN IPADDR NETMASK GATEWAY NAMESERVER MTU\n"
        "      IPV6 (ON/OFF) IPV6ADDR (addr/len) IPV6GATEWAY IPV6NAMESERVER\n"
        "The file is the one MTCPCFG names (C:\\VMDOS\\NET\\VMTCP.CFG); DHCP fills it in." );
  exit( 1 );
}


static bool validate( const char *key, const char *val ) {
  NetAddr a;
  uint8_t b[16], l;
  if ( !stricmp( key, "IPADDR" ) || !stricmp( key, "NETMASK" ) || !stricmp( key, "GATEWAY" ) ||
       !stricmp( key, "NAMESERVER" ) || !stricmp( key, "NAMESERVER_PREFERRED" ) )
    return Addr::parse( val, &a ) && a.family == 4;
  if ( !stricmp( key, "IPV6ADDR" ) ) return Addr::parsePrefix6( val, b, &l );
  if ( !stricmp( key, "IPV6GATEWAY" ) || !stricmp( key, "IPV6NAMESERVER" ) ) return Addr::parse6( val, b );
  if ( !stricmp( key, "IPV6" ) ) return !stricmp( val, "ON" ) || !stricmp( val, "OFF" );
  if ( !stricmp( key, "PACKETINT" ) ) { unsigned v = (unsigned)strtoul( val, NULL, 16 ); return v >= 0x60 && v <= 0x80; }
  if ( !stricmp( key, "MTU" ) ) { int v = atoi( val ); return v >= 576 && v <= 1500; }
  return true;
}


static int setKey( int argc, char *argv[] ) {
  const char *key = argv[1];
  char val[ CFG_VAL_LEN ] = "";
  for ( int i=2; i < argc; i++ ) {
    if ( i > 2 ) strcat( val, " " );
    strncat( val, argv[i], sizeof(val) - strlen( val ) - 2 );
  }
  char ukey[40];
  strncpy( ukey, key, sizeof(ukey)-1 ); ukey[sizeof(ukey)-1] = 0;
  strupr( ukey );
  const char *drop[2] = { ukey, NULL };
  if ( strcmp( val, "-" ) == 0 ) {
    if ( Cfg::update( drop, NULL ) ) return 1;
    printf( "%s removed\n", ukey );
    return 0;
  }
  if ( !validate( ukey, val ) ) { fprintf( stderr, "%s: '%s' isn't a valid value\n", ukey, val ); return 1; }
  char line[ CFG_VAL_LEN + 44 ];
  sprintf( line, "%s %s", ukey, val );
  const char *add[4] = { line, NULL, NULL, NULL };
  // A new file: the packet driver vmdos's VMPKT uses, and a host name.
  FILE *f = fopen( Cfg::fileName( ), "r" );
  if ( f ) fclose( f );
  else {
    int n = 1;
    if ( strcmp( ukey, "PACKETINT" ) ) add[n++] = "PACKETINT 0x60";
    if ( strcmp( ukey, "HOSTNAME" ) ) add[n++] = "HOSTNAME VMDOS";
  }
  if ( Cfg::update( drop, add ) ) return 1;
  printf( "%s\n", line );
  return 0;
}


static void showKey( const char *label, const char *key ) {
  char v[ CFG_VAL_LEN ];
  if ( Cfg::get( key, v, sizeof(v) ) && v[0] ) printf( "  %-14s %s\n", label, v );
}


int main( int argc, char *argv[] ) {
  bool quick = false;
  if ( argc > 1 ) {
    if ( argv[1][0] == '/' || argv[1][0] == '-' ) {
      if ( toupper( argv[1][1] ) == 'Q' ) quick = true;
      else usage( );
    } else {
      if ( argc < 3 ) usage( );
      if ( !Cfg::fileName( ) ) { fprintf( stderr, "Set MTCPCFG first (SET MTCPCFG=C:\\VMDOS\\NET\\VMTCP.CFG)\n" ); return 1; }
      return setKey( argc, argv );
    }
  }

  const char *fn = Cfg::fileName( );
  if ( !fn ) { fprintf( stderr, "MTCPCFG is not set (SET MTCPCFG=C:\\VMDOS\\NET\\VMTCP.CFG)\n" ); return 1; }

  if ( Net::start( ctrlBreakHandler ) ) return 1;

  char buf[50];
  printf( "Config file      %s\n", fn );
  printf( "Packet driver    INT 0x%02X, MAC %02X:%02X:%02X:%02X:%02X:%02X, MTU %u\n",
          Packet_getSoftwareInt( ), MyEthAddr[0], MyEthAddr[1], MyEthAddr[2],
          MyEthAddr[3], MyEthAddr[4], MyEthAddr[5], MyMTU );
  showKey( "Hostname", "HOSTNAME" );
  showKey( "Domain", "DOMAIN" );

  puts( "IPv4" );
  if ( Net::haveIpv4( ) ) {
    printf( "  %-14s %s", "Address", Addr::format4( MyIpAddr, buf ) );
    printf( " / %s\n", Addr::format4( Netmask, buf ) );
    printf( "  %-14s %s\n", "Gateway", Addr::format4( Gateway, buf ) );
    showKey( "Name server", "NAMESERVER" );
    showKey( "Lease (s)", "LEASE_TIME" );
  } else puts( "  no address (DHCP, or VMTCP IPADDR a.b.c.d)" );

  puts( "IPv6" );
  if ( !Ip6::enabled ) puts( "  off (VMTCP IPV6 ON)" );
  else {
    printf( "  %-14s %s\n", "Link-local", Addr::format6( Ip6::linkLocal, buf ) );
    if ( !Addr::isZero6( Ip6::global ) ) printf( "  %-14s %s/%u\n", "Address", Addr::format6( Ip6::global, buf ), Ip6::globalLen );
    else puts( "  Address        none (DHCP, or VMTCP IPV6ADDR addr/len)" );
    if ( !Addr::isZero6( Ip6::router ) ) printf( "  %-14s %s\n", "Gateway", Addr::format6( Ip6::router, buf ) );
    if ( !Addr::isZero6( Ip6::nameServer ) ) printf( "  %-14s %s\n", "Name server", Addr::format6( Ip6::nameServer, buf ) );

    if ( !quick ) {
      uint8_t savedRouter[16];
      memcpy( savedRouter, Ip6::router, 16 );
      memset( Ip6::router, 0, 16 );
      if ( Ip6::discoverRouter( 2000 ) ) {
        printf( "  Router says    %s, lifetime %us%s%s, MTU %u\n", Addr::format6( Ip6::router, buf ), Ip6::raLifetime,
                (Ip6::raFlags & 0x80) ? ", managed (DHCPv6)" : "", (Ip6::raFlags & 0x40) ? ", other info (DHCPv6)" : "", Ip6::mtu );
        for ( int i=0; i < Ip6::raPrefixes; i++ )
          printf( "    prefix       %s/%u%s%s\n", Addr::format6( Ip6::raPrefix[i].prefix, buf ), Ip6::raPrefix[i].len,
                  (Ip6::raPrefix[i].flags & 0x40) ? " autoconf" : "", (Ip6::raPrefix[i].flags & 0x80) ? " on-link" : "" );
        for ( int i=0; i < Ip6::raDnsCount; i++ )
          printf( "    DNS          %s\n", Addr::format6( Ip6::raDns[i], buf ) );
      } else puts( "  Router says    nothing (no IPv6 router answered)" );
      if ( !Addr::isZero6( savedRouter ) ) memcpy( Ip6::router, savedRouter, 16 );
    }
  }

  Net::stop( );
  return 0;
}
