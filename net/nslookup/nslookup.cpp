/*
   NSLOOKUP: DNS lookups over IPv4 or IPv6 (vmdos).

     NSLOOKUP name [server]            A and AAAA
     NSLOOKUP address [server]         reverse (PTR, in-addr.arpa / ip6.arpa)
     NSLOOKUP -type=MX name [server]   A AAAA CNAME MX NS PTR SOA TXT ANY ...

   The servers are NAMESERVER and IPV6NAMESERVER from the config file;
   a name without a dot is tried with DOMAIN appended first.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmnet.h"


static void __interrupt __far ctrlBreakHandler( ) { Net::breakFlag = 1; }

static ResolvResult res;


static void usage( void ) {
  puts( "NSLOOKUP: DNS lookups over IPv4 or IPv6\n\n"
        "  NSLOOKUP [-type=T] name [server]    T: A AAAA CNAME MX NS PTR SOA TXT ANY\n"
        "  NSLOOKUP address [server]           reverse lookup (IPv4 or IPv6)\n\n"
        "Without -type a name gets its A and AAAA records. The server is a name\n"
        "or address; the default is NAMESERVER / IPV6NAMESERVER from MTCPCFG." );
  exit( 1 );
}


static const char *rcodeText( int8_t rc ) {
  switch ( rc ) {
    case -1: return "no answer (timeout)";
    case -2: return "no usable name server";
    case -3: return "bad reply";
    case 1:  return "format error";
    case 2:  return "server failure";
    case 3:  return "no such name (NXDOMAIN)";
    case 4:  return "not implemented";
    case 5:  return "refused";
  }
  return "error";
}


// Prints the answer section (and, for a bare name lookup, nothing else).
// Returns the number of answers printed.
static int show( const char *qname, bool skipCname ) {
  int n = 0;
  for ( int i=0; i < res.count; i++ ) {
    ResolvRR *rr = &res.rr[i];
    if ( rr->section != 1 ) continue;
    if ( rr->type == DNS_TYPE_CNAME ) {
      if ( !skipCname ) printf( "%s\tcanonical name = %s\n", rr->name, rr->text );
      continue;
    }
    switch ( rr->type ) {
      case DNS_TYPE_A: case DNS_TYPE_AAAA:
        printf( "Name:    %s\nAddress: %s\n", rr->name, rr->text ); break;
      case DNS_TYPE_PTR:
        printf( "%s\tname = %s\n", rr->name, rr->text ); break;
      case DNS_TYPE_MX:
        printf( "%s\tmail exchanger = %s\n", rr->name, rr->text ); break;
      case DNS_TYPE_NS:
        printf( "%s\tnameserver = %s\n", rr->name, rr->text ); break;
      case DNS_TYPE_TXT:
        printf( "%s\ttext = %s\n", rr->name, rr->text ); break;
      case DNS_TYPE_SOA:
        printf( "%s\tSOA %s\n", rr->name, rr->text ); break;
      default:
        printf( "%s\t%s %s\n", rr->name, Resolv::typeName( rr->type ), rr->text );
    }
    n++;
  }
  return n;
}


static int8_t ask( const NetAddr *server, const char *name, uint16_t type ) {
  if ( server->family ) return Resolv::query( server, name, type, &res );
  return Resolv::queryAny( name, type, &res );
}


int main( int argc, char *argv[] ) {
  uint16_t qtype = 0;
  const char *name = NULL, *serverArg = NULL;

  for ( int i=1; i < argc; i++ ) {
    if ( argv[i][0] == '-' || argv[i][0] == '/' ) {
      const char *o = argv[i] + 1;
      if ( strnicmp( o, "type=", 5 ) == 0 || strnicmp( o, "q=", 2 ) == 0 || strnicmp( o, "querytype=", 10 ) == 0 ) {
        qtype = Resolv::typeFromName( strchr( o, '=' ) + 1 );
        if ( !qtype ) { fprintf( stderr, "Unknown type: %s\n", strchr( o, '=' ) + 1 ); return 1; }
      } else usage( );
    }
    else if ( !name ) name = argv[i];
    else if ( !serverArg ) serverArg = argv[i];
    else usage( );
  }
  if ( !name ) usage( );

  if ( Net::start( ctrlBreakHandler ) ) return 1;
  Resolv::loadConfig( );

  char buf[50];
  NetAddr server;
  memset( &server, 0, sizeof(server) );
  if ( serverArg ) {
    if ( Resolv::resolve( serverArg, 0, false, &server ) ) {
      fprintf( stderr, "Can't find server %s\n", serverArg );
      Net::stop( );
      return 1;
    }
  } else if ( Resolv::serverCount == 0 ) {
    fprintf( stderr, "No name server: set NAMESERVER or IPV6NAMESERVER (DHCP does), or give one\n" );
    Net::stop( );
    return 1;
  }

  int rc = 0;
  NetAddr lit;
  char qname[ RESOLV_NAME_LEN ];

  if ( Addr::parse( name, &lit ) && (qtype == 0 || qtype == DNS_TYPE_PTR) ) {
    Resolv::reverseName( &lit, qname );
    qtype = DNS_TYPE_PTR;
  } else {
    strncpy( qname, name, sizeof(qname) - 1 ); qname[sizeof(qname)-1] = 0;
  }

  // A name without a dot: DOMAIN first, then as given.
  const char *tries[2];
  char fq[ RESOLV_NAME_LEN ];
  int ntries = 0;
  if ( !strchr( qname, '.' ) && Resolv::domain[0] ) {
    snprintf( fq, sizeof(fq), "%s.%s", qname, Resolv::domain );
    tries[ ntries++ ] = fq;
  }
  tries[ ntries++ ] = qname;

  uint16_t types[2];
  int ntypes = 0;
  if ( qtype ) types[ ntypes++ ] = qtype;
  else { types[ ntypes++ ] = DNS_TYPE_A; types[ ntypes++ ] = DNS_TYPE_AAAA; }

  bool shownServer = false, found = false;
  int8_t lastRc = 0;
  for ( int t=0; t < ntries && !found && !Net::breakFlag; t++ ) {
    int answers = 0;
    bool nx = false;
    for ( int k=0; k < ntypes && !Net::breakFlag; k++ ) {
      int8_t r = ask( &server, tries[t], types[k] );
      if ( !shownServer && r >= 0 ) {
        printf( "Server:  %s\n\n", Addr::format( &res.server, buf ) );
        shownServer = true;
        if ( !res.authoritative ) puts( "Non-authoritative answer:" );
      }
      lastRc = r;
      if ( r < 0 ) break;
      if ( r == 3 ) { nx = true; break; }
      if ( r != 0 ) break;
      answers += show( tries[t], k > 0 );
    }
    if ( answers ) found = true;
    else if ( lastRc == 0 && !nx && t == ntries - 1 ) {
      printf( "*** %s: no %s records\n", tries[t], qtype ? Resolv::typeName( qtype ) : "A or AAAA" );
      found = true;
      rc = 1;
    }
  }
  if ( !found ) {
    if ( Net::breakFlag ) puts( "Aborted" );
    else printf( "*** Can't find %s: %s\n", qname, rcodeText( lastRc ) );
    rc = 1;
  }

  Net::stop( );
  return rc;
}
