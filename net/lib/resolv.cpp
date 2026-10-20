/*
   vmdos network tools: a small DNS resolver (RFC 1035, AAAA from RFC 3596)
   over UDP on IPv4 or IPv6. Servers: NAMESERVER (IPv4) and IPV6NAMESERVER
   from the config file; DOMAIN is appended to names without a dot.
*/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmnet.h"
#include "timer.h"
#include "utils.h"


NetAddr Resolv::servers[3];
uint8_t Resolv::serverCount = 0;
char    Resolv::domain[64] = "";

#define DNS_PORT 53

static uint8_t  qbuf[ 300 ];
static uint8_t  rbuf[ 1232 ];
static uint16_t rlen;
static bool     got;
static uint16_t wantId;
static uint16_t localPort;


void Resolv::loadConfig( void ) {
  char v[ CFG_VAL_LEN ];
  serverCount = 0;
  if ( Cfg::get( "NAMESERVER_PREFERRED", v, sizeof(v) ) && Addr::parse( v, &servers[ serverCount ] ) ) serverCount++;
  if ( Cfg::get( "NAMESERVER", v, sizeof(v) ) && Addr::parse( v, &servers[ serverCount ] ) ) serverCount++;
  if ( Ip6::enabled && !Addr::isZero6( Ip6::nameServer ) ) {
    servers[ serverCount ].family = 6;
    memcpy( servers[ serverCount ].a, Ip6::nameServer, 16 );
    serverCount++;
  }
  // Without IPv4 the IPv4 servers are no use: move IPv6 ones first.
  if ( !Net::haveIpv4( ) ) {
    NetAddr tmp[3]; uint8_t n = 0;
    for ( int i=0; i < serverCount; i++ ) if ( servers[i].family == 6 ) tmp[n++] = servers[i];
    for ( int i=0; i < serverCount; i++ ) if ( servers[i].family == 4 ) tmp[n++] = servers[i];
    memcpy( servers, tmp, sizeof(tmp) );
  }
  if ( Cfg::get( "DOMAIN", v, sizeof(v) ) ) { strncpy( domain, v, sizeof(domain)-1 ); domain[sizeof(domain)-1] = 0; }
}


static const struct { uint16_t t; const char *n; } typeNames[] = {
  { DNS_TYPE_A, "A" }, { DNS_TYPE_NS, "NS" }, { DNS_TYPE_CNAME, "CNAME" }, { DNS_TYPE_SOA, "SOA" },
  { DNS_TYPE_PTR, "PTR" }, { DNS_TYPE_MX, "MX" }, { DNS_TYPE_TXT, "TXT" }, { DNS_TYPE_AAAA, "AAAA" },
  { 33, "SRV" }, { DNS_TYPE_ANY, "ANY" }, { 0, NULL }
};

const char *Resolv::typeName( uint16_t t ) {
  static char buf[12];
  for ( int i=0; typeNames[i].n; i++ ) if ( typeNames[i].t == t ) return typeNames[i].n;
  sprintf( buf, "TYPE%u", t );
  return buf;
}

uint16_t Resolv::typeFromName( const char *s ) {
  for ( int i=0; typeNames[i].n; i++ ) if ( stricmp( typeNames[i].n, s ) == 0 ) return typeNames[i].t;
  if ( strnicmp( s, "TYPE", 4 ) == 0 ) return (uint16_t)atoi( s + 4 );
  return 0;
}


void Resolv::reverseName( const NetAddr *a, char *out ) {
  if ( a->family == 4 ) {
    sprintf( out, "%u.%u.%u.%u.in-addr.arpa", a->a[3], a->a[2], a->a[1], a->a[0] );
    return;
  }
  char *p = out;
  for ( int i=15; i >= 0; i-- ) p += sprintf( p, "%x.%x.", a->a[i] & 15, a->a[i] >> 4 );
  strcpy( p, "ip6.arpa" );
}


static void dnsHandler( const NetAddr *src, uint16_t srcPort, uint16_t dstPort, uint8_t *data, uint16_t len ) {
  if ( srcPort != DNS_PORT || len < 12 || got ) return;
  if ( ((data[0] << 8) | data[1]) != wantId ) return;
  if ( len > sizeof(rbuf) ) len = sizeof(rbuf);
  memcpy( rbuf, data, len );
  rlen = len;
  got = true;
}


// Reads a (possibly compressed) name at off; returns the offset after it
// in the record, or 0 if broken.
static uint16_t readName( uint16_t off, char *out, uint16_t outLen ) {
  uint16_t end = 0;
  uint16_t o = 0;
  int jumps = 0;
  out[0] = 0;
  while ( 1 ) {
    if ( off >= rlen ) return 0;
    uint8_t l = rbuf[off];
    if ( l == 0 ) { if ( !end ) end = off + 1; break; }
    if ( (l & 0xc0) == 0xc0 ) {
      if ( off + 1 >= rlen || ++jumps > 20 ) return 0;
      if ( !end ) end = off + 2;
      off = ((l & 0x3f) << 8) | rbuf[off+1];
      continue;
    }
    if ( off + 1 + l > rlen ) return 0;
    for ( int i=0; i < l; i++ ) {
      if ( o < outLen - 2 ) out[o++] = rbuf[off + 1 + i];
    }
    if ( o < outLen - 2 ) out[o++] = '.';
    off += l + 1;
  }
  if ( o == 0 ) out[o++] = '.';
  else if ( o > 1 ) o--;                        // no trailing dot
  out[o] = 0;
  return end;
}


static uint16_t buildQuery( const char *name, uint16_t qtype ) {
  wantId = (uint16_t)rand( );
  uint8_t *p = qbuf;
  p[0] = (uint8_t)(wantId >> 8); p[1] = (uint8_t)wantId;
  p[2] = 0x01; p[3] = 0x00;                     // RD
  p[4] = 0; p[5] = 1; p[6] = p[7] = p[8] = p[9] = p[10] = p[11] = 0;
  p += 12;
  const char *s = name;
  while ( *s ) {
    const char *dot = strchr( s, '.' );
    uint16_t l = dot ? (uint16_t)(dot - s) : (uint16_t)strlen( s );
    if ( l == 0 || l > 63 || (p - qbuf) + l + 6 > (int)sizeof(qbuf) ) return 0;
    *p++ = (uint8_t)l;
    memcpy( p, s, l ); p += l;
    s += l;
    if ( *s == '.' ) s++;
  }
  *p++ = 0;
  *p++ = (uint8_t)(qtype >> 8); *p++ = (uint8_t)qtype;
  *p++ = 0; *p++ = 1;
  return (uint16_t)(p - qbuf);
}


static void rdataText( ResolvRR *rr, uint16_t off, uint16_t len ) {
  char *t = rr->text;
  t[0] = 0;
  switch ( rr->type ) {
    case DNS_TYPE_A:
      if ( len == 4 ) { memcpy( rr->raw, rbuf + off, 4 ); Addr::format4( rr->raw, t ); }
      break;
    case DNS_TYPE_AAAA:
      if ( len == 16 ) { memcpy( rr->raw, rbuf + off, 16 ); Addr::format6( rr->raw, t ); }
      break;
    case DNS_TYPE_NS: case DNS_TYPE_CNAME: case DNS_TYPE_PTR:
      readName( off, t, RESOLV_DATA_LEN );
      break;
    case DNS_TYPE_MX: {
      uint16_t pref = (rbuf[off] << 8) | rbuf[off+1];
      int n = sprintf( t, "%u ", pref );
      readName( off + 2, t + n, RESOLV_DATA_LEN - n );
      break;
    }
    case DNS_TYPE_SOA: {
      char a[64], b[64];
      uint16_t o = readName( off, a, sizeof(a) );
      if ( o ) o = readName( o, b, sizeof(b) );
      if ( o && o + 20 <= off + len ) {
        uint32_t serial = ((uint32_t)rbuf[o] << 24) | ((uint32_t)rbuf[o+1] << 16) | (rbuf[o+2] << 8) | rbuf[o+3];
        sprintf( t, "%.48s %.48s %lu", a, b, serial );
      }
      break;
    }
    case DNS_TYPE_TXT: {
      uint16_t o = off, n = 0;
      while ( o < off + len && n < RESOLV_DATA_LEN - 4 ) {
        uint8_t l = rbuf[o++];
        if ( n ) t[n++] = ' ';
        t[n++] = '"';
        for ( int i=0; i < l && o < off + len && n < RESOLV_DATA_LEN - 3; i++, o++ )
          t[n++] = isprint( rbuf[o] ) ? rbuf[o] : '?';
        t[n++] = '"';
      }
      t[n] = 0;
      break;
    }
    default: {
      int n = sprintf( t, "\\# %u", len );
      for ( uint16_t i=0; i < len && n < RESOLV_DATA_LEN - 3; i++ ) n += sprintf( t + n, "%s%02x", i ? "" : " ", rbuf[off+i] );
    }
  }
}


static int8_t parseReply( ResolvResult *res ) {
  if ( rlen < 12 || !(rbuf[2] & 0x80) ) return -3;
  res->rcode = rbuf[3] & 0x0f;
  res->authoritative = (rbuf[2] & 0x04) != 0;
  res->truncated = (rbuf[2] & 0x02) != 0;
  uint16_t qd = (rbuf[4] << 8) | rbuf[5];
  uint16_t an = (rbuf[6] << 8) | rbuf[7];
  uint16_t ns = (rbuf[8] << 8) | rbuf[9];
  uint16_t ar = (rbuf[10] << 8) | rbuf[11];
  uint16_t off = 12;
  char tmp[ RESOLV_NAME_LEN ];
  for ( uint16_t i=0; i < qd; i++ ) {
    off = readName( off, tmp, sizeof(tmp) );
    if ( !off || off + 4 > rlen ) return -3;
    off += 4;
  }
  uint16_t total = an + ns + ar;
  for ( uint16_t i=0; i < total; i++ ) {
    ResolvRR scratch;
    ResolvRR *rr = res->count < RESOLV_MAX_RR ? &res->rr[ res->count ] : &scratch;
    off = readName( off, rr->name, RESOLV_NAME_LEN );
    if ( !off || off + 10 > rlen ) break;
    rr->type = (rbuf[off] << 8) | rbuf[off+1];
    rr->ttl = ((uint32_t)rbuf[off+4] << 24) | ((uint32_t)rbuf[off+5] << 16) | (rbuf[off+6] << 8) | rbuf[off+7];
    uint16_t dlen = (rbuf[off+8] << 8) | rbuf[off+9];
    off += 10;
    if ( off + dlen > rlen ) break;
    rr->section = i < an ? 1 : (i < an + ns ? 2 : 3);
    if ( rr->type != 41 ) {                     // OPT
      rdataText( rr, off, dlen );
      if ( rr != &scratch ) res->count++;
    }
    off += dlen;
  }
  return res->rcode;
}


int8_t Resolv::query( const NetAddr *server, const char *name, uint16_t qtype, ResolvResult *res ) {
  memset( res, 0, sizeof(*res) );
  res->server = *server;
  res->rcode = -1;
  uint16_t qlen = buildQuery( name, qtype );
  if ( !qlen ) { res->rcode = -3; return -3; }

  localPort = 0xC000 + (rand( ) & 0x3fff);
  got = false;
  if ( Net::listen( localPort, dnsHandler ) ) { res->rcode = -2; return -2; }

  for ( int attempt = 0; attempt < 3 && !got && !Net::breakFlag; attempt++ ) {
    if ( Net::sendUdpWait( server, localPort, DNS_PORT, qbuf, qlen, 2000 ) ) {
      res->rcode = -2;
      break;
    }
    clockTicks_t start = TIMER_GET_CURRENT( );
    while ( !got && !Net::breakFlag && Timer_diff( start, TIMER_GET_CURRENT( ) ) < TIMER_MS_TO_TICKS( 2000ul ) ) {
      Net::poll( );
    }
  }
  Net::unlisten( localPort );
  if ( !got ) return res->rcode;
  return parseReply( res );
}


int8_t Resolv::queryAny( const char *name, uint16_t qtype, ResolvResult *res ) {
  int8_t rc = -2;
  if ( serverCount == 0 ) { memset( res, 0, sizeof(*res) ); res->rcode = -2; return -2; }
  for ( int i=0; i < serverCount; i++ ) {
    if ( servers[i].family == 4 && !Net::haveIpv4( ) ) continue;
    if ( servers[i].family == 6 && !Ip6::enabled ) continue;
    rc = query( &servers[i], name, qtype, res );
    if ( rc >= 0 ) return rc;
    if ( Net::breakFlag ) break;
  }
  return rc;
}


static bool findAddr( const ResolvResult *res, uint16_t type, NetAddr *out ) {
  for ( int i=0; i < res->count; i++ ) {
    if ( res->rr[i].section == 1 && res->rr[i].type == type ) {
      out->family = type == DNS_TYPE_A ? 4 : 6;
      memcpy( out->a, res->rr[i].raw, type == DNS_TYPE_A ? 4 : 16 );
      return true;
    }
  }
  return false;
}


int8_t Resolv::resolve( const char *name, uint8_t family, bool prefer6, NetAddr *out ) {
  if ( Addr::parse( name, out ) ) {
    if ( family && out->family != family ) return -1;
    return 0;
  }
  static ResolvResult res;
  char fq[ RESOLV_NAME_LEN ];
  const char *tries[2];
  int ntries = 0;
  if ( !strchr( name, '.' ) && domain[0] ) {
    snprintf( fq, sizeof(fq), "%s.%s", name, domain );
    tries[ ntries++ ] = fq;
  }
  tries[ ntries++ ] = name;

  uint16_t order[2];
  int norder = 0;
  bool want4 = family != 6 && Net::haveIpv4( );
  bool want6 = family != 4 && Ip6::enabled;
  if ( prefer6 ) { if ( want6 ) order[norder++] = DNS_TYPE_AAAA; if ( want4 ) order[norder++] = DNS_TYPE_A; }
  else           { if ( want4 ) order[norder++] = DNS_TYPE_A; if ( want6 ) order[norder++] = DNS_TYPE_AAAA; }

  for ( int t=0; t < ntries; t++ ) {
    for ( int o=0; o < norder; o++ ) {
      int8_t rc = queryAny( tries[t], order[o], &res );
      if ( rc < 0 ) return rc;
      if ( rc == 0 && findAddr( &res, order[o], out ) ) return 0;
      if ( Net::breakFlag ) return -1;
    }
  }
  return 3;                                      // NXDOMAIN-ish: nothing found
}
