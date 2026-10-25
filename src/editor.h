/* editor.h -- Diamond's editing core, the same on the PicoCalc and on a PC.
 *
 * The core keeps the document, takes keys, and fills in a grid of character
 * cells (the screen); the platform reads the keyboard, draws the grid, and
 * loads and saves files (the ed_io_ functions below). */
#ifndef DIAMOND_EDITOR_H
#define DIAMOND_EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCR_COLS 64
#define SCR_ROWS 32
#define SCR_PX 320		/* the screen is 320 x 320 pixels */

/* what a cell is drawn as */
enum {
	A_TEXT, A_STATUS, A_MENU, A_KEY, A_RULER, A_MSG, A_BOX, A_BOXKEY,
	A_BLOCK,		/* inside the marked block */
	A_FOUND,		/* what find has just found */
	A_BOLD, A_ITALIC, A_BOLDITALIC,	/* the text's own formatting */
	A_H1, A_H2, A_H3,	/* headings (#, ##, ### and below) */
	A_CODE,			/* `code` */
	A_MARK,			/* a Markdown mark, when ^OD shows them */
	A_LINK,			/* a link's text */
	A_LINEMARK,		/* a quotation's >, drawn as a bar; a list's -, as a bullet */
	A_INBLOCK = 0x40,	/* added to a text style: inside the marked block */
	A_CURSOR = 0x80		/* added to any of them: the cursor is here */
};

typedef struct {
	char ch[SCR_ROWS][SCR_COLS];
	uint8_t at[SCR_ROWS][SCR_COLS];
} screen_t;

/* keys: printable ASCII as itself, Ctrl+letter as 1..26, and these */
enum {
	K_BS = 8, K_TAB = 9, K_ENTER = 13, K_ESC = 27,
	K_UP = 0x100, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN,
	K_DEL, K_INS,
	K_SHOT,			/* Shift+F5: a screenshot */
	K_NONE = -1		/* ed_key_wait() timed out */
};
#define CTRL(c) ((c) & 0x1F)

/* what ed_run() ends with */
enum { ED_CLOSE, ED_EXIT };	/* back to the file list; out to the shell */

/* ---- the platform provides these ---- */

/* the next key, or K_NONE if none comes within ms (ms < 0: wait forever) */
int ed_key_wait(int ms);
/* draw the screen (only what changed since last time is worth drawing) */
void ed_show(const screen_t *s);
/* the reading view draws pixels itself: w x h RGB565 pixels, row by row */
void ed_blit(int x, int y, int w, int h, const uint16_t *px);
/* something else drew on the screen: the next ed_show() draws all of it */
void ed_redraw(void);
/* read the whole file into buf (cap bytes): true, or false and a message */
bool ed_io_load(const char *path, char *buf, size_t cap, size_t *len, const char **err);
/* write the document (two pieces, one after the other) in place of the
   file, keeping the old one as a .bak; NULL or a message */
const char *ed_io_save(const char *path, const char *a, size_t alen,
                       const char *b, size_t blen, bool crlf);
/* remember the help level (0..3) for next time */
void ed_save_help_level(int level);
/* save the screen as a picture and as text, with info (Diamond's state)
   added to the text; the files' name without its ending, or NULL */
const char *ed_screenshot(const screen_t *s, const char *info);
/* the same, for a screen the reading view drew: the pixels as they are on
   the screen, and the info as the text */
const char *ed_screenshot_pixels(const char *info);

/* ---- the core ---- */

/* edit path, whose text is the first len bytes of buf; buf holds cap bytes,
   which is as big as the document can get.  Returns ED_CLOSE or ED_EXIT. */
int ed_run(const char *path, char *buf, size_t len, size_t cap, int help_level);

#endif
