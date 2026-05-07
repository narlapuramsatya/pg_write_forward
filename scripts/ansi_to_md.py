#!/usr/bin/env python3
"""ansi_to_md.py -- convert an ANSI-colored transcript to a Markdown file
with inline HTML colors so it renders nicely on GitHub.

Usage: ansi_to_md.py INPUT.txt OUTPUT.md TITLE
"""
import re
import sys
import html

ANSI_RE = re.compile(r"\x1b\[([0-9;]*)m")

# 30-37 / 90-97 foreground; 40-47 / 100-107 background; 1=bold, 2=dim
FG = {
    30: "#000000", 31: "#cd3131", 32: "#0dbc79", 33: "#e5e510",
    34: "#2472c8", 35: "#bc3fbc", 36: "#11a8cd", 37: "#e5e5e5",
    90: "#666666", 91: "#f14c4c", 92: "#23d18b", 93: "#f5f543",
    94: "#3b8eea", 95: "#d670d6", 96: "#29b8db", 97: "#ffffff",
}
BG = {
    40: "#000000", 41: "#cd3131", 42: "#0dbc79", 43: "#e5e510",
    44: "#2472c8", 45: "#bc3fbc", 46: "#11a8cd", 47: "#e5e5e5",
    100: "#666666", 101: "#f14c4c", 102: "#23d18b", 103: "#f5f543",
    104: "#3b8eea", 105: "#d670d6", 106: "#29b8db", 107: "#ffffff",
}


def convert(text: str) -> str:
    out = []
    state = {"fg": None, "bg": None, "bold": False, "dim": False}
    open_span = False

    def close():
        nonlocal open_span
        if open_span:
            out.append("</span>")
            open_span = False

    def open():
        nonlocal open_span
        styles = []
        if state["fg"]:
            styles.append(f"color:{state['fg']}")
        if state["bg"]:
            styles.append(f"background-color:{state['bg']}")
        if state["bold"]:
            styles.append("font-weight:bold")
        if state["dim"]:
            styles.append("opacity:0.65")
        if styles:
            out.append(f'<span style="{";".join(styles)}">')
            open_span = True

    pos = 0
    for m in ANSI_RE.finditer(text):
        out.append(html.escape(text[pos:m.start()]))
        codes = m.group(1)
        params = [int(p) for p in codes.split(";") if p != ""] or [0]
        close()
        for p in params:
            if p == 0:
                state.update(fg=None, bg=None, bold=False, dim=False)
            elif p == 1:
                state["bold"] = True
            elif p == 2:
                state["dim"] = True
            elif p in FG:
                state["fg"] = FG[p]
            elif p in BG:
                state["bg"] = BG[p]
        open()
        pos = m.end()
    out.append(html.escape(text[pos:]))
    close()
    return "".join(out)


def main():
    src, dst, title = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(src, "r", encoding="utf-8", errors="replace") as f:
        body = convert(f.read())
    md = f"""# {title}

> Captured transcript of `scripts/pwf_demo_themed.sh`.
> Colors are preserved via inline HTML — GitHub renders this correctly.
> Re-generate with: `scripts/pwf_demo_themed.sh --no-pause --tee scripts/pwf_demo_themed_output.txt && scripts/ansi_to_md.py scripts/pwf_demo_themed_output.txt scripts/pwf_demo_themed_output.md "{title}"`

<pre style="background-color:#1e1e1e;color:#cccccc;padding:14px;border-radius:6px;font-family:'SFMono-Regular',Consolas,'Liberation Mono',Menlo,monospace;font-size:12.5px;line-height:1.45;overflow-x:auto;">
{body}</pre>
"""
    with open(dst, "w", encoding="utf-8") as f:
        f.write(md)
    print(f"wrote {dst} ({len(md):,} bytes)")


if __name__ == "__main__":
    main()
