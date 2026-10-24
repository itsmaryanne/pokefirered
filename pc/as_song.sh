#!/bin/bash
# Monta um .s de música (sound/songs/midi/*.s) com o assembler x86.
# O Makefile original monta esses arquivos direto com `-I sound` (sem preproc); aqui expandimos os
# `.include` e removemos os comentários `@` (estilo ARM), que o assembler x86 não entende.
set -o pipefail
f="$1"; out="$2"
sed 's/@.*$//' "$f" | perl pc/inline_inc.pl | sed 's/@.*$//' \
 | sed -E 's/\.word\b/.4byte/g; s/\.hword\b/.2byte/g; s/\.align\b/.p2align/g' \
 | as --32 -I sound --defsym FIRERED=1 --defsym REVISION=0 --defsym ENGLISH=1 --defsym MODERN=1 -o "$out" -
