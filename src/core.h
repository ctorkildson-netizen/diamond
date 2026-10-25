/* core.h -- what the editing core shares with the reading view (reader.c).
 * Not for the platform: that's editor.h. */
#ifndef DIAMOND_CORE_H
#define DIAMOND_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* the document */
size_t doc_len(void);
char doc_at(size_t i);
/* what Markdown makes of byte i (the F_ flags below) */
uint8_t doc_fmt(size_t i);
/* a plain text file (not Markdown): nothing in it is formatting */
bool doc_plain(void);

enum { F_HIDDEN = 1, F_BOLD = 2, F_ITAL = 4, F_CODE = 8, F_OPEN = 16 /* an opening mark */, F_HEAD = 32 /* x level */ };
#define F_LINK F_OPEN		/* on a shown byte: the text of a link */

/* the reading view (or, if page_view, page view), from the paragraph holding
   position pos: returns the position to put the cursor at when it closes
   (the start of the paragraph at the top of the screen) */
size_t reader_run(size_t pos, const char *name, bool page_view);

#endif
