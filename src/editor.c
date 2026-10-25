/* editor.c -- Diamond's editing core: the document, the WordStar keys, and
 * the screen.
 *
 * The document is a gap buffer: one block of memory with the text before the
 * cursor at the front, the text after it at the back, and the free space (the
 * gap) between, so typing at the cursor costs nothing.  Lines are soft-wrapped
 * at WRAP columns as they are drawn; nothing about the wrapping is stored.
 * Text is UTF-8: a character is a lead byte plus any continuation bytes, and
 * only lead bytes take a column. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "editor.h"
#include "core.h"

#define WRAP 63			/* text columns; the 64th takes the cursor at a line's end */
#define LINES_PER_PAGE 54	/* for P and L in the status line: a Letter page */
#define UNERASE_MAX 32768	/* ^U brings back the last deletion up to this size */
#define NOWHERE ((size_t)-1)	/* a block end or place marker not set */

/* ------------------------------------------------------------ the text */

static char *buf;
static size_t cap, gs, ge;	/* text is buf[0..gs) then buf[ge..cap) */

static size_t len(void) { return cap - (ge - gs); }
static char at(size_t i) { return i < gs ? buf[i] : buf[i + (ge - gs)]; }

static void move_gap(size_t pos)
{
	if (pos < gs) {
		size_t n = gs - pos;
		memmove(buf + ge - n, buf + pos, n);
		gs -= n;
		ge -= n;
	} else if (pos > gs) {
		size_t n = pos - gs;
		memmove(buf + gs, buf + ge, n);
		gs += n;
		ge += n;
	}
}

/* room for n more bytes?  The buffer never grows: it's all the memory the
   platform could give at the start, so when it's full, it's full. */
static bool make_room(size_t n) { return ge - gs >= n; }

static int percent_full(void) { return (int)((len() * 100 + cap - 1) / cap); }

static bool cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

/* ------------------------------------------------------------ state */

static struct {
	const char *path;
	size_t cur;		/* the cursor: a position in the text */
	size_t top;		/* start of the line at the top of the screen */
	int want;		/* the column Up and Down try to keep */
	bool modified, overtype, crlf;
	int help;		/* help level, 0..3 */
	char msg[SCR_COLS + 1];	/* shown in place of the status line once */
	char *unerase;		/* the last deletion, for ^U */
	size_t unerase_len;
	unsigned cmd;		/* counts the keys handled */
	unsigned kill_cmd;	/* the key that last added to unerase */
	size_t kill_pos;	/* where that deletion was */
	unsigned edits;		/* counts changes to the text (for the format cache) */
	bool marks;		/* ^OD: show the Markdown marks */
	bool plain;		/* a plain text file (.txt): no Markdown at all
				   (WordStar's "non-document" mode) */
	int pend;		/* ^PB/^PY with nothing marked: the next character typed */
	int carry;		/* a space typed at the end of bold went after its marks;
				   the next letter takes them along (still bold): the style */
	size_t bb, bk;		/* the block: ^KB and ^KK (NOWHERE: not set) */
	bool hide;		/* ^KH: the block isn't shown (or used) */
	size_t mark[10];	/* ^K0..^K9 */
	size_t hl_a, hl_b;	/* what find found, shown until the next key */
	char find[48], repl[48], opts[12];	/* the last search, for ^L */
	int last_search;	/* 0 none, 1 ^QF, 2 ^QA */
} E;

/* positions that must move when text goes in or comes out before them */
static size_t *pinned(int i)
{
	if (i == 0) return &E.bb;
	if (i == 1) return &E.bk;
	return &E.mark[i - 2];
}
#define PINNED 12

static void pins_insert(size_t pos, size_t n)
{
	for (int i = 0; i < PINNED; i++) {
		size_t *p = pinned(i);
		if (*p != NOWHERE && *p > pos) *p += n;
	}
}

static void pins_delete(size_t pos, size_t n)
{
	for (int i = 0; i < PINNED; i++) {
		size_t *p = pinned(i);
		if (*p == NOWHERE || *p <= pos) continue;
		*p = *p >= pos + n ? *p - n : pos;
	}
}

static bool block_ok(void) { return !E.hide && E.bb != NOWHERE && E.bk != NOWHERE && E.bb < E.bk; }

static screen_t scr;

/* for the status line's line number: how many screen lines come before
   paragraph start ln_anchor (counting from the top each time is slow in
   a long document; changes before the anchor make it start again) */
static size_t ln_anchor;
static int ln_count;

static void say(const char *m) { snprintf(E.msg, sizeof E.msg, "%s", m); }

static void insert(const char *s, size_t n)
{
	if (!make_room(n)) {
		say(" Memory is full: nothing more can go in. Save with ^KS.");
		return;
	}
	int before = percent_full();
	pins_insert(E.cur, n);
	E.edits++;
	if (E.cur < ln_anchor) ln_anchor = 0, ln_count = 0;
	move_gap(E.cur);
	memcpy(buf + gs, s, n);
	gs += n;
	E.cur += n;
	E.modified = true;
	if (before < 90 && percent_full() >= 90)
		say(" Memory is 90% full: time to save, and start a new document");
}

/* delete n bytes at pos; keep them for ^U if keep.  Deletions made one
   after another at the same place (^Y ^Y ^Y) are kept together. */
/* keep n deleted bytes (from src, or the text at pos) for ^U; spanning
   pos..pos+span in the text before they went */
static void keep_kill(size_t pos, size_t span, const char *src, size_t n)
{
	bool after = E.kill_cmd + 1 == E.cmd && pos == E.kill_pos;		/* ^Y ^Y */
	bool before = E.kill_cmd + 1 == E.cmd && pos + span == E.kill_pos;	/* ^Q Bksp twice */
	size_t old = after || before ? E.unerase_len : 0;
	size_t k = n < UNERASE_MAX - old ? n : UNERASE_MAX - old;
	char *u = realloc(E.unerase, old + k + 1);
	if (!u) return;
	if (before) memmove(u + k, u, old);
	for (size_t i = 0; i < k; i++) u[(before ? 0 : old) + i] = src ? src[i] : at(pos + i);
	E.unerase = u;
	E.unerase_len = old + k;
	E.kill_cmd = E.cmd;
	E.kill_pos = pos;
}

static void delete(size_t pos, size_t n, bool keep)
{
	if (!n) return;
	if (keep) keep_kill(pos, n, NULL, n);
	move_gap(pos);
	ge += n;
	E.edits++;
	if (pos < ln_anchor) ln_anchor = 0, ln_count = 0;
	if (E.cur > pos) E.cur = E.cur >= pos + n ? E.cur - n : pos;
	pins_delete(pos, n);
	E.modified = true;
}

/* ------------------------------------------------------------ formatting */

/* What Markdown makes of each byte of a paragraph (a line, up to its
   newline): whether it's a mark (hidden unless ^OD), and how it's styled.
   Worked out a paragraph at a time and kept until the text changes.

   Marks: # to ###### and a space at the start (a heading); *, **, *** or
   _, __, ___ around text (italic, bold, both: the CommonMark way of
   pairing them, simplified); `backticks` around code; a backslash before
   a mark character (the backslash).  A mark with no partner is left as
   text, so it shows. */
/* (the F_ flags are in core.h) */
#define F_MARK F_HIDDEN		/* every hidden byte is a mark */
/* on a * or _ mark (which is never drawn, so needs no heading level): whether
   it belongs to an italic pair or a bold one */
#define F_W1 32
#define F_W2 64
#define WIDTH(f) ((f) & F_W2 ? 2 : 1)
/* on a shown byte: the > of a quotation or the - * + of a list item, drawn
   as a bar or a bullet (a heading level that can't be) */
#define F_LINEMARK (7 * F_HEAD)
#define PARA_MAX 8192		/* longer paragraphs are shown as they are */

static uint8_t finfo[PARA_MAX];
static size_t f_start = (size_t)-1, f_len;
static unsigned f_edits;
static bool f_long;		/* the paragraph is too long: no formatting */

static bool alnum(char c) { return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || (c & 0x80); }
static bool space(char c) { return c == ' ' || c == '\t' || c == '\n'; }

static void parse_para(size_t ps, size_t n)
{
	memset(finfo, 0, n);
	#define C(i) ((i) < n ? at(ps + (i)) : '\n')
	size_t i = 0;

	/* a heading */
	int level = 0;
	while (level < 6 && C((size_t)level) == '#') level++;
	if (level && C((size_t)level) == ' ') {
		for (size_t k = 0; k < n; k++) finfo[k] = (uint8_t)(level * F_HEAD);
		for (size_t k = 0; k <= (size_t)level; k++) finfo[k] |= F_HIDDEN | F_MARK;
		i = level + 1;
	}
	size_t body = i;

	/* a quotation's >s, or a list item's -, * or + */
	if (!level) {
		size_t q = 0;
		while (q < n && q < 3 && C(q) == ' ') q++;
		if (C(q) == '>')
			while (q < n && C(q) == '>') {
				finfo[q] |= F_LINEMARK;
				q++;
				while (q < n && C(q) == ' ') q++;
			}
		else {
			q = 0;
			while (q < n && C(q) == ' ') q++;
			if ((C(q) == '-' || C(q) == '*' || C(q) == '+') && C(q + 1) == ' ') finfo[q] |= F_LINEMARK;
		}
	}

	/* code spans first: nothing inside them is a mark */
	for (size_t k = body; k < n; k++) {
		if (C(k) != '`' || (finfo[k] & F_CODE)) continue;
		size_t run = 1;
		while (C(k + run) == '`') run++;
		size_t j = k + run, end = (size_t)-1;
		while (j < n) {
			if (C(j) == '`') {
				size_t r = 1;
				while (C(j + r) == '`') r++;
				if (r == run) { end = j; break; }
				j += r;
			} else j++;
		}
		if (end == (size_t)-1) { k += run - 1; continue; }
		for (size_t q = k; q < end + run; q++) finfo[q] |= F_CODE;
		for (size_t q = 0; q < run; q++) finfo[k + q] |= F_HIDDEN | F_OPEN, finfo[end + q] |= F_HIDDEN;
		k = end + run - 1;
	}

	/* links: [text](address), on one line, with some text; the brackets
	   and the address are marks, the text is shown as a link */
	for (size_t k = body; k < n; k++) {
		if (C(k) != '[' || (finfo[k] & (F_CODE | F_HIDDEN))) continue;
		if (k > body && C(k - 1) == '\\') continue;		/* \[ is a plain [ */
		size_t j = k + 1;
		int depth = 0;
		while (j < n && !(finfo[j] & F_CODE)) {
			if (C(j) == '[') depth++;
			else if (C(j) == ']') { if (!depth) break; depth--; }
			j++;
		}
		if (j >= n || C(j) != ']' || C(j + 1) != '(' || j == k + 1) continue;
		size_t r = j + 2;
		while (r < n && C(r) != ')' && C(r) != ' ' && C(r) != '\t') r++;
		if (r >= n || C(r) != ')') continue;
		finfo[k] |= F_HIDDEN | F_OPEN;
		for (size_t q = k + 1; q < j; q++)
			if (!(finfo[q] & F_HIDDEN)) finfo[q] |= F_LINK;	/* (F_LINK is F_OPEN: not on marks) */
		for (size_t q = j; q <= r; q++) finfo[q] |= F_HIDDEN;
		k = r;
	}

	/* emphasis: runs of * or _, paired with a stack of openers */
	struct { size_t pos, len, run; char ch; bool both; } st[32];
	int sp = 0;
	for (size_t k = body; k < n; ) {
		char c = C(k);
		if (finfo[k] & F_CODE) { k++; continue; }
		if (finfo[k] & F_HIDDEN) { k++; continue; }	/* a link's address: no marks in it */
		if (c == '\\' && k + 1 < n && strchr("*_`#\\[]", C(k + 1))) {
			finfo[k] = (uint8_t)((finfo[k] & ~F_OPEN) | F_HIDDEN);	/* \* is a plain * */
			k += 2;
			continue;
		}
		if (c != '*' && c != '_') { k++; continue; }
		size_t run = 1;
		while (C(k + run) == c) run++;
		char before = k ? C(k - 1) : ' ', after = C(k + run);
		bool open = !space(after), close = k > body && !space(before);
		if (c == '_') { open = open && !alnum(before); close = close && !alnum(after); }
		size_t pos = k, left = run;
		if (close) {
			while (left) {
				/* CommonMark's "rule of 3": a run that could open or close
				   doesn't pair with one whose length adds up to a multiple
				   of 3 (so **a*b* reads as bold a, then italic b) */
				int o = sp - 1;
				while (o >= 0 && (st[o].ch != c ||
				       ((open || st[o].both) && (st[o].run + run) % 3 == 0 &&
				        !(st[o].run % 3 == 0 && run % 3 == 0)))) o--;
				if (o < 0) break;
				size_t use = left >= 2 && st[o].len >= 2 ? 2 : 1;
				size_t ob = st[o].pos + st[o].len - use, oe = st[o].pos + st[o].len;
				uint8_t w = use == 2 ? F_W2 : F_W1;
				for (size_t q = ob; q < oe; q++) finfo[q] = (uint8_t)((finfo[q] & 0x1F) | F_HIDDEN | F_OPEN | w);
				for (size_t q = pos; q < pos + use; q++) finfo[q] = (uint8_t)((finfo[q] & 0x1F & ~F_OPEN) | F_HIDDEN | w);
				for (size_t q = oe; q < pos; q++) finfo[q] |= use == 2 ? F_BOLD : F_ITAL;
				st[o].len -= use;
				sp = st[o].len ? o + 1 : o;	/* openers above it are unmatched */
				pos += use;
				left -= use;
			}
		}
		if (left && open && sp < 32) {
			st[sp].pos = pos;
			st[sp].len = left;
			st[sp].run = run;
			st[sp].both = close;
			st[sp].ch = c;
			sp++;
		}
		k += run;
	}
	#undef C
}

/* the formatting of byte i */
static uint8_t fmt(size_t i)
{
	if (i >= len() || E.plain) return 0;
	if (f_start == (size_t)-1 || f_edits != E.edits || i < f_start || i >= f_start + f_len) {
		size_t ps = i, pe = i;
		while (ps > 0 && at(ps - 1) != '\n') ps--;
		while (pe < len() && at(pe) != '\n') pe++;
		f_start = ps;
		f_len = pe - ps;
		f_edits = E.edits;
		f_long = f_len > PARA_MAX;
		if (!f_long) parse_para(ps, f_len);
		if (i >= pe) return 0;		/* the newline itself */
	}
	return f_long ? 0 : finfo[i - f_start];
}

size_t doc_len(void) { return len(); }
char doc_at(size_t i) { return at(i); }
uint8_t doc_fmt(size_t i) { return fmt(i); }
bool doc_plain(void) { return E.plain; }

/* Markdown is .md and .markdown; anything else (.txt) is plain text */
static bool is_markdown(const char *path)
{
	const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
	if (!dot || (slash && dot < slash)) return false;
	char e[10];
	size_t i = 0;
	for (dot++; *dot && i < sizeof e - 1; dot++) e[i++] = (char)(*dot | 0x20);
	e[i] = 0;
	return !strcmp(e, "md") || !strcmp(e, "markdown");
}

/* does byte i take a column on screen? */
static bool shown(size_t i)
{
	char c = at(i);
	if (cont(c)) return false;
	if (E.marks) return true;
	return !(fmt(i) & F_HIDDEN);
}

/* ------------------------------------------------------------ lines */

/* the start of the screen line after the one starting at s (len() at the end) */
static size_t next_line(size_t s)
{
	size_t n = len(), i = s, after_space = 0;
	int cols = 0;
	while (i < n) {
		char c = at(i);
		if (c == '\n') return i + 1;
		if (shown(i)) {
			if (cols == WRAP) {
				if (c == ' ') return i + 1;	/* the space ends this line, unseen */
				return after_space > s ? after_space : i;
			}
			cols++;
		}
		if (c == ' ') after_space = i + 1;
		i++;
	}
	return n;
}

/* does a screen line start at s?  (Only at the very end is there doubt:
   after a final newline there's an empty last line; otherwise there isn't.) */
static bool line_at(size_t s)
{
	return s < len() || s == 0 || at(len() - 1) == '\n';
}

static size_t para_start(size_t p)
{
	while (p > 0 && at(p - 1) != '\n') p--;
	return p;
}

/* the start of the screen line holding position p */
static size_t line_start(size_t p)
{
	size_t s = para_start(p);
	for (;;) {
		size_t e = next_line(s);
		if (e > p || e >= len()) return s;
		s = e;
	}
}

/* the last position the cursor can take on the line starting at s */
static size_t line_last(size_t s)
{
	size_t e = next_line(s);
	if (e == len() && (e == 0 || at(e - 1) != '\n')) return e;	/* the end of the text */
	return e > s ? e - 1 : s;
}

/* the column of position p on its line */
static int col_of(size_t s, size_t p)
{
	int c = 0;
	for (size_t i = s; i < p; i++)
		if (shown(i)) c++;
	return c;
}

/* The cursor is a position in the text, and hidden marks take no room
   on screen, so several positions can look the same: "**bold**" with the
   cursor on the b could be at the first *, the second or the b.  Moves put
   it before the marks (so typing at the end of "bold" carries on in bold);
   typing leaves it after what was typed. */

static bool hidden(size_t i) { char c = at(i); return c != '\n' && !cont(c) && !shown(i); }

/* where the cursor shows: the first shown character at or after p */
static size_t vis(size_t p)
{
	while (p < len() && hidden(p)) p++;
	return p;
}

/* the same place, before any hidden marks (but not before lo) */
static size_t canon(size_t p, size_t lo)
{
	while (p > lo && hidden(p - 1)) p--;
	return p;
}

/* the position at column col on the line starting at s (or its last one) */
static size_t pos_at(size_t s, int col)
{
	size_t last = line_last(s), i = s;
	for (int c = 0; i < last; i++) {
		if (!shown(i)) continue;
		if (c == col) break;
		c++;
	}
	return canon(i, s);
}

/* after the character the cursor shows on */
static size_t next_char(size_t p)
{
	size_t n = len();
	p = vis(p);
	if (p >= n) return n;
	if (at(p) == '\n') return p + 1;
	p++;
	while (p < n && cont(at(p))) p++;
	return p;
}

/* the character before (or the newline ending the line before) */
static size_t prev_char(size_t p)
{
	while (p > 0) {
		p--;
		if (at(p) == '\n' || shown(p)) return canon(p, 0);
	}
	return 0;
}

/* ------------------------------------------------------------ the screen layout */

static int text_row0(void) { return E.help >= 3 ? 9 : E.help >= 1 ? 2 : 1; }
static int text_rows(void) { return SCR_ROWS - text_row0(); }

/* how many screen lines from a down to b (a, b line starts, a <= b) */
static int lines_between(size_t a, size_t b)
{
	int n = 0;
	while (a < b) { a = next_line(a); n++; }
	return n;
}

/* the number of screen lines before line start ls */
static int line_number(size_t ls)
{
	size_t a = para_start(ls);
	if (a >= ln_anchor) ln_count += lines_between(ln_anchor, a);
	else ln_count -= lines_between(a, ln_anchor);
	ln_anchor = a;
#ifdef CHECK_LINES
	if (ln_count + lines_between(a, ls) != lines_between(0, ls)) abort();
#endif
	return ln_count + lines_between(a, ls);
}

/* keep the cursor's line on screen */
static void keep_in_view(void)
{
	size_t ls = line_start(E.cur);
	if (ls < E.top) { E.top = ls; return; }
	if (line_start(E.top) != E.top) E.top = line_start(E.top);	/* text changed above */
	int n = lines_between(E.top, ls), rows = text_rows();
	while (n >= rows) { E.top = next_line(E.top); n--; }
}

/* ------------------------------------------------------------ drawing */

static void put(int row, int col, int attr, const char *s)
{
	for (; *s && col < SCR_COLS; s++, col++) {
		scr.ch[row][col] = *s;
		scr.at[row][col] = (uint8_t)attr;
	}
}

static void clear_row(int row, int attr)
{
	memset(scr.ch[row], ' ', SCR_COLS);
	memset(scr.at[row], attr, SCR_COLS);
}

/* a menu line: words starting with ^ (and the next character) stand out */
static void put_menu(int row, int col, int attr, int keyattr, const char *s)
{
	for (; *s && col < SCR_COLS; col++, s++) {
		bool key = *s == '^' && s[1];
		scr.ch[row][col] = *s;
		scr.at[row][col] = (uint8_t)(key ? keyattr : attr);
		if (key && col + 1 < SCR_COLS) {
			s++; col++;
			scr.ch[row][col] = *s;
			scr.at[row][col] = (uint8_t)keyattr;
		}
	}
}

/* a character as the 5x10 font can show it */
static char glyph_at(size_t i)
{
	unsigned char c = (unsigned char)at(i);
	if (c == '\t') return ' ';
	if (c < 0x80) return c < 0x20 ? '.' : (char)c;
	/* UTF-8: decode, and show the common ones as their ASCII look-alikes */
	unsigned cp = 0;
	int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
	cp = c & (0x3F >> more);
	for (int k = 1; k <= more && i + k < len(); k++) cp = cp << 6 | (at(i + k) & 0x3F);
	switch (cp) {
	case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
	case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
	case 0x2013: case 0x2014: case 0x2212: return '-';
	case 0x2026: return '.';
	case 0x00A0: return ' ';
	case 0x2022: case 0x00B7: return '*';
	}
	if (cp >= 0xC0 && cp <= 0xFF) {		/* accented Latin letters, unaccented */
		static const char lat[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
		return lat[cp - 0xC0];
	}
	return '?';
}

static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

static const char *const main_menu[] = {
	"               < < <   M A I N   M E N U   > > >",
	" --Cursor Movement--    -Delete-    -Miscellaneous-   -Menus-",
	" ^S char left ^D right  ^G char     ^V insert on/off  ^J help",
	" ^A word left ^F right  Bksp left   ^N split line     ^K files",
	" ^E line up   ^X down   ^T word     ^U unerase        ^Q quick",
	" --Scrolling--          ^Y line     F5 screenshot     ^O options",
	" ^W up  ^Z down   ^R up a screen  ^C down a screen     ^P bold",
};
#define MAIN_MENU_ROWS 7

/* how a character with formatting f is drawn */
static int style_of(uint8_t f)
{
	if (E.marks && (f & F_MARK)) return A_MARK;
	if (!(f & F_HIDDEN) && (f & F_LINK)) return A_LINK;
	if (!(f & F_HIDDEN) && (f & F_LINEMARK) == F_LINEMARK) return E.marks ? A_MARK : A_LINEMARK;
	if (f >= F_HEAD) return f < 2 * F_HEAD ? A_H1 : f < 3 * F_HEAD ? A_H2 : A_H3;
	if (f & F_CODE) return A_CODE;
	if ((f & (F_BOLD | F_ITAL)) == (F_BOLD | F_ITAL)) return A_BOLDITALIC;
	if (f & F_BOLD) return A_BOLD;
	if (f & F_ITAL) return A_ITALIC;
	return A_TEXT;
}

static void draw(const char *pending)
{
	/* the status line */
	clear_row(0, A_STATUS);
	if (E.msg[0]) {
		put(0, 0, A_STATUS, E.msg);
		E.msg[0] = 0;
	} else {
		char b[SCR_COLS + 1];
		size_t ls = line_start(E.cur);
		int line = line_number(ls);
		snprintf(b, sizeof b, " %.26s%s", base_name(E.path), E.modified ? "*" : "");
		put(0, 0, A_STATUS, b);
		snprintf(b, sizeof b, "P%02d L%02d C%02d", line / LINES_PER_PAGE + 1,
		         line % LINES_PER_PAGE + 1, col_of(ls, E.cur) + 1);
		put(0, 30, A_STATUS, b);
		put(0, 48, A_STATUS, E.overtype ? "Overtype" : "Insert");
		/* the style typing would have here: ^PB/^PY waiting, or inside bold */
		int st = E.pend;
		if (!st && !E.marks && E.cur > 0 && !hidden(E.cur - 1) && at(E.cur - 1) != '\n')
			st = fmt(E.cur - 1) & (F_BOLD | F_ITAL);
		if (!st && E.carry) st = E.carry;	/* (the space just typed: bold carries on) */
		if (st) put(0, 42, A_STATUS, st == (F_BOLD | F_ITAL) ? "B+I" : st == F_BOLD ? "Bold" : "Ital");
		if (percent_full() >= 90) {
			snprintf(b, sizeof b, "%d%%", percent_full());
			put(0, 57, A_MSG, b);
		}
		if (pending) put(0, 62, A_STATUS, pending);
	}

	int row = 1;
	if (E.help >= 3)
		for (int i = 0; i < MAIN_MENU_ROWS; i++, row++) {
			clear_row(row, A_MENU);
			put_menu(row, 0, A_MENU, A_KEY, main_menu[i]);
		}
	if (E.help >= 1) {
		char r[SCR_COLS + 1];
		for (int c = 0; c < SCR_COLS; c++) r[c] = c % 5 == 0 ? '!' : '-';
		r[0] = 'L';
		r[WRAP - 1] = 'R';
		r[WRAP] = r[WRAP + 1] = ' ';
		r[SCR_COLS] = 0;
		clear_row(row, A_RULER);
		put(row, 0, A_RULER, r);
		row++;
	}

	/* the text */
	size_t s = E.top, cv = vis(E.cur);
	bool have = line_at(s);
	for (; row < SCR_ROWS; row++) {
		clear_row(row, A_TEXT);
		if (!have) continue;
		size_t e = next_line(s);
		int col = 0;
		for (size_t i = s; i < e && col < SCR_COLS; i++) {
			if (!shown(i) && at(i) != '\n') continue;
			if (i >= E.hl_a && i < E.hl_b) scr.at[row][col] = A_FOUND;
			else if (block_ok() && i >= E.bb && i < E.bk)
				scr.at[row][col] = at(i) == '\n' ? A_BLOCK : (uint8_t)(style_of(fmt(i)) | A_INBLOCK);
			else if (at(i) != '\n') scr.at[row][col] = (uint8_t)style_of(fmt(i));
			if (i == cv) scr.at[row][col] |= A_CURSOR;
			if (at(i) == '\n') break;
			scr.ch[row][col++] = glyph_at(i);
		}
		if (cv == len() && e == len() && line_start(E.cur) == s)	/* (a full last line: the last column) */
			scr.at[row][col < SCR_COLS ? col : SCR_COLS - 1] |= A_CURSOR;
		have = e != s && line_at(e) && !(e == len() && at(e - 1) != '\n');
		s = e;
	}
}

/* a box of lines over the text, for menus and help */
static void draw_box(const char *const *lines, int n)
{
	int w = 0;
	for (int i = 0; i < n; i++) {
		int l = (int)strlen(lines[i]);
		if (l > w) w = l;
	}
	w += 2;
	if (w > SCR_COLS) w = SCR_COLS;
	int r0 = text_row0() + 1, c0 = (SCR_COLS - w) / 2;
	if (r0 > SCR_ROWS - n - 1) r0 = SCR_ROWS - n - 1;	/* a tall box goes higher */
	if (r0 < 1) r0 = 1;
	for (int i = -1; i <= n; i++) {
		int r = r0 + i;
		if (r < 0 || r >= SCR_ROWS) continue;
		for (int c = 0; c < w; c++) {
			scr.ch[r][c0 + c] = ' ';
			scr.at[r][c0 + c] = A_BOX;
		}
		if (i >= 0 && i < n) put_menu(r, c0 + 1, A_BOX, A_BOXKEY, lines[i]);
	}
}

/* ------------------------------------------------------------ asking */

/* a yes/no question on the status line */
static bool ask_yes(const char *q)
{
	draw(NULL);
	clear_row(0, A_MSG);
	put(0, 0, A_MSG, q);
	ed_show(&scr);
	int k = ed_key_wait(-1);
	return k == 'y' || k == 'Y';
}

/* ------------------------------------------------------------ moving */

static void go_up(void)
{
	size_t ls = line_start(E.cur);
	if (ls == 0) return;
	E.cur = pos_at(line_start(ls - 1), E.want);
}

static void go_down(void)
{
	size_t ls = line_start(E.cur), e = next_line(ls);
	if (e == ls || !line_at(e)) return;
	E.cur = pos_at(e, E.want);
}

static bool word_char(char c) { return c != ' ' && c != '\n' && c != '\t'; }

static size_t word_right(size_t p)
{
	size_t n = len();
	while (p < n && word_char(at(p))) p++;
	while (p < n && !word_char(at(p))) p++;
	return p;
}

static size_t word_left(size_t p)
{
	while (p > 0 && !word_char(at(p - 1))) p--;
	while (p > 0 && word_char(at(p - 1))) p--;
	return p;
}

static void scroll_down(void)		/* ^Z: the text moves up a line */
{
	size_t e = next_line(E.top);
	if (e == E.top || !line_at(e)) return;
	E.top = e;
	if (E.cur < E.top) E.cur = pos_at(E.top, E.want);
}

static void scroll_up(void)		/* ^W: the text moves down a line */
{
	if (E.top == 0) return;
	E.top = line_start(E.top - 1);
	if (lines_between(E.top, line_start(E.cur)) >= text_rows()) go_up();
}

static void place_marks(size_t pos, const char *cl, size_t ncl, const char *op, size_t nop, bool cur_inside);

/* a link whose text is all gone ("[](address)" at pos-1): take its marks too */
static bool drop_empty_link(size_t pos)
{
	if (pos == 0 || pos + 1 >= len() || at(pos - 1) != '[' || at(pos) != ']' || at(pos + 1) != '(') return false;
	size_t r = pos + 2;
	while (r < len() && at(r) != ')' && at(r) != ' ' && at(r) != '\n') r++;
	if (r >= len() || at(r) != ')') return false;
	size_t cur = E.cur;
	delete(pos - 1, r + 2 - pos, false);
	E.cur = cur > r ? cur - (r + 2 - pos) : cur >= pos - 1 ? pos - 1 : cur;
	return true;
}

/* delete the character the cursor shows on (not the marks before it) */
static void delete_char(size_t p)
{
	size_t a = vis(p), b = next_char(p);
	bool marks_round = a > 0 && hidden(a - 1) && b < len() && hidden(b);
	/* opening marks just before it, closing ones just after */
	size_t lo = a, hi = b;
	while (lo > 0 && hidden(lo - 1) && (fmt(lo - 1) & F_OPEN) && strchr("*_`", at(lo - 1))) lo--;
	while (hi < len() && hidden(hi) && !(fmt(hi) & F_OPEN) && strchr("*_`", at(hi))) hi++;
	delete(a, b - a, false);
	if (drop_empty_link(a)) return;
	hi -= b - a;
	if (!marks_round || a - lo != hi - a) {
		/* a mark now against a space wouldn't count: move it to the word */
		char m[16];
		size_t nl = a - lo, nh = hi - a;
		if (nl && nl < 16 && a < len() && (at(a) == ' ' || at(a) == '\t')) {
			for (size_t i = 0; i < nl; i++) m[i] = at(lo + i);
			delete(lo, nl, false);
			place_marks(lo, NULL, 0, m, nl, false);
			return;
		}
		if (nh && nh < 16 && a > 0 && (at(a - 1) == ' ' || at(a - 1) == '\t')) {
			for (size_t i = 0; i < nh; i++) m[i] = at(a + i);
			delete(a, nh, false);
			place_marks(a, m, nh, NULL, 0, false);
			return;
		}
	}
	/* the last character of something bold or italic: take its marks too */
	size_t l = 0, r = 0;
	char m = a < len() ? at(a) : 0;
	if (!marks_round || (m != '*' && m != '_' && m != '`')) return;
	while (l < a && at(a - 1 - l) == m) l++;
	while (a + r < len() && at(a + r) == m) r++;
	if (l == r && l >= 1 && l <= 3) {
		delete(a - l, 2 * l, false);
		E.cur = a - l;
	}
}

/* Which bytes of a..b go when a..b is deleted: all but the hidden marks
   whose partners are outside it (so bold stays paired).  Marks pair up
   in order, each closing mark with the last opening one of its kind. */
static void going(size_t a, size_t b, uint8_t *go)
{
	size_t st[64];
	int sp = 0;
	for (size_t i = a; i < b; i++) {
		go[i - a] = 1;
		if (!hidden(i)) continue;
		char c = at(i);
		if (c == '[' && (fmt(i) & F_OPEN)) {	/* a link: kept unless its "](address)" goes too */
			size_t j = i + 1;
			while (j < len() && at(j) != '\n' && !(hidden(j) && at(j) == ']')) j++;
			size_t r = j;
			while (r < len() && at(r) != ')' && at(r) != '\n') r++;
			if (r < len() && at(r) == ')') r++;
			if (r > b) go[i - a] = 0;
			continue;
		}
		if (c == ']' && !(fmt(i) & F_OPEN)) {	/* a link's "](address)": kept unless its [ goes too */
			size_t o = i;
			while (o > 0 && at(o - 1) != '\n' && !(hidden(o - 1) && at(o - 1) == '[')) o--;
			size_t r = i;
			while (r < len() && at(r) != ')' && at(r) != '\n') r++;
			if (r < len() && at(r) == ')') r++;
			if (o == 0 || o - 1 < a)
				for (size_t q = i; q < r && q < b; q++) go[q - a] = 0;
			else
				for (size_t q = i; q < r && q < b; q++) go[q - a] = 1;
			i = (r < b ? r : b) - 1;
			continue;
		}
		if (c != '*' && c != '_' && c != '`') continue;	/* # and \\ go with their text */
		if (c != '`' && !(fmt(i) & (F_W1 | F_W2))) continue;	/* (a * or _ in a link's address) */
		if (fmt(i) & F_OPEN) {
			go[i - a] = 0;			/* kept, unless its partner turns up */
			if (sp < 64) st[sp++] = i;
		} else {
			int o = sp - 1;
			while (o >= 0 && at(st[o]) != c) o--;
			if (o < 0) { go[i - a] = 0; continue; }	/* its opener is before a */
			go[st[o] - a] = 1;
			for (int q = o; q < sp - 1; q++) st[q] = st[q + 1];
			sp--;
		}
	}
}

/* delete a..b, keeping marks whose partners stay; for ^U if keep */
static void delete_keep(size_t a, size_t b, bool keep)
{
	if (b <= a) return;
	/* the hidden marks just outside a..b count too: a kept mark whose
	   partner is right there, with nothing shown between, would make an
	   empty pair, so both go */
	size_t lo = a, hi = b;
	if (!E.marks) {
		while (lo > 0 && hidden(lo - 1)) lo--;
		while (hi < len() && hidden(hi)) hi++;
	}
	size_t n = hi - lo;
	uint8_t *go = malloc(n), *opener = malloc(n), *fl = malloc(n);
	char *t = keep ? malloc(n) : NULL, *orig = malloc(n);
	if (!go || !opener || !orig || !fl || (keep && !t)) {
		free(go); free(opener); free(t); free(orig); free(fl);
		delete(a, b - a, keep);
		return;
	}
	memset(go, 0, n);
	going(a, b, go + (a - lo));
	for (size_t i = 0; i < n; i++) {
		orig[i] = at(lo + i);
		fl[i] = fmt(lo + i);
		opener[i] = hidden(lo + i) && (fl[i] & F_OPEN);
	}
	#define MARK(i) (hidden(lo + (i)) && strchr("*_`", orig[i]) && (orig[i] == '`' || (fl[i] & (F_W1 | F_W2))))
	/* kept closing marks against openers just before a */
	size_t ost[32];
	int osp = 0;
	for (size_t i = 0; i < a - lo; i++) {
		if (!MARK(i)) continue;
		if (opener[i]) { if (osp < 32) ost[osp++] = i; }
		else if (osp && orig[ost[osp - 1]] == orig[i]) osp--;
	}
	for (size_t i = a - lo; i < b - lo; i++) {
		if (go[i] || !MARK(i) || opener[i]) continue;
		if (!osp || orig[ost[osp - 1]] != orig[i]) break;
		go[i] = go[ost[--osp]] = 1;
	}
	/* kept opening marks against closers just after b */
	size_t cst[32];
	int cn = 0, cj = 0;
	{
		int depth[3] = { 0, 0, 0 };
		for (size_t i = b - lo; i < n; i++) {
			if (!MARK(i)) continue;
			int ci = orig[i] == '*' ? 0 : orig[i] == '_' ? 1 : 2;
			if (opener[i]) depth[ci]++;
			else if (depth[ci]) depth[ci]--;
			else if (cn < 32) cst[cn++] = i;
		}
	}
	for (size_t i = b - lo; i-- > a - lo; ) {
		if (go[i] || !MARK(i) || !opener[i]) continue;
		if (cj >= cn || orig[cst[cj]] != orig[i]) break;
		go[i] = go[cst[cj++]] = 1;
	}
	/* with nothing but marks left between the text either side (and no
	   space there), closing marks meeting the same opening ones join two
	   bold pieces into one: both go */
	if (!E.marks && !(lo > 0 && (at(lo - 1) == ' ' || at(lo - 1) == '\t' || at(lo - 1) == '\n')) &&
	    !(hi < len() && (at(hi) == ' ' || at(hi) == '\t' || at(hi) == '\n')) && lo > 0 && hi < len()) {
		/* the closing and opening marks there, as pairs' ends: one byte for
		   an italic's, two for a bold's (and a backtick each) */
		size_t C[64], O[64], cw[64], ow[64];
		int nc = 0, no = 0;
		for (size_t i = 0; i < n; i++) {
			if (go[i] || !MARK(i)) continue;
			size_t w = orig[i] == '`' ? 1 : WIDTH(fl[i]);
			if (i + w > n) w = 1;
			if (!opener[i]) { if (nc < 64) C[nc] = i, cw[nc++] = w; }
			else if (no < 64) O[no] = i, ow[no++] = w;
			i += w - 1;
		}
		/* the outermost closing end against the outermost opening one: the
		   same kind joins up ("**" then "*" is bold ending and italic
		   starting, which reads fine as it is) */
		int oi = 0;
		while (nc && oi < no && orig[C[nc - 1]] == orig[O[oi]] && cw[nc - 1] == ow[oi]) {
			nc--;
			for (size_t q = 0; q < cw[nc]; q++) go[C[nc] + q] = 1;
			for (size_t q = 0; q < ow[oi]; q++) go[O[oi] + q] = 1;
			oi++;
		}
	}
	#undef MARK
	/* for ^U: what goes from a..b */
	size_t k = 0;
	if (t) for (size_t i = a - lo; i < b - lo; i++) if (go[i]) t[k++] = orig[i];
	/* the marks of a..b that stay, closing ones and opening ones */
	char *cl = malloc(n + 1), *op = malloc(n + 1);
	if (!cl || !op) { free(cl); free(op); free(go); free(opener); free(t); free(orig); free(fl); delete(a, b - a, keep); return; }
	size_t ncl = 0, nop = 0, stay = 0;
	for (size_t i = a - lo; i < b - lo; i++)
		if (!go[i]) {
			stay++;
			if (opener[i]) op[nop++] = orig[i];
			else cl[ncl++] = orig[i];
		}
	/* delete, from the end, a run at a time */
	size_t before_a = 0;		/* how much goes before a (moves a back) */
	for (size_t i = 0; i < a - lo; i++) before_a += go[i];
	for (size_t i = n; i > 0; ) {
		size_t e = i;
		while (i > 0 && go[i - 1]) i--;
		if (i < e) delete(lo + i, e - i, false);
		while (i > 0 && !go[i - 1]) i--;
	}
	a -= before_a;
	if (t && k) keep_kill(a, b - a + before_a, t, k);
	/* the marks kept are together at a now: put them against the text
	   either side of the gap (with no gap, a closing mark meeting the same
	   opening one joins two bold pieces into one: both go) */
	if (stay) {
		delete(a, stay, false);
		place_marks(a, cl, ncl, op, nop, false);
	}
	drop_empty_link(a);
	free(go); free(opener); free(t); free(orig); free(fl); free(cl); free(op);
}

/* the text of a..b without its unpartnered marks (malloc'd; *n its length) */
static char *text_of(size_t a, size_t b, size_t *n)
{
	uint8_t *go = malloc(b - a + 1);
	char *t = malloc(b - a + 1);
	*n = 0;
	if (!go || !t) { free(go); free(t); return NULL; }
	going(a, b, go);
	for (size_t i = a; i < b; i++) if (go[i - a]) t[(*n)++] = at(i);
	free(go);
	return t;
}

/* put marks into the text at pos: closing ones against the text before
   (back over any spaces: a closing mark after a space doesn't close), and
   opening ones against the text after (on over spaces); the cursor moves
   along if it's after them (or at them, for the opening ones if
   cur_inside) */
static void place_marks(size_t pos, const char *cl, size_t ncl, const char *op, size_t nop, bool cur_inside)
{
	size_t cur = E.cur;
	char *o2 = malloc(nop + 1), *c2 = malloc(ncl + 1);
	if (!o2 || !c2) {		/* (no memory: put them back where they were) */
		free(o2); free(c2);
		E.cur = pos; insert(cl, ncl); insert(op, nop);
		E.cur = cur >= pos ? cur + ncl + nop : cur;
		return;
	}
	if (nop) memcpy(o2, op, nop);
	if (ncl) memcpy(c2, cl, ncl);
	op = o2;
	cl = c2;
	if (nop) {
		size_t r = pos;
		while (r < len() && (at(r) == ' ' || at(r) == '\t')) r++;
		/* its closing mark right there: bold with nothing in it, so neither */
		while (nop && r < len() && hidden(r) && !(fmt(r) & F_OPEN) && at(r) == op[nop - 1]) {
			delete(r, 1, false);
			if (cur > r) cur--;
			nop--;
		}
	}
	if (nop) {
		size_t r = pos;
		while (r < len() && (at(r) == ' ' || at(r) == '\t')) r++;
		E.cur = r;
		insert(op, nop);
		if (cur > r || (cur == r && cur_inside)) cur += nop;
	}
	if (ncl) {
		size_t q = pos, ps = para_start(pos);
		while (q > ps && (at(q - 1) == ' ' || at(q - 1) == '\t')) q--;
		while (ncl && q > 0 && hidden(q - 1) && (fmt(q - 1) & F_OPEN) && at(q - 1) == cl[0]) {
			delete(q - 1, 1, false);	/* its opening mark right there: neither */
			if (cur >= q) cur--;
			q--;
			memmove(c2, c2 + 1, --ncl);
		}
		E.cur = q;
		if (!ncl) { E.cur = cur; free(o2); free(c2); return; }
		insert(cl, ncl);
		if (cur >= q) cur += ncl;
	}
	E.cur = cur;
	free(o2);
	free(c2);
}

/* Insert text with line breaks in it.  A break inside something bold
   (or italic, or code) would leave its marks in two paragraphs, unpaired,
   so the marks are closed before the first break and opened again after
   the last: both halves stay bold. */
static void insert_text(const char *s, size_t n)
{
	const char *nl = memchr(s, '\n', n);
	if (!nl || E.marks) { insert(s, n); return; }
	/* past any closing marks just ahead (the end of a bold word: nothing to split) */
	size_t p = E.cur;
	while (p < len() && hidden(p) && !(fmt(p) & F_OPEN)) p++;
	char st[16];
	size_t sat[16];
	int sp = 0;
	for (size_t i = para_start(p); i < p; i++) {
		if (!hidden(i)) continue;
		char c = at(i);
		if (c == ']' && !(fmt(i) & F_OPEN)) c = '[';	/* a link's end closes its [ */
		else if (c == '[' && (fmt(i) & F_OPEN)) ;
		else if (c != '*' && c != '_' && c != '`') continue;
		else if (c != '`' && !(fmt(i) & (F_W1 | F_W2))) continue;	/* (in a link's address) */
		if (fmt(i) & F_OPEN) { if (sp < 16) sat[sp] = i, st[sp++] = c; }
		else {
			int o = sp - 1;
			while (o >= 0 && st[o] != c) o--;
			if (o >= 0) { for (int q = o; q < sp - 1; q++) st[q] = st[q + 1], sat[q] = sat[q + 1]; sp--; }
			if (c == '[') while (i + 1 < p && at(i) != ')') i++;	/* past its address */
		}
	}
	/* a link the break falls inside: both halves keep its address */
	size_t url = NOWHERE, url_len = 0;
	for (int q = 0; q < sp; q++)
		if (st[q] == '[') {
			size_t j = p;
			while (j < len() && at(j) != '\n' && !(hidden(j) && at(j) == ']' && !(fmt(j) & F_OPEN))) j++;
			size_t r = j;
			while (r < len() && at(r) != ')' && at(r) != '\n') r++;
			if (r < len() && at(r) == ')') r++;
			if (j < len() && at(j) == ']') { url = j; url_len = r - j; }
			else { for (int z = q; z < sp - 1; z++) st[z] = st[z + 1], sat[z] = sat[z + 1]; sp--; }	/* (not a link after all) */
			break;
		}
	E.cur = p;
	if (!sp) { insert(s, n); return; }
	/* opening marks with nothing after them yet (the break comes straight
	   after "**"): they move to after the break instead of being closed */
	int empty = 0;
	for (int q = sp - 1; q >= 0; q--) {
		bool shown_after = false;
		for (size_t i = sat[q] + 1; i < p && !shown_after; i++) shown_after = !hidden(i);
		if (shown_after) break;
		empty++;
	}
	for (int q = sp - 1; q >= sp - empty; q--) {	/* innermost first: positions stay good */
		delete(sat[q], 1, false);
		p--;
	}
	E.cur = p;
	const char *last = s + n;
	while (last[-1] != '\n') last--;		/* just after the last break */
	size_t a = E.cur;
	insert(s, n);
	if (E.cur != a + n) return;			/* memory full */
	char *cl = malloc((size_t)sp + url_len + 1), op[16];
	if (!cl) return;
	int ncl = sp - empty;
	size_t k = 0;
	for (int q = ncl - 1; q >= 0; q--) {		/* close, innermost first */
		if (st[q] != '[') { cl[k++] = st[q]; continue; }
		size_t u = url + (url > a ? n : 0);	/* (the text went in before it) */
		for (size_t z = 0; z < url_len; z++) cl[k++] = at(u + z);
	}
	for (int q = 0; q < sp; q++) op[q] = st[q];
	place_marks(a + (size_t)(last - s), NULL, 0, op, (size_t)sp, true);	/* open again after the last break */
	place_marks(a + (size_t)(nl - s), cl, k, NULL, 0, false);		/* close before the first */
	free(cl);
}

/* ^PB, ^PY with a block: put marks around each paragraph of it */
static void wrap_block(const char *m)
{
	size_t ml = strlen(m), a = E.bb, b = E.bk, e = b, cur = E.cur, added = 0;
	for (;;) {
		size_t s = e;
		while (s > a && at(s - 1) != '\n') s--;
		size_t x = s, y = e;
		while (y > x && space(at(y - 1))) y--;
		while (x < y && space(at(x))) x++;
		if (x < y && x == para_start(x) && at(x) == '#') {	/* not around a heading's #s */
			while (x < y && at(x) == '#') x++;
			while (x < y && at(x) == ' ') x++;
		}
		/* already bold (or italic) from end to end: take it off instead */
		size_t os = NOWHERE, cs = NOWHERE;
		if (x < y && !E.marks) {
			for (size_t q = x; q + ml <= y && hidden(q) && (fmt(q) & F_OPEN); q++)
				if (at(q) == m[0] && WIDTH(fmt(q)) == ml) { os = q; break; }
			size_t c0 = y, c1 = y;
			while (c0 > x && hidden(c0 - 1) && !(fmt(c0 - 1) & F_OPEN)) c0--;
			while (c1 < len() && hidden(c1) && !(fmt(c1) & F_OPEN)) c1++;
			for (size_t q = c1; q >= c0 + ml; q--)
				if (at(q - 1) == m[0] && WIDTH(fmt(q - 1)) == ml) { cs = q - ml; break; }
		}
		if (os != NOWHERE && cs != NOWHERE && cs > os) {
			delete(cs, ml, false);
			delete(os, ml, false);
			cur -= cur >= cs + ml ? 2 * ml : cur > os ? ml : 0;
			added -= 2 * ml;
		} else if (x < y && make_room(2 * ml)) {
			E.cur = y; insert(m, ml);
			E.cur = x; insert(m, ml);
			cur += cur >= y ? 2 * ml : cur > x ? ml : 0;
			added += 2 * ml;
		}
		if (s <= a) break;
		e = s - 1;
	}
	E.cur = cur;
	E.bb = a;			/* the block still holds it all, marks and all */
	E.bk = b + added;
}

/* ------------------------------------------------------------ menus */

static const char *const k_menu[] = {
	"^K  files and blocks: then one of these keys",
	"",
	"S  save, and go on       B  block begins here",
	"D  save, back to list    K  block ends here",
	"X  save, back to shell   C  copy the block here",
	"Q  close, no saving      V  move the block here",
	"                         Y  delete the block",
	"0-9  set a place marker  H  hide/show the block",
	"",
	"(with Ctrl or without: ^KS and KS are the same)",
};
#define K_MENU_LINES 10
static const char *const q_menu[] = {
	"^Q  quick moves: then one of these keys",
	"",
	"S  start of line      D  end of line",
	"E  top of screen      X  bottom of screen",
	"R  start of document  C  end of document",
	"B  start of block     K  end of block",
	"0-9  a place marker",
	"F  find               A  find and replace",
	"Y  delete to end of line",
	"Backspace  delete to start of line",
	"",
	"(with Ctrl or without: ^QS and QS are the same)",
};
#define Q_MENU_LINES 12
static const char *const o_menu[] = {
	"^O  options: then one of these keys",
	"",
	"D  show or hide the Markdown marks",
	"P  the reading view: the document as it reads",
	"V  page view: the pages as they'd print",
	"",
	"(with Ctrl or without)",
};
#define O_MENU_LINES 7
static const char *const p_menu[] = {
	"^P  bold and italic: then one of these keys",
	"",
	"B  bold     Y  italic     Space  back to plain",
	"",
	"With a block marked (^KB ^KK), it becomes bold or",
	"italic. Otherwise what you type next is, until",
	"^P Space (or ^PB, ^PY again) at the end of it.",
	"",
	"(with Ctrl or without)",
};
#define P_MENU_LINES 9
static const char *const j_menu[] = {
	"^J  help: then one of these keys",
	"",
	"H  set the help level",
	"J  the keys",
	"",
	"(with Ctrl or without)",
};
#define J_MENU_LINES 6
static const char *const keys_help[] = {
	"Diamond: the keys",
	"",
	"Ctrl alone, then a letter: the same as Ctrl+letter",
	"^E ^X ^S ^D   up, down, left, right (arrows too)",
	"^A ^F         a word left, a word right",
	"^R ^C         a screen up, down (PgUp, PgDn too)",
	"^W ^Z         scroll the text down, up a line",
	"^QS ^QD       start, end of line (Home, End too)",
	"^QR ^QC       start, end of document",
	"^G Del        delete the character at the cursor",
	"Backspace     delete the character before it",
	"^T ^Y         delete a word, a line",
	"^U            bring back what ^T or ^Y deleted",
	"^N            split the line at the cursor",
	"^V            insert or overtype",
	"^KS ^KD ^KX   save; save and close; save and exit",
	"^KQ           close without saving",
	"^KB ^KK       mark the start, end of a block",
	"^KC ^KV ^KY   copy, move, delete the block",
	"^KH           hide or show the block",
	"^PB ^PY       bold, italic (the block, or what's typed)",
	"^P Space      back to plain text (at the end of bold)",
	"^OD           show the Markdown marks (** _ # `)",
	"^OP ^OV       the reading view; page view (Esc comes back)",
	"^K0..^K9      set a place marker; ^Q0..^Q9 go there",
	"^QF ^QA ^L    find; find and replace; again",
	"^JH           help level: 3 shows the most",
	"",
	"(any key to go on)",
};

/* a prefix (^K, ^Q, ^O, ^J): its second key, as an upper-case letter, or 0 */
static int second_key(const char *name, const char *const *menu, int n)
{
	draw(name);
	ed_show(&scr);
	/* the menu: at once at help level 3, after a moment's pause at 2 (so
	   someone who knows the keys never sees it), never below that */
	int k = E.help >= 3 ? K_NONE : ed_key_wait(E.help >= 2 ? 500 : -1);
	if (k == K_NONE) {
		draw(name);
		draw_box(menu, n);
		ed_show(&scr);
		k = ed_key_wait(-1);
	}
	if (k >= 1 && k <= 26) return k + '@';
	if (k >= 'a' && k <= 'z') return k - 32;
	if (k >= 'A' && k <= 'Z') return k;
	if (k >= '0' && k <= '9') return k;
	if (k == K_BS || k == K_DEL) return 8;
	if (k == ' ') return ' ';
	return 0;
}

static void show_box(const char *const *lines, int n)
{
	draw(NULL);
	draw_box(lines, n);
	ed_show(&scr);
	ed_key_wait(-1);
}

/* ------------------------------------------------------------ files */

static bool save(void)
{
	const char *err = ed_io_save(E.path, buf, gs, buf + ge, cap - ge, E.crlf);
	if (err) {
		char b[SCR_COLS + 1];
		snprintf(b, sizeof b, " Not saved: %s", err);
		say(b);
		return false;
	}
	E.modified = false;
	say(" Saved");
	return true;
}

/* ------------------------------------------------------------ asking for text */

/* a line of text on the status line, starting from what buf holds;
   false on Esc */
static bool ask_text(const char *label, char *buf_, size_t size)
{
	size_t n = strlen(buf_);
	for (;;) {
		char b[SCR_COLS + 1];
		draw(NULL);
		clear_row(0, A_MSG);
		snprintf(b, sizeof b, " %s %s", label, buf_);
		put(0, 0, A_MSG, b);
		int c = (int)strlen(b);
		if (c < SCR_COLS) scr.at[0][c] |= A_CURSOR;
		ed_show(&scr);
		int k = ed_key_wait(-1);
		if (k == K_ESC) return false;
		if (k == K_ENTER) return true;
		if (k == K_BS) { if (n) buf_[--n] = 0; continue; }
		if (k == CTRL('Y')) { n = 0; buf_[0] = 0; continue; }	/* WordStar: clear the answer */
		if (k >= 0x20 && k < 0x7F && n + 1 < size) { buf_[n++] = (char)k; buf_[n] = 0; }
	}
}

/* ------------------------------------------------------------ blocks */

static void block_copy(bool move)
{
	if (!block_ok()) { say(" No block: mark its start with ^KB and its end with ^KK"); return; }
	if (move && E.cur > E.bb && E.cur < E.bk) { say(" The cursor is inside the block"); return; }
	/* the block's text, less any marks whose partners are outside it (they
	   stay where they are, so nothing is left half bold) */
	size_t n;
	char *t = text_of(E.bb, E.bk, &n);
	if (!t) { say(" Not enough memory to copy the block"); return; }
	if (!move && !make_room(n)) { free(t); say(" Memory is full: no room for a copy of the block"); return; }
	if (move) delete_keep(E.bb, E.bk, false);	/* moves the cursor back if it was after */
	size_t at_ = E.cur, was = len();
	insert_text(t, n);
	free(t);
	E.bb = at_;
	E.bk = at_ + (len() - was);
	E.cur = at_;
}

/* ------------------------------------------------------------ finding */

static bool word_ch(char c) { return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || (c & 0x80); }

static char fold(char c, bool nocase) { return nocase && c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* is E.find at p, as the text is shown (hidden marks passed over)?  Its
   end, or NOWHERE */
static size_t match_at(size_t p, bool nocase, bool whole)
{
	size_t n = strlen(E.find), q = p, end = len();
	if (q >= end || hidden(q)) return NOWHERE;
	for (size_t i = 0; i < n; i++) {
		while (i && q < end && hidden(q)) q++;
		if (q >= end || fold(at(q), nocase) != fold(E.find[i], nocase)) return NOWHERE;
		q++;
	}
	if (whole) {
		size_t b = canon(p, 0), v = vis(q);
		if ((b > 0 && word_ch(at(b - 1))) || (v < end && word_ch(at(v)))) return NOWHERE;
	}
	return q;
}

/* the next match from p (or before p, backwards); NOWHERE if none, and
   where it ends in *e */
static size_t search(size_t p, bool back, bool nocase, bool whole, size_t *e)
{
	if (!E.find[0]) return NOWHERE;
	if (back) {
		while (p > 0) { p--; if ((*e = match_at(p, nocase, whole)) != NOWHERE) return p; }
		return NOWHERE;
	}
	for (; p < len(); p++)
		if ((*e = match_at(p, nocase, whole)) != NOWHERE) return p;
	return NOWHERE;
}

/* put the replacement over what was found at m..e: letter for letter over
   the shown characters, so the marks among them stay where they are; any
   more letters go after the last, any fewer and the rest are deleted.
   Where the cursor goes after it. */
static size_t replace_at(size_t m, size_t e)
{
	size_t pos[sizeof E.find], nv = 0, rlen = strlen(E.repl);
	for (size_t q = m; q < e && nv < sizeof pos / sizeof pos[0]; q++)
		if (!hidden(q)) pos[nv++] = q;
	size_t k = rlen < nv ? rlen : nv;
	for (size_t i = 0; i < k; i++)
		if (at(pos[i]) != E.repl[i]) {
			E.cur = pos[i];
			delete(pos[i], 1, false);
			insert(&E.repl[i], 1);
		}
	if (rlen > nv) {
		E.cur = pos[nv - 1] + 1;
		insert(E.repl + nv, rlen - nv);
		return E.cur;
	}
	for (size_t i = nv; i > rlen; i--) delete_char(pos[i - 1]);
	return rlen ? pos[rlen - 1] + 1 : m;
}

/* ^QF (replace false) and ^QA (true), or ^L (again: ask nothing) */
static void find_replace(bool replace, bool again)
{
	if (!again) {
		if (!ask_text("Find?", E.find, sizeof E.find) || !E.find[0]) return;
		if (replace && !ask_text("Replace with?", E.repl, sizeof E.repl)) return;
		if (!ask_text(replace ? "Options (B back, U case, W words, G whole document, N don't ask)?"
		                      : "Options (B backwards, U ignore case, W whole words, G from the top)?",
		              E.opts, sizeof E.opts)) return;
		E.last_search = replace ? 2 : 1;
	}
	bool back = false, nocase = false, whole = false, global = false, noask = false;
	for (const char *o = E.opts; *o; o++)
		switch (*o | 0x20) {
		case 'b': back = true; break;
		case 'u': nocase = true; break;
		case 'w': whole = true; break;
		case 'g': global = true; break;
		case 'n': noask = true; break;
		}
	size_t flen = strlen(E.find), rlen = strlen(E.repl);
	/* The cursor ends just after what was found (just before it, searching
	   backwards), as in WordStar, so ^L goes on from there and the cursor
	   doesn't sit on the highlight. */
	size_t p = global && !again ? (back ? len() : 0) : E.cur;
	int count = 0;
	for (;;) {
		size_t me;
		size_t m = search(p, back, nocase, whole, &me);
		if (m == NOWHERE) break;
		E.cur = back ? m : me;
		if (!replace) {
			E.hl_a = m;
			E.hl_b = me;
			return;
		}
		if (!noask) {
			E.hl_a = m;
			E.hl_b = me;
			keep_in_view();
			draw(NULL);
			clear_row(0, A_MSG);
			put(0, 0, A_MSG, " Replace this? (Y/N, Esc to stop)");
			ed_show(&scr);
			int k = ed_key_wait(-1);
			E.hl_a = E.hl_b = 0;
			if (k == K_ESC) break;
			if (k != 'y' && k != 'Y') { p = back ? m : m + 1; continue; }
		}
		if (rlen > flen && !make_room(rlen - flen)) { say(" Memory is full"); return; }
		size_t after = replace_at(m, me);
		count++;
		p = back ? m : after;
		E.cur = back ? m : after;
	}
	char b[SCR_COLS + 1];
	if (replace) snprintf(b, sizeof b, " %d replaced", count);
	else snprintf(b, sizeof b, " Not found: %s", E.find);
	say(b);
}

/* ------------------------------------------------------------ the main loop */

int ed_run(const char *path, char *text, size_t n, size_t capacity, int help_level)
{
	memset(&E, 0, sizeof E);
	E.path = path;
	E.plain = !is_markdown(path);
	E.help = help_level < 0 ? 0 : help_level > 3 ? 3 : help_level;

	/* CR LF line ends (Windows) become LF, and go back that way on saving */
	size_t j = 0;
	for (size_t i = 0; i < n; i++) {
		if (text[i] == '\r' && i + 1 < n && text[i + 1] == '\n') { E.crlf = true; continue; }
		text[j++] = text[i];
	}
	n = j;
	buf = text;
	cap = capacity;
	gs = n;
	ge = cap;
	E.cur = 0;
	ln_anchor = 0;
	ln_count = 0;
	f_start = (size_t)-1;		/* nothing kept from the last document */
	E.bb = E.bk = NOWHERE;
	for (int i = 0; i < 10; i++) E.mark[i] = NOWHERE;

	int result = -1;
	while (result < 0) {
		keep_in_view();
		draw(NULL);
		ed_show(&scr);
		int k = ed_key_wait(-1);
		bool vertical = false, keep_pend = false, stay = false, keep_carry = false;
		size_t was = E.cur;
		unsigned edits = E.edits;
		size_t ls;
		E.cmd++;
		E.hl_a = E.hl_b = 0;		/* a find's highlight lasts until the next key */

		switch (k) {
		case CTRL('E'): case K_UP: go_up(); vertical = true; break;
		case CTRL('X'): case K_DOWN: go_down(); vertical = true; break;
		case CTRL('S'): case K_LEFT: E.cur = prev_char(E.cur); break;
		case CTRL('D'): case K_RIGHT: E.cur = next_char(E.cur); break;
		case CTRL('A'): E.cur = word_left(E.cur); break;
		case CTRL('F'): E.cur = word_right(E.cur); break;
		case CTRL('R'): case K_PGUP:
			for (int i = 1; i < text_rows(); i++) { go_up(); if (E.top) E.top = line_start(E.top - 1); }
			vertical = true;
			break;
		case CTRL('C'): case K_PGDN:
			for (int i = 1; i < text_rows(); i++) { go_down(); scroll_down(); }
			vertical = true;
			break;
		case CTRL('W'): scroll_up(); vertical = true; break;
		case CTRL('Z'): scroll_down(); vertical = true; break;
		case K_HOME: E.cur = line_start(E.cur); break;
		case K_END: E.cur = line_last(line_start(E.cur)); break;

		case CTRL('G'): case K_DEL:
			delete_char(E.cur);
			break;
		case K_BS:
			if (canon(E.cur, 0) > 0) delete_char(prev_char(E.cur));
			break;
		case CTRL('T'):
			delete_keep(E.cur, word_right(E.cur), true);
			break;
		case CTRL('Y'):
			ls = line_start(E.cur);
			delete_keep(ls, next_line(ls), true);
			E.cur = ls;
			break;
		case CTRL('U'):
			if (E.unerase_len) insert_text(E.unerase, E.unerase_len);
			break;
		case CTRL('N'): {
			insert_text("\n", 1);
			size_t q = E.cur;		/* back to the break (before any marks it closed) */
			while (q > 0 && at(q - 1) != '\n') q--;
			if (q > 0) E.cur = canon(q - 1, para_start(q - 1));
			break;
		}
		case CTRL('V'): case K_INS: E.overtype = !E.overtype; break;
		case CTRL('L'):
			if (E.last_search) find_replace(E.last_search == 2, true);
			else say(" Nothing to repeat: ^QF finds");
			break;
		case K_ENTER: insert_text("\n", 1); break;
		case K_TAB: insert("    ", 4 - col_of(line_start(E.cur), E.cur) % 4); break;
		case K_ESC: say(" ^KD saves and goes back to the list; ^J is help"); break;
		case K_SHOT: {
			char info[256];
			snprintf(info, sizeof info,
			         "file %s\nlength %zu, cursor %zu, top %zu, column wanted %d\n"
			         "help level %d, %s, %s line ends, %s\n"
			         "gap %zu..%zu of %zu, unerase %zu bytes\n",
			         E.path, len(), E.cur, E.top, E.want, E.help,
			         E.overtype ? "overtype" : "insert", E.crlf ? "CR LF" : "LF",
			         E.modified ? "changed" : "unchanged", gs, ge, cap, E.unerase_len);
			const char *name = ed_screenshot(&scr, info);
			char b[SCR_COLS + 1];
			snprintf(b, sizeof b, name ? " Screenshot: %s (.bmp and .txt)" : " The screenshot failed",
			         name ? name : "");
			say(b);
			vertical = true;
			break;
		}

		case CTRL('K'):
			k = second_key("^K", k_menu, K_MENU_LINES);
			if (k >= '0' && k <= '9') {
				E.mark[k - '0'] = E.cur;
				char b[SCR_COLS + 1];
				snprintf(b, sizeof b, " Place marker %c set; ^Q%c comes back here", k, k);
				say(b);
				break;
			}
			switch (k) {
			case 'B': E.bb = E.cur; E.hide = false; if (E.bk != NOWHERE && E.bk <= E.bb) E.bk = NOWHERE; break;
			case 'K': E.bk = E.cur; E.hide = false; if (E.bb != NOWHERE && E.bb >= E.bk) E.bb = NOWHERE; break;
			case 'H': E.hide = !E.hide; break;
			case 'C': block_copy(false); break;
			case 'V': block_copy(true); break;
			case 'Y':
				if (!block_ok()) { say(" No block to delete"); break; }
				if (E.bk - E.bb > UNERASE_MAX &&
				    !ask_yes(" The block is too big for ^U to bring back. Delete it? (Y/N)"))
					break;
				E.kill_cmd = 0;		/* not added to an earlier deletion */
				delete_keep(E.bb, E.bk, true);
				E.bb = E.bk = NOWHERE;
				break;
			case 'S': save(); break;
			case 'D': if (save()) result = ED_CLOSE; break;
			case 'X': if (save()) result = ED_EXIT; break;
			case 'Q':
				if (!E.modified || ask_yes(" Close without saving the changes? (Y/N)"))
					result = ED_CLOSE;
				break;
			}
			break;
		case CTRL('Q'):
			ls = line_start(E.cur);
			k = second_key("^Q", q_menu, Q_MENU_LINES);
			if (k >= '0' && k <= '9') {
				if (E.mark[k - '0'] == NOWHERE) say(" That place marker isn't set: ^K and the digit set it");
				else E.cur = E.mark[k - '0'];
				break;
			}
			switch (k) {
			case 'B': if (E.bb != NOWHERE) E.cur = E.bb; break;
			case 'K': if (E.bk != NOWHERE) E.cur = E.bk; break;
			case 'F': find_replace(false, false); break;
			case 'A': find_replace(true, false); break;
			case 'S': E.cur = ls; break;
			case 'D': E.cur = line_last(ls); break;
			case 'R': E.cur = 0; break;
			case 'C': E.cur = len(); break;
			case 'E': E.cur = pos_at(E.top, E.want); vertical = true; break;
			case 'X': {
				size_t s = E.top;
				for (int i = 1; i < text_rows(); i++) {
					size_t e = next_line(s);
					if (e == s || !line_at(e)) break;
					s = e;
				}
				E.cur = pos_at(s, E.want);
				vertical = true;
				break;
			}
			case 'Y': {
				size_t last = line_last(ls);
				delete_keep(E.cur, last, true);
				break;
			}
			case 8: delete_keep(ls, E.cur, true); break;
			}
			break;
		case CTRL('O'): {
			int o = second_key("^O", o_menu, O_MENU_LINES);
			if (o == 'P' || o == 'V') {	/* the reading view, page view (WordStar 5's ^OP: preview) */
				size_t back = reader_run(E.cur, base_name(E.path), o == 'V');
				ed_redraw();
				if (back > E.cur || back < para_start(E.cur)) {	/* it moved on: go there */
					E.cur = back;
					E.top = line_start(E.cur);
				}
				break;
			}
			if (o == 'D' && E.plain) { say(" Plain text: everything is shown just as it is"); break; }
			if (o == 'D') {
				E.marks = !E.marks;
				ln_anchor = 0;		/* the lines wrap differently now */
				ln_count = 0;
				say(E.marks ? " The Markdown marks are shown (^OD hides them)" : " The Markdown marks are hidden");
			}
			break;
		}
		case CTRL('P'): {
			int p = second_key("^P", p_menu, P_MENU_LINES);
			if (E.plain && (p == 'B' || p == 'Y' || p == ' ')) {
				say(" Plain text has no bold or italic (save as .md for those)");
				break;
			}
			int f = p == 'B' ? F_BOLD : p == 'Y' ? F_ITAL : 0;
			if (p == ' ') {		/* back to plain: out past any closing marks here */
				E.pend = 0;
				if (vis(E.cur) != E.cur && E.cur > 0 && at(E.cur - 1) != '\n' &&
				    (fmt(E.cur - 1) & (F_BOLD | F_ITAL)))
					E.cur = vis(E.cur), stay = true;
				else if (E.cur > 0 && !hidden(E.cur - 1) && (fmt(E.cur - 1) & (F_BOLD | F_ITAL)))
					say(" Plain text starts at the end of the bold or italic");
				break;
			}
			if (!f) { E.pend = 0; break; }
			if (block_ok()) { wrap_block(f == F_BOLD ? "**" : "*"); E.pend = 0; break; }
			/* just inside the end of bold or italic: step out past the marks */
			if (!E.pend && vis(E.cur) != E.cur && E.cur > 0 && at(E.cur - 1) != '\n' &&
			    (fmt(E.cur - 1) & (F_BOLD | F_ITAL))) {
				E.cur = vis(E.cur);
				keep_pend = stay = true;
				break;
			}
			E.pend ^= f;
			keep_pend = true;
			break;
		}
		case CTRL('J'):
			switch (second_key("^J", j_menu, J_MENU_LINES)) {
			case 'H': {
				char q[SCR_COLS + 1];
				snprintf(q, sizeof q, " Help level (0-3, now %d)?", E.help);
				draw(NULL);
				clear_row(0, A_MSG);
				put(0, 0, A_MSG, q);
				ed_show(&scr);
				int h = ed_key_wait(-1);
				if (h >= '0' && h <= '3') {
					E.help = h - '0';
					ed_save_help_level(E.help);
				}
				break;
			}
			case 'J': case 0:
				show_box(keys_help, sizeof keys_help / sizeof keys_help[0]);
				break;
			}
			break;

		default:
			if (k >= 0x20 && k < 0x7F) {
				char c = (char)k;
				if (E.overtype && vis(E.cur) < len() && at(vis(E.cur)) != '\n')
					delete_char(E.cur);
				/* a space at the end of something bold goes after its marks
				   (inside, it would stop them closing) */
				if (c == ' ' && !E.pend && !E.marks && E.cur < len() && hidden(E.cur) && !(fmt(E.cur) & F_OPEN)) {
					E.carry = E.cur > 0 ? fmt(E.cur - 1) & (F_BOLD | F_ITAL) : 0;
					while (E.cur < len() && hidden(E.cur) && !(fmt(E.cur) & F_OPEN)) E.cur++;
					keep_carry = true;
				} else if (c != ' ' && E.carry && !E.pend) {
					/* the closing marks back past the spaces: bring them along */
					size_t s = E.cur;
					while (s > 0 && (at(s - 1) == ' ' || at(s - 1) == '\t')) s--;
					size_t c0 = s;
					while (c0 > 0 && hidden(c0 - 1) && !(fmt(c0 - 1) & F_OPEN)) c0--;
					if (c0 < s && s - c0 < 16) {
						char m[16];
						size_t ml = s - c0;
						for (size_t i = 0; i < ml; i++) m[i] = at(c0 + i);
						delete(c0, ml, false);
						insert(&c, 1);
						insert(m, ml);
						E.cur -= ml;		/* before them: still in bold */
						E.carry = 0;
						break;
					}
					E.carry = 0;
				} else if (c == ' ' && E.carry) keep_carry = true;
				if (E.pend && c != ' ') {
					const char *m = E.pend == (F_BOLD | F_ITAL) ? "***" : E.pend == F_BOLD ? "**" : "*";
					size_t ml = strlen(m);
					char t[8];
					memcpy(t, m, ml);
					t[ml] = c;
					memcpy(t + ml + 1, m, ml);
					insert(t, 2 * ml + 1);
					E.cur -= ml;		/* before the closing marks: go on typing in the style */
					E.pend = 0;
				} else insert(&c, 1);
			}
			break;
		}
		if (!stay && E.edits == edits && E.cur != was) E.cur = canon(E.cur, para_start(E.cur));	/* a move */
		if (!keep_pend && !(k >= 0x20 && k < 0x7F)) E.pend = 0;
		if (!keep_carry) E.carry = 0;
		if (!vertical) E.want = col_of(line_start(E.cur), E.cur);
	}
	buf = NULL;
	free(E.unerase);
	return result;
}
