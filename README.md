# Diamond

A word processor for the ClockworkPi **PicoCalc** with an RP2350 (Pico 2 W),
UF2 Loader and Blair Leduc's
[PicoCalc Shell](https://github.com/BlairLeduc/picocalc-shell), with
WordStar's keys. It saves documents as Markdown, so anything can read them.

The Markdown marks are hidden while you write: bold shows as bold, italic as
italic, headings in their colours, and a quotation's `>` as a bar. ^OP opens
a reading view, in proportional type with real bold and italic, and ^OV a
page view, which shows each page as it would print on Letter paper.

The name is WordStar's: the cursor keys ^E ^S ^D ^X sit in a diamond under
the left hand.

**[welcome.md](testdocs/welcome.md)** is the guide: a short tutorial to read
in Diamond itself, with a section on where Diamond and WordStar differ.

## Trying it

1. Copy `diamond.uf2` (from the latest release) into `pico2-apps` on the SD card.
2. Make a folder called `/documents` on the card, and copy
   [`welcome.md`](testdocs/welcome.md) into it.
3. Run diamond from the shell, and open welcome.md.

Diamond starts in `/documents`, or at the top of the card if there's no
such folder. When you save, the previous version is kept as a `.bak` file.
F5 saves a screenshot in `/screenshots`. Files are dated when the shell's
clock is set (shell v1.1 or later).

## With a Bluetooth keyboard

`diamond-bt.uf2` is Diamond with a Bluetooth keyboard as well as the
PicoCalc's own; both work at once. Pair the keyboard first with **btpair**
from [picocalc-extras](https://github.com/ctorkildson-netizen/picocalc-extras/tree/main/bluetooth),
then run diamond-bt and press a key on the keyboard: it connects in a few
seconds. It needs a Pico 2 W, and it's about three times the size of the
plain `diamond.uf2` (the Bluetooth stack and the radio's firmware), so it
loads more slowly; the plain one is the same Diamond without it.

## What it is and isn't

- An editor for text. It doesn't show pictures, and it isn't a web
  browser: it shows a link, but can't open one.
- Markdown files (ending in `.md` or `.markdown`) are formatted. Any other
  file is plain text: nothing in it is formatting, and every character shows
  as typed.
- A document can be up to 384 KB, as memory allows (the opening screen
  says how much room there is). A single paragraph longer than 8 KB is shown
  without formatting.
- The editing screen uses the PicoCalc's built-in font, which is plain
  ASCII: curly quotes, dashes and accented letters show as their plain
  look-alikes (é as e), and anything with no look-alike as `?`. The file
  keeps them unchanged, and the reading view shows accented letters, curly
  quotes and dashes properly.
- WordStar's own files (with their control codes) aren't read or written.

## Markdown support

In the style of [markdownguide.org](https://www.markdownguide.org/tools/)'s
tables:

| Element | Support | Notes |
|---|---|---|
| Headings | Yes | `#` style only; underlined (`===`) headings show as plain text |
| Paragraphs | Yes | |
| Line Breaks | No | The reading view joins a paragraph's lines into one |
| Bold | Yes | |
| Italic | Yes | |
| Blockquotes | Yes | Nested too |
| Ordered Lists | Yes | |
| Unordered Lists | Yes | Nested too |
| Code | Yes | |
| Horizontal Rules | Yes | |
| Links | Yes | The `[text](address)` form; reference links aren't supported |
| Images | No | Shown as typed |
| Tables | Partial | The reading view lines up the columns; text in a cell isn't formatted |
| Fenced Code Blocks | Yes | |
| Syntax Highlighting | No | |
| Footnotes | No | |
| Heading IDs | No | |
| Definition Lists | No | |
| Strikethrough | No | Shown as typed |
| Task Lists | No | Shown as a list item with `[ ]` in it |
| Emoji (copy and paste) | No | The fonts have no emoji |
| Emoji (shortcodes) | No | |
| Highlight | No | |
| Subscript | No | |
| Superscript | No | |
| Automatic URL Linking | No | |
| Disabling Automatic URL Linking | N/A | |
| HTML | No | Shown as typed |

Indented code blocks (four spaces) aren't supported either; use a fence.
[`testdocs/`](testdocs/) has a document for each of these, and
[beyond-basics.md](testdocs/beyond-basics.md) shows what Diamond leaves alone.

## Building

With the Pico SDK 2.x, and `PICO_SDK_PATH` pointing at it:

```
git clone https://github.com/ctorkildson-netizen/diamond
cd diamond
git submodule update --init --recursive
mkdir build && cd build
cmake .. && make
```

That makes `diamond.uf2`. With `cmake .. -DDIAMOND_BT=ON` (in a build
folder of its own) it makes `diamond-bt.uf2` instead.

The source is in [`src/`](src/): [`editor.c`](src/editor.c) is the editing,
with the Markdown formatting; [`reader.c`](src/reader.c) the reading and page
views; [`main.c`](src/main.c) the PicoCalc's side (the screen, keys, files and
the list of documents). [`host/`](host/) builds the editor on a PC, drawing the
screen as text, for testing. [`fonts/mkfonts.py`](fonts/mkfonts.py) makes the
reading view's fonts (`src/rfonts.c`) from DejaVu Sans and Sans Mono.

## Submodules

| Submodule | Pinned to |
|---|---|
| [picocalc-extras](https://github.com/ctorkildson-netizen/picocalc-extras) | v1.1 (the list of documents, and the Bluetooth keyboard for diamond-bt) |
| [picocalc-shell](https://github.com/BlairLeduc/picocalc-shell) (Blair Leduc) | v1.1 (the clock) |
| [picocalc-text-starter](https://github.com/BlairLeduc/picocalc-text-starter) (Blair Leduc) | v0.15 plus four fixes, from the `diamond-dev` branch of [a fork](https://github.com/ctorkildson-netizen/picocalc-text-starter): FAT32 reading and writing without walking a file's chain from the start, file dates, and text clipped at the screen's edge |

The fixes have been offered to Blair. When a release of his includes them,
the submodule goes back to his repo, pinned to that release. Dependabot opens
a pull request here when a submodule's repo moves on.

## Licence

MIT (see [LICENSE](LICENSE)). The reading view's fonts are made from
[DejaVu](https://dejavu-fonts.github.io/) fonts, which keep their own
licence ([fonts/LICENSE-DejaVu.txt](fonts/LICENSE-DejaVu.txt)).

## Who made it

Designed and tested by Chris Torkildson, on his PicoCalc. The code was
written by Claude (Anthropic's AI), working with him; each commit says so.

## Status

An alpha: tested by one user on one PicoCalc. Back up anything you care
about. Reports and improvements are welcome.
