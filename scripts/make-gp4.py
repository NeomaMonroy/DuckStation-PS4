#!/usr/bin/env python3
import os
import sys
import time
from xml.sax.saxutils import escape, quoteattr

def main():
    stage, content_id, out = sys.argv[1], sys.argv[2], sys.argv[3]
    files, dirs = [], set()
    for root, subdirs, names in os.walk(stage):
        subdirs.sort()
        rel_root = os.path.relpath(root, stage).replace(os.sep, "/")
        if rel_root != ".":
            dirs.add(rel_root)
        for n in sorted(names):
            rel = n if rel_root == "." else rel_root + "/" + n
            if rel == os.path.basename(out) and rel_root == ".":
                continue
            files.append(rel)
    children = {}
    for d in dirs:
        parent = d.rsplit("/", 1)[0] if "/" in d else ""
        children.setdefault(parent, []).append(d)

    def tree(parent, indent):
        lines = []
        for d in sorted(children.get(parent, [])):
            name = quoteattr(d.rsplit("/", 1)[-1])
            sub = tree(d, indent + "\t")
            if sub:
                lines.append("%s<dir targ_name=%s>" % (indent, name))
                lines.extend(sub)
                lines.append("%s</dir>" % indent)
            else:
                lines.append("%s<dir targ_name=%s />" % (indent, name))
        return lines

    file_tags = "\n".join('\t\t<file targ_path=%s orig_path=%s />' % (quoteattr(f), quoteattr(f)) for f in files)
    rootdir = "\n".join(["\t<rootdir>"] + tree("", "\t\t") + ["\t</rootdir>"])
    text = ('<?xml version="1.0"?>\n'
            '<psproject xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" fmt="gp4" version="1000">\n'
            '\t<volume>\n'
            '\t\t<volume_type>pkg_ps4_app</volume_type>\n'
            '\t\t<volume_id>PS4VOLUME</volume_id>\n'
            '\t\t<volume_ts>%s</volume_ts>\n'
            '\t\t<package content_id="%s" passcode="00000000000000000000000000000000"\n'
            '\t\t\tstorage_type="digital50" app_type="full" />\n'
            '\t\t<chunk_info chunk_count="1" scenario_count="1">\n'
            '\t\t\t<chunks>\n'
            '\t\t\t\t<chunk id="0" layer_no="0" label="Chunk #0" />\n'
            '\t\t\t</chunks>\n'
            '\t\t\t<scenarios default_id="0">\n'
            '\t\t\t\t<scenario id="0" type="sp" initial_chunk_count="1" label="Scenario #0">0</scenario>\n'
            '\t\t\t</scenarios>\n'
            '\t\t</chunk_info>\n'
            '\t</volume>\n'
            '\t<files img_no="0">\n'
            '%s\n'
            '\t</files>\n'
            '%s\n'
            '</psproject>\n') % (time.strftime("%Y-%m-%d %H:%M:%S"), escape(content_id), file_tags, rootdir)
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)
    print("pkg.gp4: %d files, %d folders" % (len(files), len(dirs)))
    return 0

if __name__ == "__main__":
    sys.exit(main())
