/*
   vmdos network tools: address parsing and printing, the config file.
*/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <io.h>

#include "vmnet.h"


bool Addr::isZero6( const uint8_t *a ) {
  for ( int i=0; i < 16; i++ ) if ( a[i] ) return false;
  return true;
}

bool Addr::same6( const uint8_t *a, const uint8_t *b ) {
  return memcmp( a, b, 16 ) == 0;
}

bool Addr::prefixMatch( const uint8_t *a, const uint8_t *b, uint8_t bits ) {
  uint8_t i = 0;
  for ( ; bits >= 8; bits -= 8, i++ ) if ( a[i] != b[i] ) return false;
  if ( bits ) {
    uint8_t m = (uint8_t)(0xff << (8 - bits));
    if ( (a[i] & m) != (b[i] & m) ) return false;
  }
  return true;
}


static bool parse4( const char *s, uint8_t *out, const char **end ) {
  for ( int i=0; i < 4; i++ ) {
    if ( !isdigit( *s ) ) return false;
    unsigned v = 0;
    int n = 0;
    while ( isdigit( *s ) ) { v = v*10 + (*s++ - '0'); if ( ++n > 3 ) return false; }
    if ( v > 255 ) return false;
    out[i] = (uint8_t)v;
    if ( i < 3 ) { if ( *s != '.' ) return false; s++; }
  }
  if ( end ) *end = s; else if ( *s ) return false;
  return true;
}


static int hexval( char c ) {
  if ( c >= '0' && c <= '9' ) return c - '0';
  c = (char)tolower( c );
  if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
  return -1;
}


// RFC 4291 text forms: full, "::" compressed, IPv4 tail (::ffff:1.2.3.4).
// A zone ("%1") is accepted and ignored: there is only one interface.
bool Addr::parse6( const char *s, uint8_t *out ) {
  uint16_t w[8];
  int n = 0, gap = -1;
  const char *p = s;

  memset( out, 0, 16 );
  if ( p[0] == ':' ) {
    if ( p[1] != ':' ) return false;
    gap = 0; p += 2;
    if ( *p == 0 || *p == '%' ) { return true; }
  }
  while ( 1 ) {
    // An IPv4 tail?
    const char *q = p;
    while ( isdigit( *q ) ) q++;
    if ( *q == '.' ) {
      uint8_t v4[4];
      const char *e;
      if ( n > 6 || !parse4( p, v4, &e ) ) return false;
      if ( *e && *e != '%' ) return false;
      w[n++] = (v4[0] << 8) | v4[1];
      w[n++] = (v4[2] << 8) | v4[3];
      break;
    }
    unsigned v = 0;
    int digits = 0;
    while ( hexval( *p ) >= 0 ) { v = (v << 4) | hexval( *p++ ); if ( ++digits > 4 ) return false; }
    if ( digits == 0 ) return false;
    if ( n == 8 ) return false;
    w[n++] = (uint16_t)v;
    if ( *p == 0 || *p == '%' ) break;
    if ( *p != ':' ) return false;
    p++;
    if ( *p == ':' ) {
      if ( gap >= 0 ) return false;
      gap = n; p++;
      if ( *p == 0 || *p == '%' ) break;
    }
  }
  if ( gap < 0 && n != 8 ) return false;
  if ( gap >= 0 && n > 7 ) return false;
  int fill = 8 - n;
  int o = 0;
  for ( int i=0; i < n; i++ ) {
    if ( i == gap ) o += fill;
    out[o*2] = (uint8_t)(w[i] >> 8); out[o*2+1] = (uint8_t)w[i];
    o++;
  }
  return true;
}


bool Addr::parsePrefix6( const char *s, uint8_t *out, uint8_t *len ) {
  char buf[64];
  strncpy( buf, s, sizeof(buf) - 1 ); buf[sizeof(buf)-1] = 0;
  char *slash = strchr( buf, '/' );
  *len = 64;
  if ( slash ) {
    *slash = 0;
    int l = atoi( slash + 1 );
    if ( l < 1 || l > 128 ) return false;
    *len = (uint8_t)l;
  }
  return parse6( buf, out );
}


bool Addr::parse( const char *s, NetAddr *out ) {
  memset( out, 0, sizeof(*out) );
  if ( parse4( s, out->a, NULL ) ) { out->family = 4; return true; }
  if ( strchr( s, ':' ) && parse6( s, out->a ) ) { out->family = 6; return true; }
  return false;
}


char *Addr::format4( const uint8_t *a, char *buf ) {
  sprintf( buf, "%u.%u.%u.%u", a[0], a[1], a[2], a[3] );
  return buf;
}


// RFC 5952: lower case, no leading zeros, the longest run (2+) of zero
// groups as "::".
char *Addr::format6( const uint8_t *a, char *buf ) {
  uint16_t w[8];
  for ( int i=0; i < 8; i++ ) w[i] = (a[i*2] << 8) | a[i*2+1];
  int best = -1, bestLen = 0;
  for ( int i=0; i < 8; ) {
    if ( w[i] == 0 ) {
      int j = i;
      while ( j < 8 && w[j] == 0 ) j++;
      if ( j - i > bestLen && j - i >= 2 ) { best = i; bestLen = j - i; }
      i = j;
    } else i++;
  }
  char *p = buf;
  // IPv4-mapped
  if ( best == 0 && bestLen == 5 && w[5] == 0xffff ) {
    sprintf( buf, "::ffff:%u.%u.%u.%u", a[12], a[13], a[14], a[15] );
    return buf;
  }
  for ( int i=0; i < 8; i++ ) {
    if ( i == best ) {
      *p++ = ':';
      if ( i == 0 ) *p++ = ':';
      i += bestLen - 1;
      continue;
    }
    p += sprintf( p, "%x", w[i] );
    if ( i < 7 ) *p++ = ':';
  }
  *p = 0;
  return buf;
}


char *Addr::format( const NetAddr *a, char *buf ) {
  if ( a->family == 4 ) return format4( a->a, buf );
  if ( a->family == 6 ) return format6( a->a, buf );
  strcpy( buf, "(none)" );
  return buf;
}


// ---------------------------------------------------------------------------
// Config file

const char *Cfg::fileName( void ) {
  return getenv( "MTCPCFG" );
}


// Splits "KEY rest": key into k (upper case kept as is), returns the value.
static char *splitLine( char *line, char *k, int klen ) {
  char *p = line;
  while ( *p == ' ' || *p == '\t' ) p++;
  int i = 0;
  while ( *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' ) {
    if ( i < klen - 1 ) k[i++] = *p;
    p++;
  }
  k[i] = 0;
  while ( *p == ' ' || *p == '\t' ) p++;
  char *v = p;
  char *e = v + strlen( v );
  while ( e > v && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t') ) *--e = 0;
  return v;
}


bool Cfg::get( const char *key, char *val, uint16_t len ) {
  const char *fn = fileName( );
  *val = 0;
  if ( !fn ) return false;
  FILE *f = fopen( fn, "r" );
  if ( !f ) return false;
  char line[160], k[40];
  bool found = false;
  while ( fgets( line, sizeof(line), f ) ) {
    char *v = splitLine( line, k, sizeof(k) );
    if ( stricmp( k, key ) == 0 ) {
      strncpy( val, v, len - 1 ); val[len-1] = 0;
      found = true;
      break;
    }
  }
  fclose( f );
  return found;
}


int8_t Cfg::update( const char * const *drop, const char * const *add ) {
  const char *fn = fileName( );
  if ( !fn ) { fprintf( stderr, "MTCPCFG is not set\n" ); return -1; }

  // The new file goes next to the old one.
  char tmp[ DOS_MAX_PATHFILE_LENGTH ];
  char drive[3], dir[ DOS_MAX_PATH_LENGTH ];
  if ( _fullpath( tmp, fn, sizeof(tmp) ) == NULL ) return -1;
  _splitpath( tmp, drive, dir, NULL, NULL );
  _makepath( tmp, drive, dir, "vmtcpcfg", "tmp" );

  FILE *in = fopen( fn, "r" );
  FILE *out = fopen( tmp, "w" );
  if ( !out ) { if ( in ) fclose( in ); fprintf( stderr, "Can't write %s\n", tmp ); return -1; }

  char line[160], copy[160], k[40];
  if ( in ) {
    while ( fgets( line, sizeof(line), in ) ) {
      strcpy( copy, line );
      splitLine( copy, k, sizeof(k) );
      bool skip = false;
      for ( int i=0; drop && drop[i]; i++ ) if ( stricmp( k, drop[i] ) == 0 ) { skip = true; break; }
      if ( skip ) continue;
      size_t l = strlen( line );
      if ( l && line[l-1] != '\n' ) { line[l] = '\n'; line[l+1] = 0; }
      fputs( line, out );
    }
    fclose( in );
  }
  for ( int i=0; add && add[i]; i++ ) fprintf( out, "%s\n", add[i] );
  if ( fclose( out ) ) { fprintf( stderr, "Error writing %s\n", tmp ); return -1; }
  unlink( fn );
  if ( rename( tmp, fn ) ) { fprintf( stderr, "Can't rename %s to %s\n", tmp, fn ); return -1; }
  return 0;
}
