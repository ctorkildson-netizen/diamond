# Welcome to Diamond

Diamond is a word processor for the PicoCalc, with WordStar's keys. This document is a short tutorial: read it in Diamond, and try each thing as you go. Nothing you do here can hurt it. Close it with **^KQ** and answer Y to leave it as it was.

A note on the keys: **^S** means hold Ctrl and press S. **^KS** means Ctrl+K, then S. After the first key you can press the second one with Ctrl or without it, so ^K^S and ^KS are the same. And you never need to hold two keys at once: tap Ctrl on its own and let go ("Ctrl" shows at the top right), and the next letter you press counts as Ctrl and that letter.

## Moving around

The keys for moving are a diamond under your left hand, which is where the name comes from:

- **^E** up, **^X** down, **^S** left, **^D** right
- **^A** a word left, **^F** a word right
- **^R** a screen up, **^C** a screen down
- **^W** and **^Z** scroll the text a line at a time
- **^QS** and **^QD** go to the start and end of a line; **^QR** and **^QC** to the start and end of the document
- **^QG** and a character goes to the next one of it, **^QH** back to the one before
- **^QI** goes to a page, as the status line counts them (P01, P02...), or to a line: type L and its number

The arrow keys, Home, End, PgUp and PgDn work too.

Try it: go to the end of this line with ^QD, and back to its start with ^QS.

## Typing and deleting

Type anywhere and the text moves along to make room. **^V** switches to overtyping, where what you type replaces what was there; the status line at the top says which you're in.

- **^G** or **Del** deletes the character at the cursor, **Backspace** the one before it
- **^T** deletes a word, **^Y** the whole line
- **^QY** deletes to the end of the line, **^Q Backspace** to its start
- **^U** brings back what ^T, ^Y or ^QY deleted
- **^N** splits the line at the cursor

Try it: delete this sentence with ^Y, then bring it back with ^U.

## Blocks

A block is a piece of text you mark, then copy, move or delete:

1. Put the cursor at its start and press **^KB**.
2. Put the cursor at its end and press **^KK**. The block lights up.
3. Move the cursor to where it should go, and press **^KC** to copy it there or **^KV** to move it. **^KY** deletes it instead.

**^KH** hides the block (and shows it again). Try it: copy this paragraph to the end of the section.

## Finding and replacing

**^QF** finds text. **^QA** finds and replaces it. **^L** finds the next one. After what to find (and what to put in its place), Diamond asks for options, any of:

- **B** backwards, **U** ignoring capitals, **W** whole words only
- **G** the whole document, from the top
- **N** replace without asking each time

Try it: find the word "diamond" with ^QF, then press ^L to find the next.

## Bold and italic

Diamond saves documents in Markdown, a plain-text format almost everything can read. Its marks (`**` around bold, `*` around italic, `#` before a heading) are hidden while you write, and the text is shown in its style instead. **^OD** shows the marks, dimmed, and hides them again.

- **^PB** makes what you type next **bold**, and **^PY** *italic*; **^P Space** goes back to plain text.
- With a block marked, ^PB and ^PY make the block bold or italic, or plain again if it was already.

All of this is for Markdown files, ending in `.md`. A file ending in `.txt` is plain text: nothing in it is formatting, every character shows just as typed, and the reading view shows it in a typewriter face so its columns line up. Use `.txt` for lists of figures, code, logs, or anything full of asterisks.

Some examples to look at with ^OD: **bold words**, *italic words*, ***both together***, and `code`, which is shown as written: `**not bold**`. Marks that aren't formatting are left alone: 2 * 3 * 4, snake_case_name, and \*escaped stars\*.

## Headings, lists and quotations

A line starting with `#` and a space is a heading; `##` a smaller one, and so on to `######`. A line starting with `- ` is an item in a list, and `1. ` an item in a numbered one. A line starting with `> ` is a quotation.

- A bulleted item
- Another, long enough to show how a list item wraps onto a second line when it reaches the edge
  - A nested item, indented two spaces
1. A numbered item
2. And another

> A quotation, set off from the text around it.

## Seeing it as it reads

**^OP** opens the reading view: the document in proportional type, with real bold and italic, headings in their sizes, and the lists, quotations and links laid out. **^OV** opens page view, which shows each page as it would print, on Letter paper with one-inch margins. In either, P switches to the other, and Esc goes back to editing.

A link looks like this in the reading view: [the PicoCalc forum](https://forum.clockworkpi.com/). Diamond only shows a link; it isn't a web browser, so it can't open one. It doesn't show pictures either: a picture's Markdown is shown as typed.

## Saving, and the rest

- **^KS** saves and carries on; **^KD** saves and goes back to the list of documents; **^KX** saves and goes back to the shell; **^KQ** closes without saving.
- Saving keeps the previous version as a `.bak` file.
- **^K0** to **^K9** set place markers, and **^Q0** to **^Q9** go back to them.
- **^KR** reads another file in at the cursor, and **^KW** writes the block to a file of its own. Both list the files in this document's folder, and you type the whole name, ending and all (`notes.md`, not `notes`); a name that starts with / can be in another folder. Or press **^X** to go down into the list, move to a file with the arrows (or ^E ^S ^D ^X) and press Enter; choosing a folder lists its files.
- **^Q?** counts the words and characters (not counting the Markdown marks).
- **^QQ** repeats a command until you press a key: ^QQ^C pages down through the document, for reading. While it repeats, 0 to 9 set the speed, from fast to slow.
- **F5** saves a screenshot in `/screenshots`, as a picture and as text.
- **^J** is help: **^JJ** lists the keys, and **^JH** sets how much help the screen shows, from 3 (the most) down to 0.

---

## Diamond and WordStar

Diamond follows WordStar 4 wherever it can, so if you know WordStar, your fingers already know Diamond. Where they differ:

- **The files are Markdown**, not WordStar's own format. WordStar wrote control codes into the file (^B for bold, ^Y for italics); Diamond writes `**` and `*`, so any program can read its documents.
- **No `<B>` and `<K>` on the screen.** WordStar put a `<B>` in the text when you pressed ^KB. Diamond keeps the text area for your text alone; the block lights up once both ends are marked.
- **Lines wrap by themselves**, so there are no margins or rulers to set (^OL, ^OR, ^OJ, ^OW) and no need for ^B to reformat a paragraph.
- **Only bold and italic.** WordStar's underline (^PS), subscript and superscript (^PV, ^PT) aren't in Markdown, and its strikeout (^PX) only in some versions of it, which Diamond leaves alone.
- **^PB and ^PY work on a block**, and ^P Space returns to plain text; in WordStar they put a code at the cursor.
- **The views are new.** WordStar 4's ^OP turned on a preview of the text as it would print; Diamond's ^OP is a reading view in proportional type, and ^OV its page view. (^OD is WordStar's own, though: there it showed or hid the print codes, as Diamond shows or hides the Markdown marks.)
- **No dot commands** (.HE, .FO, .PA and the rest): headings and the rest come from Markdown.
- **Plain text by the file's name.** WordStar opened program files and plain text in its non-document mode (N at the opening menu); Diamond does the same for any file that doesn't end in `.md`.
- **Fewer file commands in ^K.** ^KR and ^KW are there, but not ^KE, ^KJ, ^KO or ^KF: the list of documents does that work.
- **Esc backs out** of a command that's asking you something ("Find?", "Close without saving?"); in WordStar, ^U did that as well as unerasing.

## The end

That's all of it. Close this with ^KQ (and Y, to leave it unchanged), or keep it: it's your copy.
