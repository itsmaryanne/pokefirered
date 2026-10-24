#!/bin/bash
# Build experimental do pokefirered para PC (x86 32 bits).  Rode da RAIZ do repositório:  ./pc/build.sh
# Pré-requisito: já ter rodado `make` uma vez (gera tools/, gráficos convertidos e os .s das músicas).
set -u
cd "$(dirname "$0")/.." || exit 1
[ -x tools/preproc/preproc ] || { echo "Rode 'make' primeiro (precisa de tools/preproc)."; exit 1; }
echo 'int main(){return 0;}' | gcc -m32 -x c - -o /tmp/_m32test 2>/dev/null || { echo "Falta gcc-multilib:  sudo apt install gcc-multilib"; exit 1; }
rm -rf build_pc; mkdir -p build_pc/log build_pc/data build_pc/sound
# Cópia de include/gba/macro.h com o DmaSet redirecionado para o DMA simulado (o original não é alterado).
perl -0pe 's/#define DmaSet\(dmaNum, src, dest, control\)\s*\\\n\{.*?\n\}\n/extern void PcDmaSet(int, unsigned, unsigned, unsigned);\n#define DmaSet(dmaNum, src, dest, control) PcDmaSet(dmaNum, (unsigned)(src), (unsigned)(dest), (unsigned)(control))\n/s' include/gba/macro.h > build_pc/gba_macro_pc.h
grep -q PcDmaSet build_pc/gba_macro_pc.h || { echo "Falha ao gerar build_pc/gba_macro_pc.h"; exit 1; }
NP=$(nproc)

echo "[1/4] Compilando C (src/*.c) em 32 bits..."
# Arquivos substituídos por versões PC (em pc/): camada de flash/save em hardware do cartucho
ls src/*.c | grep -v -E "src/agb_flash" | xargs -P"$NP" -n1 pc/cc_one.sh > build_pc/result.txt
echo "   OK: $(grep -c '^OK' build_pc/result.txt)   FALHAS: $(grep -c '^FAIL' build_pc/result.txt)"
grep '^FAIL' build_pc/result.txt | sed 's/^/   /'

echo "[2/4] Montando dados (data/*.s) e músicas (sound/songs/midi/*.s) com o assembler x86..."
ok=0; bad=0
for f in data/*.s; do b=$(basename "$f" .s)
  if pc/as_data.sh "$f" 2> "build_pc/data/$b.log"; then ok=$((ok+1)); else bad=$((bad+1)); echo "   FALHOU: $f"; fi
done
echo "   data: OK $ok, FALHAS $bad"
ls sound/songs/midi/*.s 2>/dev/null | xargs -P"$NP" -I{} sh -c 'b=$(basename {} .s); pc/as_song.sh {} build_pc/sound/$b.o 2> build_pc/sound/$b.log && echo OK || echo "FAIL {}"' > build_pc/sound_result.txt
echo "   músicas: OK $(grep -c '^OK' build_pc/sound_result.txt), FALHAS $(grep -c '^FAIL' build_pc/sound_result.txt)"
grep '^FAIL' build_pc/sound_result.txt | head -5 | sed 's/^/   /'
as --32 -o build_pc/pc_syms.o pc/syms.s

echo "[3/4] Compilando a camada PC (pc/*.c)..."
for f in bios pc_main intr sound_stub ppu_timing m4a_real flash_file dma ppu m4a_engine audio frame_sync crash net_bridge coop coop_net; do gcc -m32 -std=gnu17 -O1 -Wall -iquote include -DFIRERED -DREVISION=0 -DENGLISH -DMODERN=1 -DPC_PORT -c -o build_pc/pc_$f.o pc/$f.c 2> build_pc/log/pc_$f.log || echo "   FALHOU pc/$f.c (veja build_pc/log/pc_$f.log)"; done

echo "[4/4] Linkando..."
gcc -m32 -no-pie -rdynamic -o build_pc/pokefirered_pc build_pc/*.o build_pc/data/*.o build_pc/sound/*.o -lm -lpthread -lrt 2> build_pc/link.log
if [ -x build_pc/pokefirered_pc ]; then echo "   LINK OK -> build_pc/pokefirered_pc"; else
  grep -o "undefined reference to \`[^']*'" build_pc/link.log | sed "s/.*\`//;s/'//" | sort -u > build_pc/undef.txt
  echo "   Símbolos ainda indefinidos: $(wc -l < build_pc/undef.txt)  (lista em build_pc/undef.txt)"
  grep -v "undefined reference" build_pc/link.log | head -5
fi

echo "[5/5] Visualizador SDL2 (64 bits, janela + teclado/controle)..."
if pkg-config --exists sdl2 2>/dev/null; then
  gcc -O2 -o build_pc/pc_viewer pc/viewer.c $(pkg-config --cflags --libs sdl2) -lrt 2> build_pc/log/viewer.log && echo "   OK -> build_pc/pc_viewer" || echo "   FALHOU (veja build_pc/log/viewer.log)"
else
  echo "   SDL2 não encontrado. Instale com:  sudo apt install libsdl2-dev pkg-config   e rode ./pc/build.sh de novo"
fi
