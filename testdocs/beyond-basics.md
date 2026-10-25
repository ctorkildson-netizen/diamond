# Beyond the basics

Markdown beyond the basics: what Diamond does with fenced code and tables, and the things it leaves alone.

## Fenced code

The stars in here are not bold, and nothing is hidden:

```
x = a**b**c
*p = 0;
_private_name = 1
```

After the fence, **this is bold** again.

## A picture

![a photo](photo.jpg) is shown as typed, not as a link.

## A table

| Name | Size |
|------|------|
| welcome.md | 6 KB |
| novel.md | 300 KB |

The rows above aren't lined up as typed. On the editing screen they stay as typed; in the reading view the columns are lined up, in the typewriter face.

## Not supported: shown as typed

Diamond doesn't format any of these, so each should look exactly as typed, on the editing screen and in the reading view: ~~strikethrough~~, <b>HTML</b>, https://example.com (not made a link), and a task list:

- [ ] not done
- [x] done
