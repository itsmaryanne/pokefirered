#!/bin/bash
# Autoteste sem janela (driver "dummy" do SDL): roda o jogo ~35 s e confere vídeo, input e áudio.
# Uso (na raiz do repositório, depois de ./pc/build.sh):   ./pc/selftest.sh
cd "$(dirname "$0")/.." || exit 1
[ -x build_pc/pokefirered_pc ] && [ -x build_pc/pc_viewer ] || { echo "Rode ./pc/build.sh primeiro."; exit 1; }
OUT=selftest_out; rm -rf $OUT; mkdir -p $OUT
pkill -x pokefirered_pc 2>/dev/null; rm -f /dev/shm/pokefirered_pc
SAVE=$OUT/selftest.sav
echo "Rodando o jogo (cerca de 40 s)..."
PC_SAVE=$SAVE PC_AUDIO_DUMP=$OUT/audio.pcm ./build_pc/pokefirered_pc > $OUT/game.log 2>&1 &
GAME=$!
sleep 1
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
PC_VIEWER_SCRIPT="100:snap:$OUT/f100.bmp,230:snap:$OUT/f230.bmp,700:snap:$OUT/f700.bmp,1520:down:enter,1526:up:enter,1700:snap:$OUT/f1700.bmp,2100:snap:$OUT/f2100.bmp,2101:quit" \
timeout -s KILL 120 ./build_pc/pc_viewer > $OUT/viewer.log 2>&1
kill $GAME 2>/dev/null; wait $GAME 2>/dev/null
python3 - "$OUT" <<'PY'
import sys, struct
out = sys.argv[1]
fails = 0
def check(ok, msg):
    global fails
    print(("  PASSOU  " if ok else "  FALHOU  ") + msg)
    if not ok: fails += 1
def bmp(path):
    try: d = open(path, 'rb').read()
    except OSError: return None
    off = struct.unpack_from('<I', d, 10)[0]; w, h = struct.unpack_from('<ii', d, 18); bpp = struct.unpack_from('<H', d, 28)[0]
    if bpp != 32: return None
    px = [d[off + i:off + i + 3] for i in range(0, w * abs(h) * 4, 4)]    # BGR
    return w, abs(h), px
def stats(img):
    w, h, px = img; n = len(px)
    black = sum(1 for p in px if max(p) < 24); white = sum(1 for p in px if min(p) > 200)
    return n, black, white, len(set(px))
print("\nVídeo:")
im = bmp(f"{out}/f100.bmp")
if im: n, b, wh, c = stats(im); check(b > 0.6 * n and 300 < wh < 6000, f"quadro 100 parece a tela de copyright (preto {100*b//n}%, texto branco {wh} px)")
else: check(False, "quadro 100 não foi capturado")
im = bmp(f"{out}/f230.bmp")
if im: n, b, wh, c = stats(im); check(c >= 4 and b < 0.7 * n, f"quadro 230 parece a cena do logo (cores distintas: {c})")
else: check(False, "quadro 230 não foi capturado")
im = bmp(f"{out}/f700.bmp")
if im: n, b, wh, c = stats(im); check(c >= 10, f"quadro 700 tem imagem rica (cores distintas: {c})")
else: check(False, "quadro 700 não foi capturado")
print("\nInput (Start apertado no quadro 1520):")
a, b = bmp(f"{out}/f1700.bmp"), bmp(f"{out}/f2100.bmp")
if a and b: check(a[2] != b[2] or stats(a)[3] > 40, f"o jogo continua mudando depois do Start (cores no quadro 1700: {stats(a)[3]}, no 2100: {stats(b)[3]})")
else: check(False, "quadros 1700/2100 não foram capturados")
print("\nÁudio:")
try:
    d = open(f"{out}/audio.pcm", 'rb').read()
    n = len(d) // 4; s = struct.unpack('<%dh' % (n * 2), d[:n * 4])
    secs = n / 32768
    def rms(x0, x1):
        seg = s[x0 * 2:x1 * 2]; return (sum(v * v for v in seg) / max(1, len(seg))) ** 0.5
    loud = max((rms(i * 32768, (i + 1) * 32768) for i in range(int(secs))), default=0)
    check(secs > 10, f"gerou {secs:.1f} s de áudio")
    check(loud > 300, f"há som audível (maior nível RMS por segundo: {loud:.0f})")
except OSError:
    check(False, "nenhum áudio foi gerado")
print("\nVisualizador:")
log = open(f"{out}/viewer.log").read()
check("snapshot" in log, "o visualizador falou com o jogo (memória compartilhada)")
print("\nResultado:", "TUDO OK" if fails == 0 else f"{fails} verificação(ões) falharam (veja {out}/game.log e {out}/viewer.log)")
sys.exit(1 if fails else 0)
PY
