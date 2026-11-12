/* vmdos: see msghlpr.h. Key_For_Next_Page and Print_Messages_With_Pauses,
   as FDOS/format userint.c had them before commit 00069b6. */
#include <stdio.h>
#include <dos.h>
#include <io.h>		/* isatty */
#include <string.h>	/* memcmp */

#include "format.h"
#include "userint.h"
#include "msghlpr.h"

void Key_For_Next_Page()
{
  if (!isatty(1))
    return; /* redirected? then do not wait. */
    /* interesting: redirection to MORESYS (>MORE$) still is a TTY   */
    /* redirection to a file is not a TTY, so waiting is avoided :-) */

  printf(catgets(catalog, 14, 1, "-- press enter to see the next page or ESC to abort  --"));

  /* Get keypress */
  regs.h.ah = 0x07;
  intdos(&regs, &regs);
  if (regs.h.al == 27)
    {
      printf(catgets(catalog, 14, 2, "\nAborted at user request.\n"));
      Exit(3,13);
    }

  printf("\n\n");

} /* Key_For_Next_Page */


void Print_Messages_With_Pauses(nl_catd catalog, int setnum, char const * const messages[])
{
  int msgnum = 0;
  int use_cats = (catgets(catalog, setnum, 0, NULL) != NULL);
  char const * msg;

  for (msg=(use_cats)?catgets(catalog, setnum, msgnum, NULL):messages[msgnum];
       msg != NULL;
       msgnum++, msg=(use_cats)?catgets(catalog, setnum, msgnum, NULL):messages[msgnum])
  {
    printf("%s", msg);
	if (msgnum && !(msgnum % 22)) /* every 23rd line after initial line */
    {
      if (memcmp(msg, "\n", 2) != 0) printf("\n");  /* add blank unless just printed a blank line */
      Key_For_Next_Page();
    }
  }
} /* Print_Messages_With_Pauses */
