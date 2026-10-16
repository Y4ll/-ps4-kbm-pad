#!/usr/bin/env bash
# Resume la estructura de un ELF para compararla desde los avisos de Actions.
f="$1"
[ -f "$f" ] || { echo "no existe $f"; exit 0; }
echo "== $f ($(stat -c%s "$f") bytes)"
echo "-- dinamico"
readelf -dW "$f" | grep -E "NEEDED|SONAME|RELA|JMPREL|INIT|FINI|FLAGS|0x6" | head -30
echo "-- program headers"
readelf -lW "$f" | sed -n '1,25p'
echo "-- secciones"
readelf -SW "$f" | awk 'NR>4{print}' | head -40
echo "-- simbolos dinamicos (Ndx UND = importado)"
readelf --dyn-syms -W "$f" | awk 'NR>3{print $4, $5, $7, $8}' | sort -u | head -80
echo "-- tipos de reubicacion"
readelf -rW "$f" | awk '/R_X86_64/{print $3}' | sort | uniq -c
