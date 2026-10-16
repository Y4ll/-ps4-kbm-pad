#!/usr/bin/env python3
# Asegura que pkg.gp4 declare las carpetas de todos los archivos (si falta una, PkgTool falla con
# "Sequence contains no elements"). Homebrew Store declara <dir targ_name="Media" /> a mano.
import re, sys

path = sys.argv[1]
txt = open(path, encoding="utf-8").read()
files = re.findall(r'<file[^>]*targ_path="([^"]+)"', txt)
tops = []
for f in files:
    f = f.replace("\\", "/")
    if "/" in f:
        d = f.split("/")[0]
        if d not in tops:
            tops.append(d)

m = re.search(r"<rootdir\s*/>", txt)
if m:
    txt = txt.replace(m.group(0), "<rootdir>\n  </rootdir>", 1)
if "<rootdir" not in txt:
    txt = txt.replace("</psproject>", "  <rootdir>\n  </rootdir>\n</psproject>", 1)

added = []
for d in tops:
    if not re.search(r'<dir\s+targ_name="%s"' % re.escape(d), txt):
        txt = txt.replace("</rootdir>", '    <dir targ_name="%s" />\n  </rootdir>' % d, 1)
        added.append(d)
open(path, "w", encoding="utf-8").write(txt)
print("fixgp4: carpetas anadidas a pkg.gp4:", ", ".join(added) if added else "ninguna")
