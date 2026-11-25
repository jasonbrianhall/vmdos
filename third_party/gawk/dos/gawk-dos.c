/* gawk-dos.c -- what gawk needs and DJGPP lacks (see gawk-dos.h): the
   character set for the regex engine, and the wide-character calls for a
   single-byte code page. */
#include "gawk-dos.h"
#include <langinfo.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>

char *
nl_langinfo (int item)
{
  return item == CODESET ? "CP437" : "";
}

size_t
mbrtowc (wchar_t *pwc, const char *s, size_t n, mbstate_t *ps)
{
  (void)ps;
  if (s == 0)
    return 0;
  if (n == 0)
    return (size_t)-2;
  if (pwc)
    *pwc = (unsigned char)*s;
  return *s != 0;
}

size_t
mbrlen (const char *s, size_t n, mbstate_t *ps)
{
  return mbrtowc (0, s, n, ps);
}

size_t
wcrtomb (char *s, wchar_t wc, mbstate_t *ps)
{
  (void)ps;
  if (s == 0)
    return 1;
  if ((unsigned)wc > 255)
    return (size_t)-1;
  *s = (char)wc;
  return 1;
}

wint_t btowc (int c) { return c == EOF ? WEOF : (wint_t)(unsigned char)c; }
int wctob (wint_t c) { return c < 256 ? (int)c : EOF; }
int mbsinit (const mbstate_t *ps) { (void)ps; return 1; }

#define BYTE(c) ((c) < 256)
wint_t towlower (wint_t c) { return BYTE (c) ? (wint_t)tolower ((int)c) : c; }
wint_t towupper (wint_t c) { return BYTE (c) ? (wint_t)toupper ((int)c) : c; }
int iswalnum (wint_t c) { return BYTE (c) && isalnum ((int)c); }
int iswupper (wint_t c) { return BYTE (c) && isupper ((int)c); }
int iswlower (wint_t c) { return BYTE (c) && islower ((int)c); }
int iswspace (wint_t c) { return BYTE (c) && isspace ((int)c); }
int iswprint (wint_t c) { return BYTE (c) && isprint ((int)c); }

static const char *const classes[] = {
  "", "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower",
  "print", "punct", "space", "upper", "xdigit", 0
};

wctype_t
wctype (const char *name)
{
  wctype_t i;
  for (i = 1; classes[i]; i++)
    if (strcmp (name, classes[i]) == 0)
      return i;
  return 0;
}

int
iswctype (wint_t c, wctype_t t)
{
  int b;
  if (!BYTE (c))
    return 0;
  b = (int)c;
  switch (t)
    {
    case 1: return isalnum (b);
    case 2: return isalpha (b);
    case 3: return b == ' ' || b == '\t';
    case 4: return iscntrl (b);
    case 5: return isdigit (b);
    case 6: return isgraph (b);
    case 7: return islower (b);
    case 8: return isprint (b);
    case 9: return ispunct (b);
    case 10: return isspace (b);
    case 11: return isupper (b);
    case 12: return isxdigit (b);
    }
  return 0;
}
