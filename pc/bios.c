// BIOS do GBA reimplementada em C para o port PC (funções chamadas via SWI no hardware real).
// Tipos simples de propósito: não depende dos headers do jogo, só da ABI das funções.
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <stdarg.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef int16_t s16; typedef int32_t s32;

/* ---------- Matemática ---------- */
s32 Div(s32 num, s32 denom) {
    if (denom == 0) return 0;                          // no hardware trava; aqui só evitamos o crash
    if (num == INT32_MIN && denom == -1) return INT32_MIN;
    return num / denom;
}

u16 Sqrt(u32 num) {                                    // raiz quadrada inteira (piso)
    uint64_t r = (uint64_t)sqrt((double)num);
    while (r * r > num) r--;
    while ((r + 1) * (r + 1) <= num) r++;
    return (u16)r;
}

static s32 ArcTanPoly(s32 i) {                         // polinômio usado pela BIOS (ArcTan)
    s32 a = -((i * i) >> 14);
    s32 b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    return (i * b) >> 16;
}

u16 ArcTan2(s16 x, s16 y) {                            // 0..0xFFFF = 0..2*PI
    s32 r;
    if (y == 0) return x >= 0 ? 0 : 0x8000;
    if (x == 0) return y >= 0 ? 0x4000 : 0xC000;
    if (y >= 0) {
        if (x >= 0) { r = (x >= y) ? ArcTanPoly(((s32)y << 14) / x) : 0x4000 - ArcTanPoly(((s32)x << 14) / y); }
        else        { r = (-x >= y) ? ArcTanPoly(((s32)y << 14) / x) + 0x8000 : 0x4000 - ArcTanPoly(((s32)x << 14) / y); }
    } else {
        if (x <= 0) { r = (-x > -y) ? ArcTanPoly(((s32)y << 14) / x) + 0x8000 : 0xC000 - ArcTanPoly(((s32)x << 14) / y); }
        else        { r = (x >= -y) ? ArcTanPoly(((s32)y << 14) / x) + 0x10000 : 0xC000 - ArcTanPoly(((s32)x << 14) / y); }
    }
    return (u16)r;
}

/* ---------- Cópia / preenchimento de memória ---------- */
void CpuSet(const void *src, void *dest, u32 control) {
    u32 count = control & 0x1FFFFF;
    int fixed = (control & 0x01000000) != 0, w32 = (control & 0x04000000) != 0;
    if (w32) { const u32 *s = src; u32 *d = dest; for (u32 i = 0; i < count; i++) d[i] = fixed ? *s : s[i]; }
    else     { const u16 *s = src; u16 *d = dest; for (u32 i = 0; i < count; i++) d[i] = fixed ? *s : s[i]; }
}

void CpuFastSet(const void *src, void *dest, u32 control) {
    u32 count = ((control & 0x1FFFFF) + 7) & ~7u;      // sempre múltiplos de 8 palavras
    int fixed = (control & 0x01000000) != 0;
    const u32 *s = src; u32 *d = dest;
    for (u32 i = 0; i < count; i++) d[i] = fixed ? *s : s[i];
}

/* ---------- Descompressão ---------- */
static void LZ77(const u8 *src, u8 *dst) {
    u32 hdr = *(const u32 *)src, size = hdr >> 8; src += 4;
    u32 out = 0;
    while (out < size) {
        u8 flags = *src++;
        for (int i = 0; i < 8 && out < size; i++, flags <<= 1) {
            if (flags & 0x80) {
                u32 len = (src[0] >> 4) + 3, disp = (((src[0] & 0xF) << 8) | src[1]) + 1; src += 2;
                while (len-- && out < size) { dst[out] = dst[out - disp]; out++; }
            } else dst[out++] = *src++;
        }
    }
}
void LZ77UnCompWram(const void *src, void *dest) { LZ77(src, dest); }
void LZ77UnCompVram(const void *src, void *dest) { LZ77(src, dest); }   // VRAM é memória comum no PC

static void RL(const u8 *src, u8 *dst) {
    u32 hdr = *(const u32 *)src, size = hdr >> 8; src += 4;
    u32 out = 0;
    while (out < size) {
        u8 f = *src++;
        if (f & 0x80) { u32 len = (f & 0x7F) + 3; u8 v = *src++; while (len-- && out < size) dst[out++] = v; }
        else          { u32 len = (f & 0x7F) + 1; while (len-- && out < size) dst[out++] = *src++; }
    }
}
void RLUnCompWram(const void *src, void *dest) { RL(src, dest); }
void RLUnCompVram(const void *src, void *dest) { RL(src, dest); }

/* ---------- Transformações afins (ATENÇÃO: fórmulas conferidas só contra a documentação, não contra o hardware) ---------- */
static s32 sin14(u32 a8) { return (s32)lround(sin(2.0 * M_PI * (a8 & 0xFF) / 256.0) * 16384.0); }
static s32 cos14(u32 a8) { return (s32)lround(cos(2.0 * M_PI * (a8 & 0xFF) / 256.0) * 16384.0); }

struct BgSrc { s32 texX, texY; s16 scrX, scrY, sx, sy; u16 alpha; };
struct BgDst { s16 pa, pb, pc, pd; s32 dx, dy; };
void BgAffineSet(struct BgSrc *src, struct BgDst *dst, s32 count) {
    for (s32 n = 0; n < count; n++) {
        u32 th = src[n].alpha >> 8;
        s32 c = cos14(th), s = sin14(th);
        s16 pa = (s16)((c * src[n].sx) >> 14), pb = (s16)(-((s * src[n].sx) >> 14));
        s16 pc = (s16)((s * src[n].sy) >> 14), pd = (s16)((c * src[n].sy) >> 14);
        dst[n].pa = pa; dst[n].pb = pb; dst[n].pc = pc; dst[n].pd = pd;
        dst[n].dx = src[n].texX - (pa * src[n].scrX + pb * src[n].scrY);
        dst[n].dy = src[n].texY - (pc * src[n].scrX + pd * src[n].scrY);
    }
}

void ObjAffineSet(void *src, void *dest, s32 count, s32 offset) {
    const u8 *s = src; u8 *d = dest;
    for (s32 n = 0; n < count; n++, s += 8) {           // a BIOS lê as structs de origem com passo de 8 bytes
        s16 sx = *(const s16 *)(s + 0), sy = *(const s16 *)(s + 2); u16 rot = *(const u16 *)(s + 4);
        u32 th = rot >> 8; s32 c = cos14(th), si = sin14(th);
        *(s16 *)(d + 0 * offset) = (s16)((c * sx) >> 14);
        *(s16 *)(d + 1 * offset) = (s16)(-((si * sx) >> 14));
        *(s16 *)(d + 2 * offset) = (s16)((si * sy) >> 14);
        *(s16 *)(d + 3 * offset) = (s16)((c * sy) >> 14);
        d += 4 * offset;
    }
}

/* ---------- Sistema ---------- */
void RegisterRamReset(u32 flags) {
    if (getenv("PC_DEBUG")) fprintf(stderr, "[pc/bios] RegisterRamReset(0x%02x)\n", flags);
    volatile u16 *io = (volatile u16 *)0x04000000;
    io[0] = 0x0080;                                              // DISPCNT: a BIOS sempre começa em "forced blank"
    if (flags & 0x01) memset((void *)0x02000000, 0, 0x40000);   // EWRAM (a região mapeada)
    if (flags & 0x02) memset((void *)0x03000000, 0, 0x7E00);     // IWRAM (sem a pilha)
    if (flags & 0x04) memset((void *)0x05000000, 0, 0x400);      // paleta
    if (flags & 0x08) memset((void *)0x06000000, 0, 0x18000);    // VRAM
    if (flags & 0x10) memset((void *)0x07000000, 0, 0x400);      // OAM
    if (flags & 0x20) memset((void *)0x04000120, 0, 0x0C);       // SIO
    if (flags & 0x40) { memset((void *)0x04000060, 0, 0x28); io[0x88 / 2] = 0x0200; }   // som (SOUNDBIAS = 0x200)
    if (flags & 0x80) {                                          // registradores de vídeo/DMA/etc.
        memset((void *)0x04000004, 0, 0x5C);                     // DISPSTAT .. BLDY
        io[0x20 / 2] = 0x100; io[0x26 / 2] = 0x100;              // BG2PA, BG2PD = 1.0 (matriz identidade)
        io[0x30 / 2] = 0x100; io[0x36 / 2] = 0x100;              // BG3PA, BG3PD = 1.0
        memset((void *)0x040000B0, 0, 0x30);                     // DMA0-3
    }
}

extern char **gPcArgv;
void SoftReset(u32 flags) {                              // reinicia o processo (como o reset por A+B+Start+Select)
    (void)flags; fprintf(stderr, "[pc] SoftReset: reiniciando o jogo...\n"); fflush(NULL);
    if (gPcArgv) execv("/proc/self/exe", gPcArgv);
    fprintf(stderr, "[pc] execv falhou; encerrando.\n"); exit(0);
}
int MultiBoot(void *mp) { (void)mp; return 1; }                 // sem multiboot no PC: retorna falha

void PcVBlankWait(void);                         // definido em pc/ppu_timing.c
void VBlankIntrWait(void) { PcVBlankWait(); }

/* ---------- Substitutos de asm ARM ---------- */
void PcHalt(void) { fprintf(stderr, "[pc] script chegou em HALT (SWI 2); encerrando.\n"); exit(1); }
void PcClearIwram(void) { memset((void *)0x03000000, 0, 0x7E00); }
void PcMusicPlayerJumpTableCopy(void) { /* TODO: tabela de comandos do player de música (som) */ }

#include <stdarg.h>
void PcLog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}
