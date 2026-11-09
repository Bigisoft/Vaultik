"""Collects the user-visible texts of the frontend for translation.

Finds every tr("...") and QObject::tr("...") in src/gui (adjacent string literals joined, C escapes
decoded) and updates translations/<lang>.json: new texts are added with an empty translation,
texts no longer in the code are dropped, existing translations stay. Usage:
    python tools/extract_strings.py bg
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LITERAL = r'"((?:[^"\\\n]|\\.)*)"'
CALL = re.compile(r'\btr\(\s*((?:' + LITERAL + r'\s*)+)[,)]')
PIECE = re.compile(LITERAL)

# Texts Qt itself shows (dialog buttons, context menus), translated through the same file.
QT_TEXTS = ["OK", "Cancel", "Close", "Yes", "No", "Apply", "Save", "&Yes", "&No", "&OK", "&Cancel", "&Close",
            "Copy", "&Copy", "Select All", "&Undo", "&Redo", "Cu&t", "&Paste", "Delete", "Show Details...",
            "Hide Details..."]


def decode(text: str) -> str:
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text):
            n = text[i + 1]
            out.append({"n": "\n", "t": "\t", "\\": "\\", '"': '"', "'": "'"}.get(n, n))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def texts() -> list[str]:
    found = set(QT_TEXTS)
    for path in sorted((ROOT / "src" / "gui").glob("*.cpp")) + sorted((ROOT / "src" / "gui").glob("*.h")):
        source = path.read_text(encoding="utf-8")
        for m in CALL.finditer(source):
            found.add("".join(decode(p) for p in PIECE.findall(m.group(1))))
    return sorted(found)


def main() -> None:
    lang = sys.argv[1] if len(sys.argv) > 1 else "bg"
    target = ROOT / "translations" / f"{lang}.json"
    old = json.loads(target.read_text(encoding="utf-8")) if target.exists() else {}
    new = {t: old.get(t, "") for t in texts()}
    target.parent.mkdir(exist_ok=True)
    target.write_text(json.dumps(new, ensure_ascii=False, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    missing = sum(1 for v in new.values() if not v)
    print(f"{target.name}: {len(new)} texts, {missing} without a translation")


if __name__ == "__main__":
    main()
