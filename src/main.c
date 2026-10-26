/* main.c -- Diamond on the PicoCalc: the keyboard, the screen, the files,
 * and the list of documents.  The editing itself is in editor.c. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "pico/stdlib.h"
#include "drivers/lcd.h"
#include "drivers/font.h"
#include "drivers/keyboard.h"
#include "drivers/southbridge.h"
#include "drivers/fat32.h"
#include "picocalc_shell.h"
#include "picker.h"
#include "editor.h"
#ifdef DIAMOND_BT
#include "btkbd.h"		/* a Bluetooth keyboard: its keys come through sb_read_keyboard() */
#endif

volatile bool user_interrupt;	/* the keyboard driver sets it on Brk */

/* ------------------------------------------------------------ keyboard */

/* The text-starter driver looks at the keyboard every 100 ms, one event at a
   time, which is slower than typing, and doesn't repeat a held key.  While
   editing, Diamond reads the keyboard itself, often, and repeats. */

#define KQ 64
static int kq[KQ];
static unsigned kq_head, kq_tail;
static bool m_ctrl, m_shift, m_alt;
/* Sticky Ctrl: Ctrl pressed and let go on its own makes the next letter a
   Ctrl+letter, so no two keys need holding at once (the one Ctrl key is in
   a corner, and the other hand is holding the PicoCalc) */
static bool ctrl_used, sticky;
static void show_sticky(bool on);
static uint8_t held;			/* the key held down (0: none) */
static int held_key;			/* what it gives */
static absolute_time_t next_repeat;

#define REPEAT_DELAY_MS 450
#define REPEAT_EVERY_MS 50

static void kq_put(int k)
{
	unsigned n = (kq_head + 1) % KQ;
	if (n == kq_tail) return;		/* full: drop it */
	kq[kq_head] = k;
	kq_head = n;
}

static int translate(uint8_t c)
{
	switch (c) {
	case KEY_ENTER: case KEY_RETURN: return K_ENTER;
	case KEY_BACKSPACE: return K_BS;
	case KEY_TAB: return K_TAB;
	case KEY_ESC: return K_ESC;
	case KEY_UP: return K_UP;
	case KEY_DOWN: return K_DOWN;
	case KEY_LEFT: return K_LEFT;
	case KEY_RIGHT: return K_RIGHT;
	case KEY_HOME: return K_HOME;
	case KEY_END: return K_END;
	case KEY_PAGE_UP: return K_PGUP;
	case KEY_PAGE_DOWN: return K_PGDN;
	case KEY_DEL: return K_DEL;
	case KEY_INSERT: return K_INS;
	case KEY_F5: return K_SHOT;		/* with Shift or without */
	}
	if (isalpha(c)) {
		if (m_ctrl || sticky) return c & 0x1F;
		if (m_shift) return toupper(c);
		return c;
	}
	if (c >= 0x20 && c < 0x7F) return c;
	return -1;
}

/* read everything the keyboard has queued */
static void kbd_poll(void)
{
	for (int i = 0; i < 16; i++) {
		uint16_t e = sb_read_keyboard();
		uint8_t state = e >> 8, code = e & 0xFF;
		if (state == 0) break;
		bool down = state == KEY_STATE_PRESSED, up = state == KEY_STATE_RELEASED;
		switch (code) {
		case KEY_MOD_CTRL:
			if (down) { m_ctrl = true; ctrl_used = false; }
			else if (up) {
				m_ctrl = false;
				if (!ctrl_used) { sticky = !sticky; show_sticky(sticky); }	/* a tap: on, or off again */
			}
			continue;
		case KEY_MOD_SHL: case KEY_MOD_SHR: if (down || up) m_shift = down; continue;
		case KEY_MOD_ALT: if (down || up) m_alt = down; continue;
		case KEY_MOD_SYM: continue;
		}
		if (down) {
			if (m_ctrl) ctrl_used = true;
			int k = translate(code);
			if (sticky) { sticky = false; show_sticky(false); }	/* used, or cancelled by another key */
			if (k < 0) continue;
			kq_put(k);
			held = code;
			held_key = k;
			next_repeat = make_timeout_time_ms(REPEAT_DELAY_MS);
		} else if (up && code == held) {
			held = 0;
		}
	}
	if (held && time_reached(next_repeat)) {
		kq_put(held_key);
		next_repeat = make_timeout_time_ms(REPEAT_EVERY_MS);
	}
}

int ed_key_wait(int ms)
{
	absolute_time_t until = ms >= 0 ? make_timeout_time_ms(ms) : at_the_end_of_time;
	for (;;) {
		kbd_poll();
		if (kq_head != kq_tail) {
			int k = kq[kq_tail];
			kq_tail = (kq_tail + 1) % KQ;
			return k;
		}
		if (time_reached(until)) return K_NONE;
		sleep_ms(3);
	}
}

/* ------------------------------------------------------------ screen */

enum { HEAVY = 1, SLANT = 2, UNDER = 4 };	/* bold: each dot doubled to the right; italic: the top leans right; a link: underlined */
static const struct { uint16_t fg, bg; uint8_t look; } colours[] = {
	[A_TEXT]   = { 0xC618, 0x0000 },	/* light grey on black */
	[A_STATUS] = { 0xFFFF, 0x0010 },	/* white on navy */
	[A_MENU]   = { 0x8410, 0x0000 },
	[A_KEY]    = { 0xFFE0, 0x0000 },	/* yellow */
	[A_RULER]  = { 0x4A69, 0x0000 },
	[A_MSG]    = { 0x0000, 0xFFE0 },	/* black on yellow: a question */
	[A_BOX]    = { 0xFFFF, 0x0010 },
	[A_BOXKEY] = { 0xFFE0, 0x0010 },
	[A_BLOCK]  = { 0x0000, 0xA514 },	/* black on grey: the marked block */
	[A_FOUND]  = { 0x0000, 0x07E0 },	/* black on green: just found */
	[A_BOLD]   = { 0xFFFF, 0x0000, HEAVY },		/* white */
	[A_ITALIC] = { 0x867F, 0x0000, SLANT },		/* sky blue */
	[A_BOLDITALIC] = { 0x867F, 0x0000, HEAVY | SLANT },
	[A_H1]     = { 0xFD20, 0x0000, HEAVY },		/* orange */
	[A_H2]     = { 0xFEA0, 0x0000, HEAVY },		/* gold */
	[A_H3]     = { 0xAFE5, 0x0000 },		/* yellow-green */
	[A_CODE]   = { 0x9772, 0x2104 },		/* green on dark grey */
	[A_MARK]   = { 0x632C, 0x0000 },		/* dim grey: a Markdown mark */
	[A_LINK]   = { 0x7D5F, 0x0000, UNDER },	/* blue, underlined */
	[A_LINEMARK] = { 0x9CD3, 0x0000 },		/* grey: drawn as a bar or a bullet */
};

static screen_t shown;
static bool shown_valid;
static bool pixels;			/* the reading view has the screen */

/* one row of cells as pixels */
static uint16_t px[SCR_COLS * 5 * GLYPH_HEIGHT];

static void render_row(const screen_t *s, int r)
{
	for (int c = 0; c < SCR_COLS; c++) {
		uint8_t a = s->at[r][c];
		uint16_t fg = colours[a & 0x3F].fg, bg = colours[a & 0x3F].bg;
		uint8_t look = colours[a & 0x3F].look;
		if (a & A_INBLOCK) { fg = colours[A_BLOCK].fg; bg = colours[A_BLOCK].bg; }	/* the block's colours, the text's shapes */
		if ((a & A_CURSOR) && (a & 0x3F) == A_FOUND) { fg = 0x0000; bg = 0xFFE0; }	/* black on yellow: the cursor on what was just found */
		else if (a & A_CURSOR) { uint16_t t = fg; fg = bg ? bg : 0x0000; bg = t; }
		const uint8_t *g = &font_5x10.glyphs[(uint8_t)s->ch[r][c] * GLYPH_HEIGHT];
		static const uint8_t bar[GLYPH_HEIGHT] = { 8, 8, 8, 8, 8, 8, 8, 8, 8, 8 };	/* a quotation's > */
		static const uint8_t dot[GLYPH_HEIGHT] = { 0, 0, 0, 0, 12, 12, 0, 0, 0, 0 };	/* a list item's - */
		if ((a & 0x3F) == A_LINEMARK) g = s->ch[r][c] == '>' ? bar : dot;
		/* letters whose strokes sit one pixel apart (M, W, X, m, w...) would
		   run together if thickened, so they stay as they are (bold still
		   shows in its brighter colour) */
		bool heavy = (look & HEAVY) && !strchr("MWXVmwxv", s->ch[r][c] ? s->ch[r][c] : ' ');
		for (int y = 0; y < GLYPH_HEIGHT; y++) {
			uint16_t *p = px + y * SCR_COLS * 5 + c * 5;
			uint8_t bits = g[y];
			if (look & SLANT && y < 4) bits >>= 1;
			if (heavy) bits |= bits >> 1;
			if (look & UNDER && y == GLYPH_HEIGHT - 1) bits = 0x1F;
			for (int x = 0; x < 5; x++) p[x] = (bits & (0x10 >> x)) ? fg : bg;
		}
	}
}

static void draw_row(const screen_t *s, int r)
{
	render_row(s, r);
	lcd_blit(px, 0, (uint16_t)(r * GLYPH_HEIGHT), SCR_COLS * 5, GLYPH_HEIGHT);
}

/* ------------------------------------------------------------ screenshots */

static bool write_all(fat32_file_t *f, const char *p, size_t n);

static void put_le(uint8_t *p, uint32_t v, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* /screenshots/diamond-001.bmp and .txt: the picture, and the screen as
   text with Diamond's state, for bug reports */
/* the next free /screenshots/diamond-NNN name, or NULL */
static const char *shot_name(char *path, size_t size)
{
	static char name[48];
	fat32_file_t f;
	if (fat32_dir_create(&f, "/screenshots") == FAT32_OK) fat32_close(&f);
	int n;
	for (n = 1; n < 1000; n++) {
		snprintf(path, size, "/screenshots/diamond-%03d.bmp", n);
		if (fat32_open(&f, path) != FAT32_OK) break;
		fat32_close(&f);
	}
	if (n == 1000) return NULL;
	snprintf(name, sizeof name, "diamond-%03d", n);
	return name;
}

const char *ed_screenshot_pixels(const char *info)
{
	char path[64];
	const char *name = shot_name(path, sizeof path);
	if (!name) return NULL;
	const int W = SCR_PX, H = SCR_PX;
	uint8_t hdr[54] = { 'B', 'M' };
	put_le(hdr + 2, 54 + W * H * 3, 4);
	put_le(hdr + 10, 54, 4);
	put_le(hdr + 14, 40, 4);
	put_le(hdr + 18, W, 4);
	put_le(hdr + 22, H, 4);
	put_le(hdr + 26, 1, 2);
	put_le(hdr + 28, 24, 2);
	put_le(hdr + 34, W * H * 3, 4);
	fat32_file_t f;
	if (fat32_create(&f, path) != FAT32_OK) return NULL;
	bool ok = write_all(&f, (const char *)hdr, sizeof hdr);
	static uint16_t row[SCR_PX];
	static uint8_t line[SCR_PX * 3];
	for (int y = H - 1; ok && y >= 0; y--) {	/* read back from the screen, bottom row first */
		lcd_unblit(row, 0, (uint16_t)y, (uint16_t)W, 1);
		for (int x = 0; x < W; x++) {
			uint16_t v = row[x];
			line[x * 3 + 0] = (uint8_t)((v & 0x1F) << 3);
			line[x * 3 + 1] = (uint8_t)(((v >> 5) & 0x3F) << 2);
			line[x * 3 + 2] = (uint8_t)((v >> 11) << 3);
		}
		ok = write_all(&f, (const char *)line, sizeof line);
	}
	fat32_close(&f);
	if (!ok) return NULL;
	snprintf(path, sizeof path, "/screenshots/%s.txt", name);
	if (fat32_create(&f, path) != FAT32_OK) return NULL;
	ok = write_all(&f, "Diamond screenshot\r\n\r\n", 22);
	for (const char *p = info; ok && *p; p++)
		ok = *p == '\n' ? write_all(&f, "\r\n", 2) : write_all(&f, p, 1);
	fat32_close(&f);
	return ok ? name : NULL;
}

const char *ed_screenshot(const screen_t *s, const char *info)
{
	static char name[48];
	char path[64];
	fat32_file_t f;
	if (fat32_dir_create(&f, "/screenshots") == FAT32_OK) fat32_close(&f);
	int n;
	for (n = 1; n < 1000; n++) {
		snprintf(path, sizeof path, "/screenshots/diamond-%03d.bmp", n);
		if (fat32_open(&f, path) != FAT32_OK) break;
		fat32_close(&f);
	}
	if (n == 1000) return NULL;
	snprintf(name, sizeof name, "diamond-%03d", n);

	/* the picture: 320x320, 24-bit, bottom row first */
	const int W = SCR_COLS * 5, H = SCR_ROWS * GLYPH_HEIGHT;
	uint8_t hdr[54] = { 'B', 'M' };
	put_le(hdr + 2, 54 + W * H * 3, 4);
	put_le(hdr + 10, 54, 4);
	put_le(hdr + 14, 40, 4);
	put_le(hdr + 18, W, 4);
	put_le(hdr + 22, H, 4);
	put_le(hdr + 26, 1, 2);
	put_le(hdr + 28, 24, 2);
	put_le(hdr + 34, W * H * 3, 4);
	if (fat32_create(&f, path) != FAT32_OK) return NULL;
	bool ok = write_all(&f, (const char *)hdr, sizeof hdr);
	static uint8_t line[SCR_COLS * 5 * 3];
	for (int r = SCR_ROWS - 1; ok && r >= 0; r--) {
		render_row(s, r);
		for (int y = GLYPH_HEIGHT - 1; ok && y >= 0; y--) {
			for (int x = 0; x < W; x++) {
				uint16_t v = px[y * W + x];
				line[x * 3 + 0] = (uint8_t)((v & 0x1F) << 3);
				line[x * 3 + 1] = (uint8_t)(((v >> 5) & 0x3F) << 2);
				line[x * 3 + 2] = (uint8_t)((v >> 11) << 3);
			}
			ok = write_all(&f, (const char *)line, sizeof line);
		}
	}
	fat32_close(&f);
	if (!ok) return NULL;

	/* the text: the screen, where the cursor is, what each cell is drawn
	   as, and Diamond's state */
	snprintf(path, sizeof path, "/screenshots/%s.txt", name);
	if (fat32_create(&f, path) != FAT32_OK) return NULL;
	char b[160];
	int cr = -1, cc = -1;
	ok = write_all(&f, "Diamond screenshot\r\n\r\n", 22);
	for (int r = 0; ok && r < SCR_ROWS; r++) {
		int k = snprintf(b, sizeof b, "%2d |", r);
		for (int c = 0; c < SCR_COLS; c++) {
			char ch = s->ch[r][c];
			b[k++] = ch >= 0x20 && ch < 0x7F ? ch : '.';
			if (s->at[r][c] & A_CURSOR) { cr = r; cc = c; }
		}
		k += snprintf(b + k, sizeof b - k, "|\r\n");
		ok = write_all(&f, b, (size_t)k);
	}
	int k = snprintf(b, sizeof b, "\r\ncursor at row %d, column %d\r\n\r\ncell kinds:\r\n", cr, cc);
	if (ok) ok = write_all(&f, b, (size_t)k);
	for (int r = 0; ok && r < SCR_ROWS; r++) {
		k = snprintf(b, sizeof b, "%2d |", r);
		for (int c = 0; c < SCR_COLS; c++) b[k++] = (char)('0' + (s->at[r][c] & 0x3F));
		k += snprintf(b + k, sizeof b - k, "|\r\n");
		ok = write_all(&f, b, (size_t)k);
	}
	if (ok) ok = write_all(&f, "\r\n", 2);
	for (const char *p = info; ok && *p; p++)
		ok = *p == '\n' ? write_all(&f, "\r\n", 2) : write_all(&f, p, 1);
	fat32_close(&f);
	return ok ? name : NULL;
}

void ed_blit(int x, int y, int w, int h, const uint16_t *px)
{
	pixels = true;
	lcd_blit((uint16_t *)px, (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);
}

void ed_redraw(void) { shown_valid = false; }

/* "Ctrl" at the right of the status line while a tapped Ctrl waits */
static void show_sticky(bool on)
{
	if (pixels || !shown_valid) return;
	static screen_t tmp;
	tmp = shown;
	if (on) {
		const char *w = "Ctrl";
		for (int i = 0; w[i]; i++) { tmp.ch[0][59 + i] = w[i]; tmp.at[0][59 + i] = A_MSG; }
	}
	draw_row(&tmp, 0);
	shown.ch[0][59] = on ? 0 : shown.ch[0][59];	/* (the next ed_show draws the row afresh) */
}

void ed_show(const screen_t *s)
{
	pixels = false;
	for (int r = 0; r < SCR_ROWS; r++)
		if (!shown_valid || memcmp(s->ch[r], shown.ch[r], SCR_COLS) ||
		    memcmp(s->at[r], shown.at[r], SCR_COLS))
			draw_row(s, r);
	shown = *s;
	shown_valid = true;
}

/* ------------------------------------------------------------ files */

/* The document's memory: one block, taken at the start, as big as can be
   had while leaving room for the rest (the list of files, ^U, the stack). */
static char *doc;
static size_t doc_cap;

static void take_memory(void)
{
	for (size_t kb = 384; kb >= 64 && !doc; kb -= 32)
		if ((doc = malloc(kb * 1024))) doc_cap = kb * 1024;
}

bool ed_io_load(const char *path, char *buf, size_t cap, size_t *len, const char **err)
{
	fat32_file_t f;
	if (fat32_open(&f, path) != FAT32_OK) { *err = "can't be opened"; return false; }
	uint32_t n = fat32_size(&f);
	if (n > cap) {
		fat32_close(&f);
		static char m[40];
		snprintf(m, sizeof m, "is too big (%u KB at most)", (unsigned)(cap / 1024));
		*err = m;
		return false;
	}
	size_t got = 0, total = 0;
	while (total < n) {
		if (fat32_read(&f, buf + total, n - total, &got) != FAT32_OK || got == 0) break;
		total += got;
	}
	fat32_close(&f);
	if (total != n) { *err = "couldn't be read"; return false; }
	*len = n;
	return true;
}

/* letter.md -> letter.bak */
static void bak_name(char *out, size_t n, const char *path)
{
	snprintf(out, n, "%s", path);
	char *slash = strrchr(out, '/'), *dot = strrchr(out, '.');
	if (dot && (!slash || dot > slash)) *dot = 0;
	strncat(out, ".bak", n - strlen(out) - 1);
}

static bool write_all(fat32_file_t *f, const char *p, size_t n)
{
	size_t done;
	while (n) {
		if (fat32_write(f, p, n, &done) != FAT32_OK || done == 0) return false;
		p += done;
		n -= done;
	}
	return true;
}

static bool write_piece(fat32_file_t *f, const char *p, size_t n, bool crlf)
{
	if (!crlf) return write_all(f, p, n);
	static char out[2048];
	size_t k = 0;
	for (size_t i = 0; i < n; i++) {
		if (k + 2 > sizeof out) { if (!write_all(f, out, k)) return false; k = 0; }
		if (p[i] == '\n') out[k++] = '\r';
		out[k++] = p[i];
	}
	return write_all(f, out, k);
}

bool ed_io_exists(const char *path)
{
	fat32_file_t f;
	if (fat32_open(&f, path) != FAT32_OK) return false;
	fat32_close(&f);
	return true;
}

/* write to name.tmp; then the old file becomes name.bak and name.tmp
   becomes the file, so a failed save never loses the old version */
const char *ed_io_save(const char *path, const char *a, size_t alen,
                       const char *b, size_t blen, bool crlf)
{
	char tmp[PICKER_PATH + 8], bak[PICKER_PATH + 8];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	bak_name(bak, sizeof bak, path);
	fat32_delete(tmp);
	fat32_file_t f;
	if (fat32_create(&f, tmp) != FAT32_OK) return "the card can't be written";
	bool ok = write_piece(&f, a, alen, crlf) && write_piece(&f, b, blen, crlf);
	fat32_close(&f);
	if (!ok) { fat32_delete(tmp); return "the card is full, or failed"; }
	fat32_file_t old;
	if (fat32_open(&old, path) == FAT32_OK) {
		fat32_close(&old);
		fat32_delete(bak);
		if (fat32_rename(path, bak) != FAT32_OK) { fat32_delete(path); }
	}
	if (fat32_rename(tmp, path) != FAT32_OK) return "it was saved as .tmp";
	return NULL;
}

/* ------------------------------------------------------------ settings */

#define CFG_DIR "/diamond"
#define CFG_PATH "/diamond/diamond.cfg"

static int read_help_level(void)
{
	fat32_file_t f;
	if (fat32_open(&f, CFG_PATH) != FAT32_OK) return 3;
	char b[256];
	size_t got = 0;
	fat32_read(&f, b, sizeof b - 1, &got);
	fat32_close(&f);
	b[got] = 0;
	char *h = strstr(b, "help");
	if (!h) return 3;
	h = strchr(h, '=');
	if (!h) return 3;
	int v = atoi(h + 1);
	return v < 0 ? 0 : v > 3 ? 3 : v;
}

void ed_save_help_level(int level)
{
	fat32_file_t d;
	if (fat32_open(&d, CFG_DIR) == FAT32_OK) fat32_close(&d);
	else if (fat32_dir_create(&d, CFG_DIR) == FAT32_OK) fat32_close(&d);
	char b[96];
	int n = snprintf(b, sizeof b, "# Diamond's settings\r\nhelp = %d\r\n", level);
	fat32_delete(CFG_PATH);
	fat32_file_t f;
	if (fat32_create(&f, CFG_PATH) != FAT32_OK) return;
	write_all(&f, b, (size_t)n);
	fat32_close(&f);
}

/* ------------------------------------------------------------ the list */

static bool is_document(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot) return false;
	return !strcasecmp(dot, ".md") || !strcasecmp(dot, ".txt") || !strcasecmp(dot, ".markdown");
}

/* lcd_putstr, cut to fit the screen: the driver's own doesn't stop at the
   edge, and a string too long for it overruns its line buffer */
static void put_text(int col, int row, const char *s)
{
	char b[64];
	int room = lcd_get_columns() - col;
	if (room <= 0) return;
	snprintf(b, sizeof b, "%.*s", room < (int)sizeof b - 1 ? room : (int)sizeof b - 1, s);
	lcd_putstr((uint8_t)col, (uint8_t)row, b);
}

/* the first lines of a document, in the list's panel */
static void preview(const char *path, int x, int y, int size)
{
	fat32_file_t f;
	if (fat32_open(&f, path) != FAT32_OK) return;
	char b[600];
	size_t got = 0;
	fat32_read(&f, b, sizeof b - 1, &got);
	fat32_close(&f);
	b[got] = 0;
	int cols = size / 5, rows = size / GLYPH_HEIGHT;
	lcd_set_font(&font_5x10);
	lcd_set_foreground(0xC618);
	lcd_set_background(0x10A2);
	char line[32];
	const char *p = b;
	for (int r = 0; r < rows && *p; r++) {
		int n = 0;
		while (*p && *p != '\n' && n < cols - 1) {
			unsigned char c = (unsigned char)*p++;
			if ((c & 0xC0) == 0x80) continue;
			line[n++] = c == '\r' ? ' ' : c >= 0x80 ? '?' : (char)c;
		}
		if (*p == '\n') p++;
		else while (*p && *p != '\n' && *p != ' ') p++;	/* a long line: the rest goes */
		line[n] = 0;
		put_text((uint8_t)(x / 5 + 1), (uint8_t)(y / GLYPH_HEIGHT + r), line);
	}
	lcd_set_background(0x0000);
}

/* a line of input on the screen; false on Esc */
static bool ask_line(const char *q, char *out, size_t n)
{
	size_t len = 0;
	out[0] = 0;
	lcd_clear_screen();
	lcd_set_font(&font_8x10);
	lcd_set_foreground(0xFFFF);
	put_text(1, 13, q);
	lcd_set_foreground(0x8410);
	put_text(1, 17, "Enter: done   Esc: never mind");
	for (;;) {
		char shown_[41];
		snprintf(shown_, sizeof shown_, "%-38s", out);
		lcd_set_foreground(0xFFE0);
		put_text(1, 15, shown_);
		lcd_putc((uint8_t)(1 + (len < 37 ? len : 37)), 15, '_');
		int k = ed_key_wait(-1);
		if (k == K_ESC) return false;
		if (k == K_ENTER) return len > 0;
		if (k == K_BS) { if (len) out[--len] = 0; continue; }
		if (k >= 0x20 && k < 0x7F && k != '/' && k != '\\' && len + 1 < n) {
			out[len++] = (char)k;
			out[len] = 0;
		}
	}
}

/* a problem: a heading, the file's name, what's wrong; any key goes on */
static void message(const char *a, const char *b, const char *c)
{
	lcd_clear_screen();
	lcd_set_font(&font_8x10);
	lcd_set_foreground(0xF800);
	put_text(1, 13, a);
	lcd_set_foreground(0xFFFF);
	put_text(1, 15, b);
	put_text(1, 16, c);
	lcd_set_foreground(0x8410);
	put_text(1, 19, "(any key to go on)");
	ed_key_wait(-1);
}

/* the sign-on screen, after WordStar's: a couple of seconds, or a key */
static void sign_on(void)
{
	static const struct { int row; uint16_t colour; const char *text; } lines[] = {
		{ 1, 0xC618, "Diamond Release 0.1  Serial #RP2350W" },
		{ 2, 0xC618, "Copyright (C) 2026 Chris Torkildson" },
		{ 3, 0xC618, "MIT licence: free to use and share" },
		{ 5, 0xC618, "PicoCalc, 320 x 320" },
		{ 6, 0xC618, "No printer (it saves for Word)" },
		{ 7, 0xC618, NULL },		/* the room for text, filled in below */
		{ 11, 0xFFFF, "Diamond" },
		{ 15, 0x8410, "after WordStar, by MicroPro" },
		{ 18, 0xC618, "Word wrap, the WordStar keys, and" },
		{ 19, 0xC618, "plain Markdown files that any" },
		{ 20, 0xC618, "computer can read." },
	};
	lcd_clear_screen();
	lcd_set_font(&font_8x10);
	lcd_set_background(0x0000);
	char room[40];
	snprintf(room, sizeof room, "Room for %u KB of text", (unsigned)(doc_cap / 1024));
	for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++) {
		lcd_set_foreground(lines[i].colour);
		put_text(lines[i].row == 11 ? 12 : 0, (uint8_t)lines[i].row, lines[i].text ? lines[i].text : room);
	}
	/* a blocky diamond where MicroPro had its three bars */
	for (int i = 0; i < 13; i++) {
		int half = (i < 6 ? i : 12 - i) * 6 + 4;
		lcd_solid_rectangle(0xC618, (uint16_t)(40 - half), (uint16_t)(80 + i * 4), (uint16_t)(2 * half), 3);
	}
	lcd_set_foreground(0xC618);
	put_text(0, 22, "_");
	ed_key_wait(2500);
}

static void to_shell(void)
{
	lcd_clear_screen();
	if (!picocalc_shell_exit()) {
		put_text(0, 15, "No SD card: switch off to leave.");
		for (;;) tight_loop_contents();
	}
}

/* edit a document; false if the user is leaving for the shell */
static bool edit(const char *path, bool is_new)
{
	/* from here Diamond reads the keyboard itself (see kbd_poll) */
	keyboard_set_background_poll(false);
	kq_head = kq_tail = 0;
	held = 0;
	int r = ED_CLOSE;
	size_t n = 0;
	const char *err = NULL;
	if (!doc) {
		message("Not enough memory to edit.", "", "");
	} else if (!is_new && !ed_io_load(path, doc, doc_cap, &n, &err)) {
		char b[48];
		snprintf(b, sizeof b, "It %s.", err);
		message("Can't open this document:", strrchr(path, '/') + 1, b);
	} else {
		shown_valid = false;
		lcd_clear_screen();
		r = ed_run(path, doc, n, doc_cap, read_help_level());
	}
	keyboard_set_background_poll(true);
	while (keyboard_key_available()) keyboard_get_key();
	return r != ED_EXIT;
}

/* The shell's clock (shell v1.1 and later), for dating the files Diamond
   saves: the local time, or false if the shell hasn't set it */
static bool shell_clock(uint16_t *date, uint16_t *time)
{
#ifdef PICOCALC_SHELL_HAS_TIME
	int64_t ms;
	int offset;
	if (!picocalc_shell_time(&ms, &offset)) return false;
	time_t t = (time_t)(ms / 1000) + (time_t)offset * 60;
	struct tm tm;
	gmtime_r(&t, &tm);
	if (tm.tm_year + 1900 < 1980 || tm.tm_year + 1900 > 2107) return false;
	fat32_pack_time(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
	                date, time);
	return true;
#else
	(void)date;
	(void)time;
	return false;
#endif
}

/* every file in dir, for ^KR's and ^KW's question */
int ed_dir_names(const char *dir, char *out, size_t size)
{
	fat32_file_t d;
	if (fat32_open(&d, dir) != FAT32_OK) return 0;
	int n = 0;
	size_t used = 0;
	fat32_entry_t e;
	for (;;) {
		if (fat32_dir_read(&d, &e) != FAT32_OK || !e.filename[0]) break;
		if (e.attr & (FAT32_ATTR_VOLUME_ID | FAT32_ATTR_HIDDEN | FAT32_ATTR_SYSTEM)) continue;
		if (e.filename[0] == '.') continue;
		bool isdir = (e.attr & FAT32_ATTR_DIRECTORY) != 0;
		size_t l = strlen(e.filename);
		if (used + l + 2 > size) break;
		memcpy(out + used, e.filename, l);
		used += l;
		if (isdir) out[used++] = '/';
		out[used++] = 0;
		n++;
	}
	fat32_close(&d);
	return n;
}

int main(void)
{
	sb_init();
	lcd_init();
	lcd_set_font(&font_8x10);
	lcd_enable_cursor(false);
	fat32_init();
	fat32_set_time_source(shell_clock);
	keyboard_init();
#ifdef DIAMOND_BT
	btkbd_init();
	void *pad = malloc(32 * 1024);	/* leave room for what Bluetooth takes as it connects */
	take_memory();
	free(pad);
#else
	take_memory();
#endif
	sign_on();
	kq_head = kq_tail = 0;
	keyboard_set_background_poll(true);

	static const char *const list_keys[] = { "  n              new document", NULL };
	static const picker_style_t style = {
		.title = "Diamond", .filter = is_document,
		.noun = "document", .nouns = "documents", .preview = preview,
		.keys = "nN", .keys_help = list_keys,
	};
	char dir[PICKER_PATH] = "/documents", path[PICKER_PATH];
	char last[FAT32_MAX_FILENAME_LEN + 1] = "";
	fat32_file_t d;
	if (fat32_open(&d, dir) == FAT32_OK) fat32_close(&d);
	else strcpy(dir, "/");

	for (;;) {
		int got = picker_run(&style, dir, last[0] ? last : NULL, path);
		if (!got) { to_shell(); continue; }
		bool go_on;
		if (got == PICKER_CHOSEN) {
			go_on = edit(path, false);
		} else {			/* n: a new document in this folder */
			keyboard_set_background_poll(false);
			char name[40];
			bool ok = ask_line("Name of the new document:", name, sizeof name);
			keyboard_set_background_poll(true);
			if (!ok) continue;
			if (!strchr(name, '.')) strncat(name, ".md", sizeof name - strlen(name) - 1);
			snprintf(path, sizeof path, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
			fat32_file_t f;
			bool exists = fat32_open(&f, path) == FAT32_OK;
			if (exists) fat32_close(&f);
			go_on = edit(path, !exists);
		}
		snprintf(last, sizeof last, "%s", strrchr(path, '/') + 1);
		if (!go_on) to_shell();
	}
}
