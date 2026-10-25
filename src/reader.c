/* reader.c -- Diamond's reading view (^OP): the document as it reads, in
 * proportional fonts, with the Markdown marks gone, paragraphs reflowed to
 * the screen, headings in their sizes, lists, quotations, code and links.
 *
 * The document is cut into blocks (a paragraph, a heading, a list item, a
 * quotation, a rule, a code block); each block is laid out into lines when
 * it's needed.  Only the screen being shown is ever laid out, so a long
 * document costs no more than a short one, apart from the list of where
 * its blocks start.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "editor.h"
#include "core.h"
#include "rfonts.h"

#define W SCR_PX
#define H SCR_PX
#define MARGIN 8
#define FOOTER 16		/* the line at the bottom */
#define TEXT_H (H - FOOTER)
#define MAX_LINES 1024		/* in one block; a longer one is cut short */
#define RGB(r, g, b) (uint16_t)(((r) >> 3) << 11 | ((g) >> 2) << 5 | (b) >> 3)

static const uint16_t C_BG = RGB(12, 12, 12), C_TEXT = RGB(225, 225, 225),
	C_H1 = RGB(255, 170, 60), C_H2 = RGB(250, 210, 90), C_H3 = RGB(200, 230, 120),
	C_CODE = RGB(150, 230, 120), C_CODEBG = RGB(40, 44, 40), C_LINK = RGB(120, 170, 255),
	C_RULE = RGB(110, 110, 110), C_BAR = RGB(90, 90, 90), C_FOOT = RGB(150, 150, 150),
	C_FOOTBG = RGB(30, 30, 46);

/* ------------------------------------------------------------ blocks */

enum { B_PARA, B_HEAD, B_ITEM, B_QUOTE, B_RULE, B_CODE, B_BLANK };

typedef struct {
	size_t start, end;	/* its bytes, newline included */
	int kind, level;	/* heading level; list or quotation depth */
	char bullet[8];		/* a list item's "•" or "12." */
	size_t text;		/* where its words start (past "# ", "- ", "> ") */
} block_t;

static size_t n_;
static char at(size_t i) { return i < n_ ? doc_at(i) : '\n'; }

static size_t line_end(size_t p) { while (p < n_ && doc_at(p) != '\n') p++; return p; }
static size_t skip_sp(size_t p) { while (p < n_ && (doc_at(p) == ' ' || doc_at(p) == '\t')) p++; return p; }

/* what a line starting at p begins: a heading, an item, a quotation...;
   *text where its words start, *level, *bullet */
static int line_kind(size_t p, size_t *text, int *level, char *bullet)
{
	size_t e = line_end(p), q = skip_sp(p);
	int ind = (int)(q - p);
	*text = q;
	*level = 0;
	if (bullet) bullet[0] = 0;
	if (q == e) return B_BLANK;
	char c = at(q);
	if (ind < 4 && c == '#') {
		int l = 0;
		while (l < 7 && at(q + l) == '#') l++;
		if (l <= 6 && (at(q + l) == ' ' || q + l == e)) {
			*level = l;
			*text = skip_sp(q + l);
			return B_HEAD;
		}
	}
	if (ind < 4 && (c == '-' || c == '*' || c == '_')) {	/* a rule: three or more alike */
		int k = 0;
		size_t r;
		for (r = q; r < e && (at(r) == c || at(r) == ' '); r++) k += at(r) == c;
		if (r == e && k >= 3) return B_RULE;
	}
	if (ind < 4 && c == '`' && at(q + 1) == '`' && at(q + 2) == '`') return B_CODE;
	if (ind < 4 && c == '>') {
		size_t r = q;
		while (at(r) == '>') { (*level)++; r = skip_sp(r + 1); }
		*text = r;
		return B_QUOTE;
	}
	if ((c == '-' || c == '*' || c == '+') && (at(q + 1) == ' ' || q + 1 == e)) {
		*level = ind / 2 + 1;
		*text = skip_sp(q + 1);
		if (bullet) strcpy(bullet, *level > 1 ? "\xE2\x80\x93" : "\xE2\x80\xA2");	/* – or • */
		return B_ITEM;
	}
	if (c >= '0' && c <= '9') {
		size_t r = q;
		while (r < e && r - q < 9 && at(r) >= '0' && at(r) <= '9') r++;
		if ((at(r) == '.' || at(r) == ')') && (at(r + 1) == ' ' || r + 1 == e)) {
			*level = ind / 3 + 1;
			*text = skip_sp(r + 1);
			if (bullet) {
				size_t k = r + 1 - q;
				if (k > 7) k = 7;
				for (size_t i = 0; i < k; i++) bullet[i] = at(q + i);
				bullet[k] = 0;
			}
			return B_ITEM;
		}
	}
	return B_PARA;
}

/* the block starting at p (a line start, not blank) */
static void block_at(size_t p, block_t *b)
{
	memset(b, 0, sizeof *b);
	b->start = p;
	b->kind = line_kind(p, &b->text, &b->level, b->bullet);
	size_t e = line_end(p);
	if (b->kind == B_HEAD || b->kind == B_RULE) { b->end = e < n_ ? e + 1 : e; return; }
	if (b->kind == B_CODE) {		/* to the closing ``` */
		size_t q = e < n_ ? e + 1 : e;
		while (q < n_) {
			size_t t, qe = line_end(q);
			int l;
			bool fence = line_kind(q, &t, &l, NULL) == B_CODE;
			q = qe < n_ ? qe + 1 : qe;
			if (fence) break;
		}
		b->end = q;
		return;
	}
	/* a paragraph, item or quotation goes on over the lines after it
	   until a blank line or a line that starts something else */
	size_t q = e < n_ ? e + 1 : e;
	while (q < n_) {
		size_t t;
		int l, k = line_kind(q, &t, &l, NULL);
		if (k == B_BLANK || k == B_HEAD || k == B_RULE || k == B_CODE || k == B_ITEM) break;
		if (k == B_QUOTE && b->kind != B_QUOTE) break;
		if (k != B_QUOTE && b->kind == B_QUOTE) break;
		size_t qe = line_end(q);
		q = qe < n_ ? qe + 1 : qe;
	}
	b->end = q;
}

/* where every block starts */
static size_t *starts;
static int n_blocks;

static bool index_blocks(void)
{
	for (int pass = 0; pass < 2; pass++) {
		int n = 0;
		size_t p = 0;
		while (p < n_) {
			size_t t;
			int l;
			if (line_kind(p, &t, &l, NULL) == B_BLANK) {
				size_t e = line_end(p);
				p = e < n_ ? e + 1 : e;
				continue;
			}
			block_t b;
			block_at(p, &b);
			if (pass) starts[n] = p;
			n++;
			p = b.end > p ? b.end : p + 1;
		}
		if (!pass) {
			n_blocks = n;
			starts = malloc(sizeof *starts * (size_t)(n ? n : 1));
			if (!starts) return false;
		}
	}
	return true;
}

/* ------------------------------------------------------------ the words */

/* a character of a block's text, as the reader sees it: hidden marks
   skipped, the line breaks inside a paragraph as spaces, the "> " at the
   start of a quotation's lines and the indent of an item's skipped, links'
   addresses skipped */
typedef struct {
	size_t p;		/* the next byte */
	size_t link_end;	/* inside a link's text until here (then skip to skip_to) */
	size_t skip_to;
} cursor_t;

static const block_t *cb;	/* the block being read */

static size_t line_text(size_t p)	/* where a continuation line's words start */
{
	size_t q = skip_sp(p);
	if (cb->kind == B_QUOTE) while (at(q) == '>') q = skip_sp(q + 1);
	return q;
}

/* a link at p: "[text](address)" all on the block's lines; its text's end
   and the end of the address */
static bool link_at(size_t p, size_t *text_end, size_t *end)
{
	size_t q = p + 1, e = cb->end;
	int depth = 0;
	while (q < e && at(q) != '\n') {
		if (at(q) == '[') depth++;
		else if (at(q) == ']') { if (!depth) break; depth--; }
		q++;
	}
	if (q >= e || at(q) != ']' || at(q + 1) != '(') return false;
	size_t r = q + 2;
	while (r < e && at(r) != ')' && at(r) != '\n' && at(r) != ' ') r++;
	if (at(r) != ')') return false;
	*text_end = q;
	*end = r + 1;
	return true;
}

/* the next character from c, or 0 at the end; *style its F_ flags, plus
   0x100 for a link */
#define LINK 0x100
static unsigned next_ch(cursor_t *c, int *style)
{
	for (;;) {
		if (c->p >= cb->end) return 0;
		if (c->link_end && c->p >= c->link_end) { c->p = c->skip_to; c->link_end = 0; continue; }
		size_t p = c->p;
		unsigned char ch = (unsigned char)at(p);
		if (ch == '\n') {
			c->p = p + 1 < cb->end ? line_text(p + 1) : p + 1;
			if (c->p >= cb->end) return 0;
			*style = 0;
			return ' ';
		}
		uint8_t f = p < n_ ? doc_fmt(p) : 0;
		if (f & F_HIDDEN) { c->p = p + 1; continue; }
		if (ch == '[' && !(f & F_CODE) && !c->link_end) {
			size_t te, end;
			if (link_at(p, &te, &end)) { c->link_end = te; c->skip_to = end; c->p = p + 1; continue; }
		}
		*style = (f & (F_BOLD | F_ITAL | F_CODE)) | (c->link_end ? LINK : 0);
		/* UTF-8 */
		unsigned cp = ch;
		int more = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : ch >= 0xC0 ? 1 : 0;
		if (more) {
			cp = ch & (0x3F >> more);
			for (int k = 1; k <= more; k++) cp = cp << 6 | ((unsigned char)at(p + k) & 0x3F);
		}
		c->p = p + 1 + more;
		if (ch == '\t') cp = ' ';
		return cp;
	}
}

/* ------------------------------------------------------------ fonts */

static const rfont_t *face(const block_t *b, int style)
{
	if (b->kind == B_HEAD) return b->level == 1 ? &rfont_h1 : b->level == 2 ? &rfont_h2 : &rfont_h3;
	if (b->kind == B_CODE || (style & F_CODE)) return (&rfont_mono);
	if ((style & (F_BOLD | F_ITAL)) == (F_BOLD | F_ITAL)) return (&rfont_bolditalic);
	if (style & F_BOLD) return (&rfont_bold);
	if (style & F_ITAL) return (&rfont_italic);
	return (&rfont_body);
}

/* the glyph for cp in f, or in a face that has it; *f changes to that face */
static const rglyph_t *glyph(const rfont_t **f, unsigned cp)
{
	const rglyph_t *g = rfont_glyph(*f, cp);
	if (g && g->adv4) return g;
	if (*f == &rfont_h1 || *f == &rfont_h2 || *f == &rfont_h3) {
		*f = &rfont_bold;
		g = rfont_glyph(*f, cp);
		if (g && g->adv4) return g;
	}
	/* look-alikes for what the fonts don't have */
	unsigned alt = cp == 0x2212 ? '-' : cp == 0x00A0 ? ' ' : '?';
	return rfont_glyph(*f, alt);
}

static int adv4(const rfont_t *f, unsigned cp) { return glyph(&f, cp)->adv4; }

/* ------------------------------------------------------------ laying out */

typedef struct {
	cursor_t from;		/* where the line's words start */
	int width4;
} line_t;

static line_t lines[MAX_LINES];
static int n_lines;
static int laid_out = -1;	/* which block lines[] is for */
static block_t lb;		/* that block */

static int left(const block_t *b)
{
	if (b->kind == B_ITEM) return MARGIN + 16 * b->level;
	if (b->kind == B_QUOTE) return MARGIN + 12 * b->level;
	return MARGIN;
}

/* Page view lays the text out as it would print: a 6.5-inch line on Letter
   paper with one-inch margins, at 12 points, in units of the reading
   fonts' pixels (the 14-pixel face standing for 12 points). */
static bool paper;
#define PAPER_W 714		/* 8.5 inches */
#define PAPER_H 924		/* 11 inches */
#define PAPER_M 84		/* 1 inch */
#define PAPER_LINE (PAPER_W - 2 * PAPER_M)
#define PAPER_TEXT (PAPER_H - 2 * PAPER_M)
#define PS(u) ((u) * 8 / 25)	/* paper units to screen pixels: the page fills the screen's height */

static int line_height(const block_t *b)
{
	if (b->kind == B_HEAD) return face(b, 0)->height + 4;
	if (b->kind == B_RULE) return 12;
	if (paper) return rfont_body.height;
	return rfont_body.height + 2;
}

/* the space after a block */
static int gap_after(const block_t *b)
{
	if (b->kind == B_ITEM) return 3;
	if (b->kind == B_HEAD) return 2;
	return 7;
}

/* lay block bi out into lines[] */
static void lay_out(int bi)
{
	if (laid_out == bi) return;
	laid_out = bi;
	block_at(starts[bi], &lb);
	cb = &lb;
	n_lines = 0;
	if (lb.kind == B_RULE) { n_lines = 1; return; }
	int room4 = paper ? (PAPER_LINE - (left(&lb) - MARGIN)) * 4 : (W - MARGIN - left(&lb)) * 4;
	if (lb.kind == B_CODE) {		/* code: one line for each, cut at the edge */
		size_t q = line_end(lb.start) + 1;
		while (q < lb.end && n_lines < MAX_LINES) {
			size_t t;
			int l;
			if (line_kind(q, &t, &l, NULL) == B_CODE) break;
			lines[n_lines].from.p = q;
			lines[n_lines].from.link_end = 0;
			lines[n_lines].width4 = 0;
			n_lines++;
			q = line_end(q) + 1;
		}
		if (!n_lines) { lines[0].from.p = lb.end; n_lines = 1; }
		return;
	}
	cursor_t c = { lb.text, 0, 0 };
	for (;;) {
		/* one line: words while they fit */
		while (1) {		/* no spaces at the start of a line */
			cursor_t s = c;
			int st;
			unsigned ch = next_ch(&s, &st);
			if (ch != ' ') break;
			c = s;
		}
		cursor_t start = c, fit = c;
		int w4 = 0, fit_w4 = 0;
		bool any = false;
		for (;;) {
			cursor_t s = c;
			int st, ww = 0;
			unsigned ch = next_ch(&s, &st);
			if (!ch) { fit = s; fit_w4 = w4; break; }
			/* a word, and the space after it */
			cursor_t e = c;
			int sp = 0;
			for (;;) {
				cursor_t t = e;
				unsigned k = next_ch(&t, &st);
				if (!k || k == ' ') break;
				ww += adv4(face(&lb, st), k);
				e = t;
			}
			if (any && w4 + ww > room4) break;
			if (!any && ww > room4) {	/* a word too long for a line: cut it */
				cursor_t t = c;
				int cw = 0;
				for (;;) {
					cursor_t u = t;
					unsigned k = next_ch(&u, &st);
					if (!k || k == ' ') break;
					int a = adv4(face(&lb, st), k);
					if (cw + a > room4 && cw) break;
					cw += a;
					t = u;
				}
				fit = t;
				fit_w4 = cw;
				c = t;
				any = true;
				break;
			}
			w4 += ww;
			any = true;
			fit = e;
			fit_w4 = w4;
			cursor_t t = e;
			unsigned k = next_ch(&t, &st);
			if (k == ' ') { sp = adv4(face(&lb, st), ' '); w4 += sp; c = t; }
			else { c = e; break; }		/* the end of the block */
		}
		lines[n_lines].from = start;
		lines[n_lines].width4 = fit_w4;
		n_lines++;
		cursor_t s = fit;
		int st;
		/* past the spaces where the line broke */
		for (;;) {
			cursor_t t = s;
			unsigned k = next_ch(&t, &st);
			if (k != ' ') break;
			s = t;
		}
		c = s;
		cursor_t t = c;
		if (!next_ch(&t, &st) || n_lines >= MAX_LINES) break;
	}
	if (!n_lines) { lines[0].from = (cursor_t){ lb.text, 0, 0 }; n_lines = 1; }
}

/* the end of line li of the laid-out block: where the next one starts */
static cursor_t line_stop(int li)
{
	if (li + 1 < n_lines) return lines[li + 1].from;
	return (cursor_t){ lb.end, 0, 0 };
}

/* ------------------------------------------------------------ drawing */

static uint16_t px[W * 32];		/* one line of the screen */

static uint16_t blend(uint16_t fg, uint16_t bg, int a)	/* a: 0..3 */
{
	if (a == 0) return bg;
	if (a == 3) return fg;
	static const int weight[4] = { 0, 85, 170, 255 };
	int w = weight[a];
	int r = ((fg >> 11) * w + (bg >> 11) * (255 - w)) / 255;
	int g = (((fg >> 5) & 63) * w + ((bg >> 5) & 63) * (255 - w)) / 255;
	int b = ((fg & 31) * w + (bg & 31) * (255 - w)) / 255;
	return (uint16_t)(r << 11 | g << 5 | b);
}

static void fill(int x, int y, int w, int h, int lh, uint16_t c)
{
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			if (i >= 0 && i < W && j >= 0 && j < lh) px[j * W + i] = c;
}

/* draw cp in f with its pen at x (quarter pixels), baseline row base */
static void draw_glyph(const rfont_t *f, unsigned cp, int x4, int top, int lh, uint16_t fg, uint16_t bg)
{
	const rglyph_t *g = glyph(&f, cp);
	int x0 = x4 / 4 + g->x, y0 = top + g->y;
	const uint8_t *bits = f->bits + g->off;
	for (int j = 0; j < g->h; j++)
		for (int i = 0; i < g->w; i++) {
			int k = j * g->w + i;
			int a = bits[k >> 2] >> (6 - 2 * (k & 3)) & 3;
			int X = x0 + i, Y = y0 + j;
			if (a && X >= 0 && X < W && Y >= 0 && Y < lh) px[Y * W + X] = blend(fg, bg, a);
		}
}

/* draw line li of the laid-out block at row y; lh tall */
static void draw_line(int li, int y, int lh)
{
	for (int i = 0; i < W * lh; i++) px[i] = C_BG;
	const block_t *b = &lb;
	cb = b;
	int x4 = left(b) * 4;
	if (b->kind == B_RULE) {
		fill(MARGIN, lh / 2, W - 2 * MARGIN, 1, lh, C_RULE);
		ed_blit(0, y, W, lh, px);
		return;
	}
	if (b->kind == B_QUOTE)
		for (int d = 0; d < b->level; d++) fill(MARGIN + 12 * d, 0, 3, lh, lh, C_BAR);
	if (b->kind == B_CODE) fill(MARGIN - 2, 0, W - 2 * MARGIN + 4, lh, lh, C_CODEBG);
	int top = (lh - (b->kind == B_HEAD ? face(b, 0)->height : rfont_body.height)) / 2;
	if (b->kind == B_HEAD) top = lh - face(b, 0)->height - 1;
	if (b->kind == B_ITEM && li == 0) {	/* the bullet or number, right-aligned before the text */
		const char *s = b->bullet;
		int bw = 0;
		for (const char *q = s; *q; ) {
			unsigned char c0 = (unsigned char)*q;
			unsigned cp = c0;
			int more = c0 >= 0xE0 ? 2 : c0 >= 0xC0 ? 1 : 0;
			if (more) { cp = c0 & (0x3F >> more); for (int k = 1; k <= more; k++) cp = cp << 6 | (q[k] & 0x3F); }
			q += 1 + more;
			bw += adv4((&rfont_body), cp);
		}
		int bx = x4 - bw - 4 * 5;
		for (const char *q = s; *q; ) {
			unsigned char c0 = (unsigned char)*q;
			unsigned cp = c0;
			int more = c0 >= 0xE0 ? 2 : c0 >= 0xC0 ? 1 : 0;
			if (more) { cp = c0 & (0x3F >> more); for (int k = 1; k <= more; k++) cp = cp << 6 | (q[k] & 0x3F); }
			q += 1 + more;
			draw_glyph((&rfont_body), cp, bx, top, lh, C_TEXT, C_BG);
			bx += adv4((&rfont_body), cp);
		}
	}
	uint16_t text = b->kind == B_HEAD ? (b->level == 1 ? C_H1 : b->level == 2 ? C_H2 : C_H3)
	              : b->kind == B_CODE ? C_CODE : C_TEXT;
	if (b->kind == B_CODE) {
		size_t p = lines[li].from.p, e = line_end(p);
		for (; p < e; ) {
			unsigned char ch = (unsigned char)at(p);
			unsigned cp = ch;
			int more = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : ch >= 0xC0 ? 1 : 0;
			if (more) { cp = ch & (0x3F >> more); for (int k = 1; k <= more; k++) cp = cp << 6 | ((unsigned char)at(p + k) & 0x3F); }
			p += 1 + more;
			if (cp == '\t') cp = ' ';
			draw_glyph((&rfont_mono), cp, x4, top, lh, C_CODE, C_CODEBG);
			x4 += adv4((&rfont_mono), cp);
			if (x4 / 4 > W) break;
		}
		ed_blit(0, y, W, lh, px);
		return;
	}
	cursor_t c = lines[li].from, stop = line_stop(li);
	for (;;) {
		if (c.p >= stop.p && !(c.link_end && stop.link_end && c.p < stop.p)) break;
		int st = 0;
		cursor_t was = c;
		unsigned cp = next_ch(&c, &st);
		if (!cp) break;
		if (was.p >= stop.p) break;
		const rfont_t *f = face(b, st);
		int a = adv4(f, cp);
		uint16_t fg = (st & LINK) ? C_LINK : (st & F_CODE) ? C_CODE : text, bg = C_BG;
		if (st & F_CODE) { bg = C_CODEBG; fill(x4 / 4, 1, (a + 3) / 4 + 1, lh - 2, lh, bg); }
		if (cp != ' ') draw_glyph(f, cp, x4, top, lh, fg, bg);
		if (st & LINK) fill(x4 / 4, top + f->ascent + 2, (a + 3) / 4, 1, lh, C_LINK);
		x4 += a;
	}
	ed_blit(0, y, W, lh, px);
}

/* some text across the footer */
static void footer(const char *l, const char *r)
{
	for (int i = 0; i < W * FOOTER; i++) px[i] = C_FOOTBG;
	int x4 = MARGIN * 4, top = (FOOTER - rfont_body.height) / 2;
	for (const char *s = l; *s; s++) { draw_glyph((&rfont_body), (unsigned char)*s, x4, top, FOOTER, C_FOOT, C_FOOTBG); x4 += adv4((&rfont_body), (unsigned char)*s); }
	int w4 = 0;
	for (const char *s = r; *s; s++) w4 += adv4((&rfont_body), (unsigned char)*s);
	x4 = (W - MARGIN) * 4 - w4;
	for (const char *s = r; *s; s++) { draw_glyph((&rfont_body), (unsigned char)*s, x4, top, FOOTER, C_FOOT, C_FOOTBG); x4 += adv4((&rfont_body), (unsigned char)*s); }
	ed_blit(0, H - FOOTER, W, FOOTER, px);
}

/* ------------------------------------------------------------ moving */

typedef struct { int b, l; } spot_t;	/* a block, and a line in it */

static int block_h(int bi, int li)	/* line li's height, and the gap after it if it's the last */
{
	lay_out(bi);
	return line_height(&lb) + (li == n_lines - 1 ? gap_after(&lb) : 0);
}

static bool down(spot_t *s)
{
	lay_out(s->b);
	if (s->l + 1 < n_lines) { s->l++; return true; }
	if (s->b + 1 < n_blocks) { s->b++; s->l = 0; return true; }
	return false;
}

static bool up(spot_t *s)
{
	if (s->l > 0) { s->l--; return true; }
	if (s->b > 0) { s->b--; lay_out(s->b); s->l = n_lines - 1; return true; }
	return false;
}

/* the screen from s; whether it reached the end of the document */
static bool draw_screen(spot_t s)
{
	int y = 0;
	bool end = false;
	while (y < TEXT_H) {
		lay_out(s.b);
		int lh = line_height(&lb), h = block_h(s.b, s.l);
		if (y + lh > TEXT_H) break;
		draw_line(s.l, y, lh);
		if (h > lh) {			/* the gap after the block */
			int g = h - lh < TEXT_H - y - lh ? h - lh : TEXT_H - y - lh;
			for (int i = 0; i < W * g; i++) px[i] = C_BG;
			if (g > 0) ed_blit(0, y + lh, W, g, px);
		}
		y += h;
		if (!down(&s)) { end = true; break; }
	}
	if (y < TEXT_H) {			/* the rest of the screen */
		for (int i = 0; i < W * 32; i++) px[i] = C_BG;
		while (y < TEXT_H) { int g = TEXT_H - y < 32 ? TEXT_H - y : 32; ed_blit(0, y, W, g, px); y += g; }
	}
	return end;
}

/* how many lines fit from s on (at least one) */
static int screenful(spot_t s)
{
	int y = 0, n = 0;
	for (;;) {
		lay_out(s.b);
		int lh = line_height(&lb);
		if (y + lh > TEXT_H && n) break;
		y += block_h(s.b, s.l);
		n++;
		if (!down(&s)) break;
	}
	return n;
}

/* ------------------------------------------------------------ page view */

static spot_t *pages;		/* where each page starts */
static int n_pages;

/* break the document into pages; a heading never ends a page */
static bool paginate(void)
{
	free(pages);
	n_pages = 0;
	int cap = 16;
	pages = malloc(sizeof *pages * (size_t)cap);
	if (!pages) return false;
	spot_t s = { 0, 0 };
	int y = 0;
	pages[n_pages++] = s;
	for (;;) {
		lay_out(s.b);
		int lh = line_height(&lb), last = s.l == n_lines - 1;
		int need = lh;
		if (lb.kind == B_HEAD && last) {	/* and the line after it */
			spot_t n = s;
			if (down(&n)) { lay_out(n.b); need += gap_after(&lb) + line_height(&lb); lay_out(s.b); }
		}
		if (y > 0 && y + need > PAPER_TEXT) {
			if (n_pages == cap) {
				spot_t *more = realloc(pages, sizeof *pages * (size_t)(cap *= 2));
				if (!more) return false;
				pages = more;
			}
			pages[n_pages++] = s;
			y = 0;
		}
		lay_out(s.b);
		y += lh + (last ? gap_after(&lb) : 0);
		if (!down(&s)) break;
	}
	return true;
}

static int page_of(spot_t s)
{
	int p = 0;
	for (int i = 0; i < n_pages; i++)
		if (pages[i].b < s.b || (pages[i].b == s.b && pages[i].l <= s.l)) p = i;
	return p;
}

static const uint16_t C_DESK = RGB(48, 48, 54), C_PAPER = RGB(245, 245, 240),
	P_TEXT = RGB(120, 120, 120), P_BOLD = RGB(40, 40, 40), P_ITAL = RGB(160, 160, 160),
	P_HEAD = RGB(20, 20, 20), P_LINK = RGB(70, 110, 210), P_CODE = RGB(90, 150, 90),
	P_CODEBG = RGB(225, 232, 225), P_RULE = RGB(170, 170, 170), P_BAR = RGB(200, 200, 200);

/* a rectangle, in screen pixels, clipped to the strip sy..sy+sh */
static void prect(int x0, int y0, int x1, int y1, int sy, int sh, uint16_t c)
{
	if (y0 < sy) y0 = sy;
	if (y1 > sy + sh) y1 = sy + sh;
	if (x0 < 0) x0 = 0;
	if (x1 > W) x1 = W;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++) px[(y - sy) * W + x] = c;
}

/* page p, the rows sy..sy+sh of the screen */
static void draw_page_strip(int p, int sy, int sh)
{
	int pw = PS(PAPER_W), ph = PS(PAPER_H), px0 = (W - pw) / 2, py0 = (TEXT_H - ph) / 2;
	for (int i = 0; i < W * sh; i++) px[i] = C_DESK;
	prect(px0, py0, px0 + pw, py0 + ph, sy, sh, C_PAPER);
	spot_t s = pages[p], end = p + 1 < n_pages ? pages[p + 1] : (spot_t){ -1, 0 };
	int y = PAPER_M;
	for (;;) {
		if (s.b == end.b && s.l == end.l) break;
		lay_out(s.b);
		cb = &lb;
		int lh = line_height(&lb), last = s.l == n_lines - 1;
		int ty = py0 + PS(y), lhp = PS(lh) > 1 ? PS(lh) : 1;
		if (ty > sy + sh) break;
		if (ty + lhp >= sy) {
			int base = px0 + PS(PAPER_M), lx = base + PS(left(&lb) - MARGIN);
			int b0 = ty + lhp * 3 / 10, b1 = ty + lhp * 8 / 10;
			if (b1 <= b0) b1 = b0 + 1;
			if (lb.kind == B_HEAD) { b0 = ty + lhp / 4; b1 = ty + lhp * 9 / 10; }
			if (lb.kind == B_RULE) prect(base, ty + lhp / 2, base + PS(PAPER_LINE), ty + lhp / 2 + 1, sy, sh, P_RULE);
			else if (lb.kind == B_CODE) {
				prect(base - 1, ty, base + PS(PAPER_LINE) + 1, ty + lhp, sy, sh, P_CODEBG);
				size_t q = lines[s.l].from.p, e = line_end(q);
				int x4 = 0, run = -1;
				for (; q <= e; q++) {
					bool ink = q < e && at(q) != ' ' && at(q) != '\t';
					if (ink && run < 0) run = x4;
					if (!ink && run >= 0) { prect(lx + PS(run / 4), b0, lx + PS(x4 / 4) + 1, b1, sy, sh, P_CODE); run = -1; }
					if (q < e) x4 += adv4(&rfont_mono, (unsigned char)at(q));
				}
			} else {
				if (lb.kind == B_QUOTE)
					for (int d = 0; d < lb.level; d++) prect(base + PS(12 * d), ty, base + PS(12 * d) + 1, ty + lhp, sy, sh, P_BAR);
				if (lb.kind == B_ITEM && s.l == 0) prect(lx - 4, b0, lx - 2, b1, sy, sh, P_TEXT);
				cursor_t c = lines[s.l].from, stop = line_stop(s.l);
				int x4 = 0, run = -1, run_st = 0;
				for (;;) {
					cursor_t was = c;
					int st = 0;
					unsigned cp = was.p < stop.p ? next_ch(&c, &st) : 0;
					bool ink = cp && cp != ' ';
					if (run >= 0 && (!ink || st != run_st)) {	/* the end of a run of one style */
						uint16_t col = lb.kind == B_HEAD ? P_HEAD : (run_st & LINK) ? P_LINK : (run_st & F_CODE) ? P_CODE
						             : (run_st & F_BOLD) ? P_BOLD : (run_st & F_ITAL) ? P_ITAL : P_TEXT;
						int r0 = b0, r1 = b1;
						if (run_st & F_BOLD && lb.kind != B_HEAD) r0--;
						prect(lx + PS(run / 4), r0, lx + PS(x4 / 4) + 1, r1, sy, sh, col);
						run = -1;
					}
					if (!cp) break;
					if (ink && run < 0) { run = x4; run_st = st; }
					x4 += adv4(face(&lb, st), cp);
				}
			}
		}
		y += lh + (last ? gap_after(&lb) : 0);
		if (!down(&s)) break;
	}
}

static void draw_page(int p)
{
	for (int sy = 0; sy < TEXT_H; sy += 32) {
		int sh = TEXT_H - sy < 32 ? TEXT_H - sy : 32;
		draw_page_strip(p, sy, sh);
		ed_blit(0, sy, W, sh, px);
	}
}

size_t reader_run(size_t pos, const char *name, bool page_view)
{
	n_ = doc_len();
	laid_out = -1;
	if (!index_blocks() || !n_blocks) {
		free(starts);
		starts = NULL;
		return pos;
	}
	/* the block holding pos (or the one after it) */
	spot_t top = { 0, 0 };
	for (int i = 0; i < n_blocks && starts[i] <= pos; i++) top.b = i;
	for (;;) {
		int k = 'P';
		bool end = false;
		if (!page_view) {
			end = draw_screen(top);
			char r[32];
			int pct = n_blocks > 1 ? (int)((long)top.b * 100 / (n_blocks - 1)) : 100;
			snprintf(r, sizeof r, "%d%%", end ? 100 : pct);
			footer("Esc back   P page view", r);
			k = ed_key_wait(-1);
		}
		page_view = false;	/* (^OV: straight into page view, once) */
		int rows = screenful(top);
		switch (k) {
		case K_DOWN: case CTRL('X'): if (!end) down(&top); break;
		case K_UP: case CTRL('E'): up(&top); break;
		case K_PGDN: case CTRL('C'): case ' ':
			for (int i = 1; i < rows && !end; i++) if (!down(&top)) break;
			break;
		case K_PGUP: case CTRL('R'): {
			/* back a screen: as many lines as fit above */
			int y = 0;
			while (up(&top)) {
				y += block_h(top.b, top.l);
				if (y > TEXT_H - rfont_body.height) { down(&top); break; }
			}
			break;
		}
		case K_HOME: top.b = 0; top.l = 0; break;
		case K_END:
			top.b = n_blocks - 1;
			lay_out(top.b);
			top.l = n_lines - 1;
			{ int y = block_h(top.b, top.l); while (up(&top)) { y += block_h(top.b, top.l); if (y > TEXT_H) { down(&top); break; } } }
			break;
		case K_ESC: case K_ENTER: case CTRL('O'): case 'q': case 'Q': {
			size_t back = starts[top.b];
			free(starts);
			starts = NULL;
			return back;
		}
		case 'p': case 'P': {		/* page view, until P again or Esc */
			footer("Working out the pages...", "");
			paper = true;
			laid_out = -1;
			if (!paginate()) {
				paper = false;
				laid_out = -1;
				footer("Not enough memory for page view", "");
				ed_key_wait(1500);
				break;
			}
			int pg = page_of(top);
			bool leave = false;
			for (;;) {
				draw_page(pg);
				char l[40];
				snprintf(l, sizeof l, "Page %d of %d", pg + 1, n_pages);
				footer(l, "P reading   Esc back");
				int k2 = ed_key_wait(-1);
				if (k2 == K_PGDN || k2 == K_DOWN || k2 == K_RIGHT || k2 == ' ' || k2 == CTRL('C') || k2 == CTRL('X')) { if (pg + 1 < n_pages) pg++; }
				else if (k2 == K_PGUP || k2 == K_UP || k2 == K_LEFT || k2 == CTRL('R') || k2 == CTRL('E')) { if (pg > 0) pg--; }
				else if (k2 == K_HOME) pg = 0;
				else if (k2 == K_END) pg = n_pages - 1;
				else if (k2 == 'p' || k2 == 'P') break;
				else if (k2 == K_ESC || k2 == K_ENTER || k2 == CTRL('O') || k2 == 'q' || k2 == 'Q') { leave = true; break; }
				else if (k2 == K_SHOT) {
					char info[64];
					snprintf(info, sizeof info, "page view, page %d of %d\n", pg + 1, n_pages);
					const char *nm = ed_screenshot_pixels(info);
					footer(nm ? "Screenshot saved:" : "The screenshot failed", nm ? nm : "");
					ed_key_wait(1500);
				}
			}
			top = pages[pg];		/* the reading view carries on from the page's top */
			paper = false;
			laid_out = -1;
			free(pages);
			pages = NULL;
			if (leave) {
				size_t back = starts[top.b];
				free(starts);
				starts = NULL;
				return back;
			}
			/* (a page's first line in the reading view is a line of the
			   same block, but the lines are wider there: start the block) */
			top.l = 0;
			break;
		}
		case K_SHOT: {
			char info[128];
			snprintf(info, sizeof info, "reading view, block %d of %d, line %d\n",
			         top.b + 1, n_blocks, top.l + 1);
			const char *nm = ed_screenshot_pixels(info);
			footer(nm ? "Screenshot saved:" : "The screenshot failed", nm ? nm : "");
			ed_key_wait(1500);
			break;
		}
		}
	}
	(void)name;
}
