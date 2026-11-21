#!/usr/bin/env python3
import os
import sys
import xml.etree.ElementTree as ET

src_dir, out_dir = sys.argv[1], sys.argv[2]
LANGUAGES = {"es": "duckstation-qt_es.ts", "es-ES": "duckstation-qt_es-ES.ts", "pt-BR": "duckstation-qt_pt-BR.ts"}

os.makedirs(out_dir, exist_ok=True)
for code, name in LANGUAGES.items():
    root = ET.parse(os.path.join(src_dir, name)).getroot()
    records = []
    for context in root.findall("context"):
        ctx = context.findtext("name") or ""
        for message in context.findall("message"):
            source = message.findtext("source") or ""
            tr = message.find("translation")
            if tr is None or tr.get("type") in ("unfinished", "vanished", "obsolete"):
                continue
            forms = tr.findall("numerusform")
            text = (forms[0].text if forms else tr.text) or ""
            if not source or not text:
                continue
            fields = (ctx, source, text)
            if any(ch in f for f in fields for ch in "\x1e\x1f"):
                continue
            records.append("\x1f".join(fields))
    with open(os.path.join(out_dir, code + ".tsv"), "w", encoding="utf-8", newline="") as f:
        f.write("\x1e".join(records))
    print("translations: %s %d strings" % (code, len(records)))
