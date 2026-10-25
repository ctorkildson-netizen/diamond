/* host_main.c -- run Diamond's core on a PC with scripted keys.
 *
 *   host_diamond file.md 'keys...'
 *
 * In the keys: ^X is Ctrl+X, \n is Enter, {UP} {DOWN} {LEFT} {RIGHT} {HOME}
 * {END} {PGUP} {PGDN} {DEL} {BS} {ESC} {TAB} are those keys, {WAIT} is a
 * pause (so a prefix menu appears), and {SHOW} prints the screen.  The screen
 * is printed again when the keys run out, and the program stops there. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "editor.h"

static const char *keys;
static jmp_buf done;
static screen_t last;
static int shows;

static void print_screen(const screen_t *s)
{
	printf("+----------------------------------------------------------------+\n");
	int cr = -1, cc = -1;
	for (int r = 0; r < SCR_ROWS; r++) {
		putchar('|');
		for (int c = 0; c < SCR_COLS; c++) {
			char ch = s->ch[r][c];
			if (s->at[r][c] & A_CURSOR) { cr = r; cc = c; if (ch == ' ') ch = '_'; }
			putchar(ch);
		}
		printf("|\n");
		if (getenv("ATTRS")) {		/* the formatting, as letters under the text */
			static const char code[] = " SMKRQXYkfbiI123cm";
			bool any = false;
			for (int c = 0; c < SCR_COLS; c++) any |= (s->at[r][c] & 0x7F) >= A_BLOCK && (s->at[r][c] & 0x3F) != A_STATUS;
			if (any) {
				putchar(' ');
				for (int c = 0; c < SCR_COLS; c++) {
					int a = s->at[r][c] & 0x7F;
					if (a & A_INBLOCK) a = (a & 0x3F) == A_TEXT ? A_BLOCK : a & 0x3F;
					putchar(a == A_TEXT ? ' ' : a < (int)sizeof code - 1 ? code[a] : '?');
				}
				putchar('\n');
			}
		}
	}
	printf("+----------------------------------------------------------------+\n");
	printf("cursor at row %d, column %d\n", cr, cc);
}

void ed_show(const screen_t *s) { last = *s; }

/* the reading view's pixels; {PIC} writes them to pic-N.ppm */
static uint16_t fb[SCR_PX * SCR_PX];
static int pics;
void ed_blit(int x, int y, int w, int h, const uint16_t *px)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			if (x + i < SCR_PX && y + j < SCR_PX) fb[(y + j) * SCR_PX + x + i] = px[j * w + i];
}
void ed_redraw(void) {}
static void write_pic(void);
const char *ed_screenshot_pixels(const char *info) { printf("%s", info); write_pic(); return "host-pic"; }
static void write_pic(void)
{
	char name[32];
	snprintf(name, sizeof name, "pic-%d.ppm", ++pics);
	FILE *f = fopen(name, "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", SCR_PX, SCR_PX);
	for (int i = 0; i < SCR_PX * SCR_PX; i++) {
		uint16_t c = fb[i];
		fputc((c >> 11) << 3 | (c >> 13), f);
		fputc(((c >> 5) & 63) << 2 | ((c >> 9) & 3), f);
		fputc((c & 31) << 3 | ((c >> 2) & 7), f);
	}
	fclose(f);
	printf("(wrote %s)\n", name);
}

static int show_now(void)
{
	print_screen(&last);
	shows++;
	return 0;
}

int ed_key_wait(int ms)
{
	for (;;) {
		if (!*keys) {
			print_screen(&last);
			longjmp(done, 1);
		}
		if (*keys == '^' && keys[1]) {
			int c = keys[1];
			keys += 2;
			return c & 0x1F;
		}
		if (*keys == '\n') { keys++; return K_ENTER; }
		if (*keys == '{') {
			static const struct { const char *n; int k; } names[] = {
				{ "UP", K_UP }, { "DOWN", K_DOWN }, { "LEFT", K_LEFT }, { "RIGHT", K_RIGHT },
				{ "HOME", K_HOME }, { "END", K_END }, { "PGUP", K_PGUP }, { "PGDN", K_PGDN },
				{ "DEL", K_DEL }, { "BS", K_BS }, { "ESC", K_ESC }, { "TAB", K_TAB },
				{ "WAIT", -2 }, { "SHOW", -3 }, { "SHOT", K_SHOT }, { "PIC", -4 },
			};
			const char *e = strchr(keys, '}');
			if (e) {
				size_t n = (size_t)(e - keys - 1);
				for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
					if (strlen(names[i].n) == n && strncmp(keys + 1, names[i].n, n) == 0) {
						keys = e + 1;
						if (names[i].k == -2) { if (ms >= 0) return K_NONE; goto next; }
						if (names[i].k == -3) { show_now(); goto next; }
						if (names[i].k == -4) { write_pic(); goto next; }
						return names[i].k;
					}
			}
		}
		return (unsigned char)*keys++;
	next:;
	}
}

bool ed_io_load(const char *path, char *buf, size_t cap, size_t *len, const char **err)
{
	FILE *f = fopen(path, "rb");
	if (!f) { *err = "can't open it"; return false; }
	*len = fread(buf, 1, cap, f);
	bool more = fgetc(f) != EOF;
	fclose(f);
	if (more) { *err = "it's too big"; return false; }
	return true;
}

const char *ed_io_save(const char *path, const char *a, size_t alen,
                       const char *b, size_t blen, bool crlf)
{
	char bak[512];
	snprintf(bak, sizeof bak, "%s.bak", path);
	rename(path, bak);
	FILE *f = fopen(path, "wb");
	if (!f) return "can't write it";
	for (int part = 0; part < 2; part++) {
		const char *p = part ? b : a;
		size_t n = part ? blen : alen;
		for (size_t i = 0; i < n; i++) {
			if (crlf && p[i] == '\n') fputc('\r', f);
			fputc(p[i], f);
		}
	}
	fclose(f);
	return NULL;
}

void ed_save_help_level(int level) { printf("(help level saved: %d)\n", level); }

const char *ed_screenshot(const screen_t *s, const char *info)
{
	print_screen(s);
	printf("%s", info);
	return "host-shot";
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: host_diamond file 'keys' [help level] [capacity]\n"); return 2; }
	keys = argv[2];
	int help = argc > 3 ? atoi(argv[3]) : 3;
	size_t cap = argc > 4 ? (size_t)atol(argv[4]) : 384 * 1024, n = 0;
	char *text = malloc(cap);
	const char *err = NULL;
	if (!ed_io_load(argv[1], text, cap, &n, &err)) { printf("not loaded: %s\n", err); n = 0; }
	if (setjmp(done)) return 0;
	int r = ed_run(argv[1], text, n, cap, help);
	print_screen(&last);
	printf("ed_run returned %s\n", r == ED_EXIT ? "ED_EXIT" : "ED_CLOSE");
	return 0;
}
