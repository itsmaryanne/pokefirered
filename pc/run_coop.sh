#!/bin/bash
# Coop local: sobe host + convidado e abre as 2 janelas do Windows. Uso: ./pc/run_coop.sh
cd "$(dirname "$0")/.." || exit 1
if [ ! -x build_pc/pokefirered_pc ] || [ -n "$(find pc -type f \( -name '*.c' -o -name '*.h' -o -name '*.pl' -o -name '*.sh' \) -not -name run_coop.sh -newer build_pc/pokefirered_pc 2>/dev/null | head -1)" ]; then
  echo "Recompilando..."; ./pc/build.sh || exit 1
fi
[ -x build_pc/pokefirered_pc ] || { echo "Build incompleto."; exit 1; }
[ "$(cat /proc/sys/vm/mmap_min_addr 2>/dev/null)" = "0" ] || sudo sysctl -w vm.mmap_min_addr=0 >/dev/null
WINHOME=$(cmd.exe /c "echo %USERPROFILE%" 2>/dev/null | tr -d '\r')
DEST="$(wslpath "$WINHOME")/FireRedPC"
[ -f "$DEST/pc_viewer.exe" ] || { echo "pc_viewer.exe nao esta em $DEST (rode ./pc/run_windows.sh uma vez)"; exit 1; }
pkill -x pokefirered_pc 2>/dev/null; sleep 0.5
cp -n pokefirered.sav guest.sav
trap 'trap - EXIT INT TERM; kill 0 2>/dev/null' EXIT INT TERM
PC_COOP_HOST=47500 PC_NET_PORT=47474 ./build_pc/pokefirered_pc > ~/host.log 2>&1 &
sleep 1
PC_SAVE=guest.sav PC_COOP_JOIN=127.0.0.1:47500 PC_NET_PORT=47475 ./build_pc/pokefirered_pc > ~/guest.log 2>&1 &
sleep 1
( tail -n0 -F ~/host.log  | grep --line-buffered "coop\|FATAL" | sed -u 's/^/[HOST]  /' ) &
( tail -n0 -F ~/guest.log | grep --line-buffered "coop\|FATAL" | sed -u 's/^/[GUEST] /' ) &
echo "Abrindo as janelas. M muda o som de uma delas. Ctrl+C aqui encerra os dois jogos."
( cd "$DEST" && ./pc_viewer.exe 127.0.0.1:47474 & ( cd "$DEST" && ./pc_viewer.exe 127.0.0.1:47475 ) & wait )
