// Renderizador de vídeo por software do GBA: lê registradores, paleta, VRAM e OAM nos endereços do GBA
// e produz um quadro 240x160 em ARGB8888. Cobre modos 0/1/2 (fundos de texto e afins), sprites
// (normais e afins), janelas (WIN0/WIN1/OBJ), alpha e brilho. Não cobre ainda: mosaico, modos bitmap 3/4/5.
#include <stdint.h>
#include <string.h>
#define W 240
#define H 160
#define IO16(o) (*(volatile uint16_t *)(0x04000000 + (o)))
#define IO32(o) (*(volatile uint32_t *)(0x04000000 + (o)))
static const uint16_t *const PLTT = (const uint16_t *)0x05000000;
static const uint8_t  *const VRAM = (const uint8_t *)0x06000000;
static const uint16_t *const OAM  = (const uint16_t *)0x07000000;

enum { L_BG0, L_BG1, L_BG2, L_BG3, L_OBJ, L_BD };
typedef struct { uint16_t c; uint8_t layer, prio, semi; } Px;   // c: cor BGR555; layer=255 => transparente

static uint32_t Rgb(uint16_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
}

static int TilePixel(const uint8_t *tile, int x, int y, int is8, int hflip, int vflip) {
    if (hflip) x = 7 - x;
    if (vflip) y = 7 - y;
    if (is8) return tile[y * 8 + x];
    uint8_t b = tile[y * 4 + (x >> 1)];
    return (x & 1) ? (b >> 4) : (b & 15);
}

// ---- Fundo de texto: preenche a linha (palavras de cor; 0xFFFF = transparente) ----
static void TextBgLine(int bg, int y, uint16_t *out) {
    uint16_t cnt = IO16(0x08 + bg * 2);
    int charBase = ((cnt >> 2) & 3) * 0x4000, scrBase = ((cnt >> 8) & 31) * 0x800, is8 = (cnt >> 7) & 1, size = cnt >> 14;
    int hofs = IO16(0x10 + bg * 4) & 0x1FF, vofs = IO16(0x12 + bg * 4) & 0x1FF;
    int wmask = (size & 1) ? 511 : 255, hmask = (size & 2) ? 511 : 255;
    int sy = (y + vofs) & hmask;
    for (int x = 0; x < W; x++) {
        int sx = (x + hofs) & wmask;
        int blk = 0; if (sx >= 256) blk += 1; if (sy >= 256) blk += (size == 3) ? 2 : 1;
        const uint16_t *map = (const uint16_t *)(VRAM + scrBase + blk * 0x800);
        uint16_t e = map[((sy & 255) >> 3) * 32 + ((sx & 255) >> 3)];
        int tile = e & 0x3FF, hf = (e >> 10) & 1, vf = (e >> 11) & 1, pal = e >> 12;
        const uint8_t *td = VRAM + charBase + tile * (is8 ? 64 : 32);
        if (charBase + tile * (is8 ? 64 : 32) + (is8 ? 64 : 32) > 0x10000) { out[x] = 0xFFFF; continue; }   // tiles de fundo só existem nos primeiros 64 KB
        int idx = TilePixel(td, sx & 7, sy & 7, is8, hf, vf);
        out[x] = idx ? (is8 ? PLTT[idx] : PLTT[pal * 16 + idx]) : 0xFFFF;
    }
}

// ---- Fundo afim (BG2/BG3 nos modos 1/2); px,py = ponto de referência da linha (8 bits de fração) ----
static int32_t gRefX[2], gRefY[2], gPrevX[2], gPrevY[2], gBlkX[2], gBlkY[2];
static int32_t Sign28(uint32_t v) { return (int32_t)(v << 4) >> 4; }
static void AffineBgLine(int bg, int32_t px, int32_t py, uint16_t *out) {
    uint16_t cnt = IO16(0x08 + bg * 2);
    int base = (bg == 2) ? 0x20 : 0x30;
    int charBase = ((cnt >> 2) & 3) * 0x4000, scrBase = ((cnt >> 8) & 31) * 0x800, wrap = (cnt >> 13) & 1, size = 128 << (cnt >> 14);
    int16_t pa = (int16_t)IO16(base), pc = (int16_t)IO16(base + 4);
    for (int x = 0; x < W; x++, px += pa, py += pc) {
        int tx = px >> 8, ty = py >> 8;
        if (wrap) { tx &= size - 1; ty &= size - 1; } else if (tx < 0 || ty < 0 || tx >= size || ty >= size) { out[x] = 0xFFFF; continue; }
        int tile = VRAM[scrBase + (ty >> 3) * (size >> 3) + (tx >> 3)];
        int idx = VRAM[charBase + tile * 64 + (ty & 7) * 8 + (tx & 7)];
        out[x] = idx ? PLTT[idx] : 0xFFFF;
    }
}
// A cada linha o hardware soma PB/PD à referência interna (e escrever BG2X/Y recarrega a referência).
static void AdvanceAffineRefs(void) {
    for (int i = 0; i < 2; i++) { int base = i == 0 ? 0x20 : 0x30; gRefX[i] += (int16_t)IO16(base + 2); gRefY[i] += (int16_t)IO16(base + 6); }
}
void PcHBlankDma(void);

// ---- Sprites: preenche buffer da linha + máscara da janela de OBJ ----
static const uint8_t kObjW[4][4] = {{8,16,32,64},{16,32,32,64},{8,8,16,32},{0,0,0,0}};
static const uint8_t kObjH[4][4] = {{8,16,32,64},{8,8,16,32},{16,32,32,64},{0,0,0,0}};
static void ObjLine(int y, Px *line, uint8_t *objwin, int dispcnt, int mosOH, int mosOV) {
    int map1d = (dispcnt >> 6) & 1, mode = dispcnt & 7;
    const uint8_t *objVram = VRAM + 0x10000;
    for (int i = 0; i < 128; i++) {
        uint16_t a0 = OAM[i * 4], a1 = OAM[i * 4 + 1], a2 = OAM[i * 4 + 2];
        int om = (a0 >> 8) & 3; if (om == 2) continue;
        int affine = om & 1, dbl = om == 3, gm = (a0 >> 10) & 3, is8 = (a0 >> 13) & 1, shape = a0 >> 14, sz = a1 >> 14;
        if (gm == 3 || shape == 3) continue;
        int w = kObjW[shape][sz], h = kObjH[shape][sz], bw = dbl ? w * 2 : w, bh = dbl ? h * 2 : h;
        int oy = a0 & 255, ox = a1 & 511; if (ox >= 256) ox -= 512;
        int ry = (y - oy) & 255; if (ry >= bh) continue;
        int mosy = (a0 >> 12) & 1; if (mosy && mosOV > 1) { ry -= y % mosOV; if (ry < 0) ry = 0; }
        int tile = a2 & 0x3FF, prio = (a2 >> 10) & 3, pal = a2 >> 12;
        if (mode >= 3 && tile < 512) continue;
        int16_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) { int g = (a1 >> 9) & 31; pa = OAM[g*16+3]; pb = OAM[g*16+7]; pc = OAM[g*16+11]; pd = OAM[g*16+15]; }
        for (int dx = 0; dx < bw; dx++) {
            int sx = ox + dx; if (sx < 0 || sx >= W) continue;
            int dxe = dx; if (mosy && mosOH > 1) { dxe = dx - (sx % mosOH); if (dxe < 0) dxe = 0; }
            int tx, ty;
            if (affine) {
                int cx = dxe - bw / 2, cy = ry - bh / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2; ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = dxe; ty = ry; if (a1 & 0x1000) tx = w - 1 - tx; if (a1 & 0x2000) ty = h - 1 - ty;
            }
            int tcx = tx >> 3, tcy = ty >> 3;
            int tn = map1d ? tile + (tcy * (w >> 3) + tcx) * (is8 ? 2 : 1) : tile + tcy * 32 + tcx * (is8 ? 2 : 1);
            const uint8_t *td = objVram + (tn & 0x3FF) * 32;
            if (td + 64 > VRAM + 0x18000) continue;
            int idx = TilePixel(td, tx & 7, ty & 7, is8, 0, 0);
            if (!idx) continue;
            if (gm == 2) { objwin[sx] = 1; continue; }
            if (line[sx].layer == 255 || prio < line[sx].prio) {
                line[sx].c = is8 ? PLTT[256 + idx] : PLTT[256 + pal * 16 + idx];
                line[sx].layer = L_OBJ; line[sx].prio = prio; line[sx].semi = (gm == 1);
            }
        }
    }
}

static uint16_t Blend(uint16_t a, uint16_t b, int eva, int evb) {
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4, g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4, bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return (uint16_t)(r | (g << 5) | (bl << 10));
}
static uint16_t Fade(uint16_t a, int y, int toWhite) {
    int r = a & 31, g = (a >> 5) & 31, b = (a >> 10) & 31;
    if (toWhite) { r += ((31 - r) * y) >> 4; g += ((31 - g) * y) >> 4; b += ((31 - b) * y) >> 4; }
    else { r -= (r * y) >> 4; g -= (g * y) >> 4; b -= (b * y) >> 4; }
    return (uint16_t)(r | (g << 5) | (b << 10));
}

void PcRenderFrame(uint32_t *out) {
    for (int i = 0; i < 2; i++) { int base = i == 0 ? 0x20 : 0x30; gRefX[i] = gPrevX[i] = Sign28(IO32(base + 8)); gRefY[i] = gPrevY[i] = Sign28(IO32(base + 12)); }
    for (int y = 0; y < H; y++) {
        if (y >= 1) PcHBlankDma();                                  // DMAs de H-blank (efeitos linha a linha) rodam no fim da linha anterior
        uint16_t dispcnt = IO16(0x00);                              // os registradores são lidos a CADA linha: o jogo os muda no meio do quadro
        int mode = dispcnt & 7;
        uint16_t bldcnt = IO16(0x50), bldalpha = IO16(0x52); int bldy = IO16(0x54) & 31; if (bldy > 16) bldy = 16;
        int eva = bldalpha & 31, evb = (bldalpha >> 8) & 31; if (eva > 16) eva = 16; if (evb > 16) evb = 16;
        int bmode = (bldcnt >> 6) & 3;
        uint16_t mosaic = IO16(0x4C);
        int mosBH = (mosaic & 15) + 1, mosBV = ((mosaic >> 4) & 15) + 1, mosOH = ((mosaic >> 8) & 15) + 1, mosOV = ((mosaic >> 12) & 15) + 1;
        if (dispcnt & 0x80) { for (int x = 0; x < W; x++) out[y * W + x] = 0xFFFFFFFFu; AdvanceAffineRefs(); continue; }   // forced blank
        uint16_t bg[4][W]; int bgOn[4] = {0,0,0,0};
        static const int textMask[3][4] = {{1,1,1,1},{1,1,0,0},{0,0,0,0}};
        for (int b = 0; b < 4; b++) {
            if (!(dispcnt & (0x100 << b))) continue;
            uint16_t cnt = IO16(0x08 + b * 2); int mos = (cnt >> 6) & 1;
            int ye = (mos && mosBV > 1) ? y - (y % mosBV) : y;      // mosaico vertical: repete a primeira linha do bloco
            if (mode <= 2 && textMask[mode][b]) { TextBgLine(b, ye, bg[b]); bgOn[b] = 1; }
            else if ((mode == 1 && b == 2) || (mode == 2 && (b == 2 || b == 3))) {
                int i = b - 2, base = (b == 2) ? 0x20 : 0x30;
                int32_t rx = Sign28(IO32(base + 8)), ry = Sign28(IO32(base + 12));
                if (rx != gPrevX[i]) { gRefX[i] = rx; gPrevX[i] = rx; }
                if (ry != gPrevY[i]) { gRefY[i] = ry; gPrevY[i] = ry; }
                int32_t ux = gRefX[i], uy = gRefY[i];
                if (mos && mosBV > 1) { if (y % mosBV == 0) { gBlkX[i] = ux; gBlkY[i] = uy; } ux = gBlkX[i]; uy = gBlkY[i]; }
                AffineBgLine(b, ux, uy, bg[b]); bgOn[b] = 1;
            } else continue;
            if (mos && mosBH > 1) { uint16_t tmp[W]; for (int x = 0; x < W; x++) tmp[x] = bg[b][x - (x % mosBH)]; memcpy(bg[b], tmp, sizeof tmp); }   // mosaico horizontal
        }
        Px obj[W]; uint8_t objwin[W]; memset(objwin, 0, sizeof objwin);
        for (int x = 0; x < W; x++) obj[x].layer = 255;
        if (dispcnt & 0x1000) ObjLine(y, obj, objwin, dispcnt, mosOH, mosOV);
        int win0 = (dispcnt >> 13) & 1, win1 = (dispcnt >> 14) & 1, winobj = (dispcnt >> 15) & 1, anywin = win0 | win1 | winobj;
        uint16_t w0h = IO16(0x40), w1h = IO16(0x42), w0v = IO16(0x44), w1v = IO16(0x46), winin = IO16(0x48), winout = IO16(0x4A);
        for (int x = 0; x < W; x++) {
            int mask = 0x3F;                                        // bits 0-3 BGs, 4 OBJ, 5 efeitos
            if (anywin) {
                mask = winout & 0x3F;
                if (winobj && objwin[x]) mask = (winout >> 8) & 0x3F;
                if (win1) { int x1 = w1h >> 8, x2 = w1h & 255, y1 = w1v >> 8, y2 = w1v & 255; if (x2 > W || x1 > x2) x2 = W; if (y2 > H || y1 > y2) y2 = H;
                    if (x >= x1 && x < x2 && y >= y1 && y < y2) mask = (winin >> 8) & 0x3F; }
                if (win0) { int x1 = w0h >> 8, x2 = w0h & 255, y1 = w0v >> 8, y2 = w0v & 255; if (x2 > W || x1 > x2) x2 = W; if (y2 > H || y1 > y2) y2 = H;
                    if (x >= x1 && x < x2 && y >= y1 && y < y2) mask = winin & 0x3F; }
            }
            uint16_t topC = PLTT[0], secC = PLTT[0]; int topL = L_BD, secL = L_BD, topSemi = 0, got = 0;
            for (int prio = 0; prio < 4 && got < 2; prio++) {
                int cand[5], n = 0;
                if ((mask & 0x10) && obj[x].layer != 255 && obj[x].prio == prio) cand[n++] = L_OBJ;
                for (int b = 0; b < 4; b++) if (bgOn[b] && (mask & (1 << b)) && (IO16(0x08 + b * 2) & 3) == prio && bg[b][x] != 0xFFFF) cand[n++] = b;
                for (int k = 0; k < n && got < 2; k++) {
                    uint16_t c = cand[k] == L_OBJ ? obj[x].c : bg[cand[k]][x];
                    if (got == 0) { topC = c; topL = cand[k]; topSemi = cand[k] == L_OBJ && obj[x].semi; secL = L_BD; secC = PLTT[0]; }
                    else { secC = c; secL = cand[k]; }
                    got++;
                }
            }
            uint16_t c = topC;
            int fx = mask & 0x20, first = (bldcnt >> topL) & 1, second = (bldcnt >> (8 + secL)) & 1;
            if (topSemi && second && (mask & 0x20)) c = Blend(topC, secC, eva, evb);
            else if (fx && first) {
                if (bmode == 1 && second) c = Blend(topC, secC, eva, evb);
                else if (bmode == 2) c = Fade(topC, bldy, 1);
                else if (bmode == 3) c = Fade(topC, bldy, 0);
            }
            out[y * W + x] = Rgb(c);
        }
        AdvanceAffineRefs();
    }
}
