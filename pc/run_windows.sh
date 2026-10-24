#!/bin/bash
# Roda o jogo no WSL e abre a janela NATIVA DO WINDOWS (pc_viewer.exe), ligada por TCP em localhost.
cd "$(dirname "$0")/.." || exit 1
PORT=${PC_NET_PORT:-47474}
needs_build() { [ ! -x build_pc/pokefirered_pc ] || [ ! -x build_pc/pc_viewer ] || [ -n "$(find pc -type f \( -name '*.c' -o -name '*.h' -o -name '*.s' -o -name '*.pl' -o -name '*.sh' \) -newer build_pc/pokefirered_pc 2>/dev/null | head -1)" ]; }
if needs_build; then echo "Fontes em pc/ mudaram: recompilando antes de rodar..."; ./pc/build.sh || exit 1; fi
if [ "$(cat /proc/sys/vm/mmap_min_addr 2>/dev/null)" != "0" ]; then
  echo "Ajustando vm.mmap_min_addr=0 (pede a senha do sudo): o jogo original tem leituras de ponteiro nulo que no GBA são inofensivas."
  sudo sysctl -w vm.mmap_min_addr=0 >/dev/null || echo "Sem sudo: o jogo roda, mas pode cair em bugs de ponteiro nulo do jogo original."
fi
[ -x build_pc/pokefirered_pc ] || { echo "Build do jogo incompleto."; exit 1; }
if [ ! -f build_pc/win/pc_viewer.exe ] || [ pc/viewer.c -nt build_pc/win/pc_viewer.exe ] || [ pc/shared.h -nt build_pc/win/pc_viewer.exe ]; then ./pc/build_windows.sh || exit 1; fi
WINHOME=$(cmd.exe /c "echo %USERPROFILE%" 2>/dev/null | tr -d '\r')
if [ -z "$WINHOME" ]; then
  echo "Não consegui falar com o Windows (interop do WSL desligado?). Copie build_pc/win/ para o Windows, rode"
  echo "  PC_NET_PORT=$PORT ./build_pc/pokefirered_pc   (aqui)  e abra pc_viewer.exe no Windows."; exit 1
fi
DEST="$(wslpath "$WINHOME")/FireRedPC"; mkdir -p "$DEST"
cp -f build_pc/win/pc_viewer.exe build_pc/win/SDL2.dll "$DEST/"
pkill -x pokefirered_pc 2>/dev/null
PC_NET_PORT=$PORT ./build_pc/pokefirered_pc &
GAME=$!
trap 'kill $GAME 2>/dev/null' EXIT INT TERM
sleep 1
( cd "$DEST" && ./pc_viewer.exe "127.0.0.1:$PORT" )
kill $GAME 2>/dev/null; wait $GAME 2>/dev/null; code=$?
[ $code -ne 0 ] && [ $code -ne 143 ] && echo "O jogo terminou com código $code (veja as mensagens acima)."
true
