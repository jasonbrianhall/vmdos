/* vmdos: msghlpr.h and msghlpr.c are referenced by FDOS/format since commit
   00069b6 (Sep 2024) but were never committed upstream. Recreated from the
   two functions that commit moved out of userint.c, unchanged. */
#ifndef MSGHLPR_H
#define MSGHLPR_H
#include "kitten.h"
void Key_For_Next_Page(void);
void Print_Messages_With_Pauses(nl_catd catalog, int setnum, char const * const messages[]);
#endif
