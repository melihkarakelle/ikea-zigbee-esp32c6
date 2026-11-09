#!/usr/bin/env python3
"""Embed main/web/svg/*.svg into index.html (const IMG=/*IMG*/{...}/*END*/).
Run after changing an SVG: python3 main/web/embed_svg.py"""
import json, pathlib, re
web = pathlib.Path(__file__).parent
img = {}
for f in sorted((web / "svg").glob("*.svg")):
    key = f.stem[5:] if f.stem.startswith("icon-") else f.stem
    svg = re.sub(r"<\?xml[^>]*>|<!--.*?-->", "", f.read_text(), flags=re.S)
    img[key] = re.sub(r">\s+<", "><", re.sub(r"\s+", " ", svg)).strip()
html = (web / "index.html").read_text()
html, n = re.subn(r"/\*IMG\*/.*?/\*END\*/", lambda m: "/*IMG*/" + json.dumps(img, ensure_ascii=False, separators=(",", ":")) + "/*END*/", html, flags=re.S)
assert n == 1, "IMG marker not found"
(web / "index.html").write_text(html)
print(f"{len(img)} SVGs embedded, index.html {len(html.encode())} bytes")
