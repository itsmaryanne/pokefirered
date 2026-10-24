#!/bin/bash
# Roda o jogo + o visualizador (janela do WSLg). Na raiz do repositório:  ./pc/run.sh
# Para a janela nativa do Windows (som melhor), use ./pc/run_windows.sh
cd "$(dirname "$0")/.." || exit 1
needs_build() { [ ! -x build_pc/pokefirered_pc ] || [ ! -x build_pc/pc_viewer ] || [ -n "$(find pc -type f \( -name '*.c' -o -name '*.h' -o -name '*.s' -o -name '*.pl' -o -name '*.sh' \) -newer build_pc/pokefirered_pc 2>/dev/null | head -1)" ]; }
if needs_build; then echo "Fontes em pc/ mudaram: recompilando antes de rodar..."; ./pc/build.sh || exit 1; fi
if [ "$(cat /proc/sys/vm/mmap_min_addr 2>/dev/null)" != "0" ]; then
  echo "Ajustando vm.mmap_min_addr=0 (pede a senha do sudo): o jogo original tem leituras de ponteiro nulo que no GBA são inofensivas."
  sudo sysctl -w vm.mmap_min_addr=0 >/dev/null || echo "Sem sudo: o jogo roda, mas pode cair em bugs de ponteiro nulo do jogo original."
fi
[ -x build_pc/pokefirered_pc ] && [ -x build_pc/pc_viewer ] || { echo "Build incompleto; veja as mensagens acima."; exit 1; }
pkill -x pokefirered_pc 2>/dev/null   # restos de execuções antigas (segurança)
./build_pc/pokefirered_pc &
GAME=$!
trap 'kill $GAME 2>/dev/null' EXIT INT TERM
./build_pc/pc_viewer
kill $GAME 2>/dev/null; wait $GAME 2>/dev/null; code=$?
[ $code -ne 0 ] && [ $code -ne 143 ] && echo "O jogo terminou com código $code (veja as mensagens acima)."
true
