#!/bin/bash
# Compila o visualizador para WINDOWS (pc_viewer.exe) a partir do WSL, com mingw-w64 e o SDL2 oficial para mingw.
# Pré-requisito:  sudo apt install mingw-w64 curl
cd "$(dirname "$0")/.." || exit 1
command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "Falta o compilador para Windows:  sudo apt install mingw-w64"; exit 1; }
VER=2.30.9
D=build_pc/win; mkdir -p $D
if [ ! -d "$D/SDL2-$VER" ]; then
  echo "Baixando o SDL2 $VER para mingw..."
  curl -L -f -o $D/sdl2-mingw.tar.gz "https://github.com/libsdl-org/SDL/releases/download/release-$VER/SDL2-devel-$VER-mingw.tar.gz" || { echo "Falha no download do SDL2 (sem internet?)"; exit 1; }
  tar -xzf $D/sdl2-mingw.tar.gz -C $D || exit 1
fi
S=$D/SDL2-$VER/x86_64-w64-mingw32
x86_64-w64-mingw32-gcc -O2 -DSDL_MAIN_HANDLED -I$S/include/SDL2 pc/viewer.c -o $D/pc_viewer.exe -L$S/lib -lSDL2 -lws2_32 -static-libgcc -mconsole || { echo "Falha ao compilar o visualizador do Windows"; exit 1; }
cp -f $S/bin/SDL2.dll $D/
echo "OK -> $D/pc_viewer.exe (+ SDL2.dll)"
