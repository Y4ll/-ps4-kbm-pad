#!/usr/bin/env python3
# Genera app/src/ini_default.h a partir de k_ini_default de src/main.c (plantilla de kbm_pad.ini).
import re, sys
src = open("src/main.c", encoding="utf-8").read()
m = re.search(r"static const char k_ini_default\[\] =\n(.*?);\n", src, re.S)
body = m.group(1)
out = "// Generado por app/gen_ini_default.py desde src/main.c (no editar a mano).\n#pragma once\nstatic const char KBM_INI_DEFAULT[] =\n" + body + ";\n"
open("app/src/ini_default.h", "w", encoding="utf-8").write(out)
print("ini_default.h:", len(body.splitlines()), "lineas")
