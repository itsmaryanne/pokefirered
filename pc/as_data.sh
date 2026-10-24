#!/bin/bash
# Monta um arquivo de dados .s do jogo com o assembler x86 (32 bits)
set -o pipefail
f="$1"; b=$(basename "$f" .s); out="${2:-build_pc/data/$b.o}"
tools/preproc/preproc "$f" charmap.txt \
 | gcc -E -std=gnu17 -x assembler-with-cpp -iquote include -I include - 2>/dev/null \
 | perl pc/inline_inc.pl \
 | tools/preproc/preproc -ie "$f" charmap.txt \
 | perl -pe 'unless(/"/){ s/\(([^()]*)\)/"(".($1=~s!\s+!!gr).")"/ge; s/\)\s*([-+*\/|&])\s*/)$1/g; s/([-+*\/|&])\s*\(/$1(/g }' \
 | sed -E 's/\.word\b/.4byte/g; s/\.hword\b/.2byte/g; s/\.align\b/.p2align/g' \
 | as --32 --defsym FIRERED=1 --defsym REVISION=0 --defsym ENGLISH=1 --defsym MODERN=1 -o "$out" -
