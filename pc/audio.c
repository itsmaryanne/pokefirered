// Saída de áudio do port PC: emula os 4 canais PSG do GBA (2 quadrados, 1 de onda, 1 de ruído), mistura com o
// PCM do motor m4a (FIFO A = direita, FIFO B = esquerda) e entrega amostras de 32768 Hz ao visualizador por um
// anel em memória compartilhada. Chamado uma vez por quadro (no m4aSoundVSync).
// Os registradores de som são lidos da memória simulada do GBA (0x04000060...). Como não há como detectar a
// ESCRITA no gatilho (bit 7 de NRx4), o gatilho é detectado por polling uma vez por quadro e o bit é limpo aqui.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shared.h"

struct PcShared *PcGetShared(void);

#define IO8(o)  (*(volatile uint8_t *)(0x04000000 + (o)))
#define IO16(o) (*(volatile uint16_t *)(0x04000000 + (o)))
#define CYC_PER_SAMPLE 128                         /* 4194304 Hz / 32768 Hz */

typedef struct {
    int on, dac;
    int vol;                                       /* volume atual 0..15 (canais 1, 2, 4) */
    int envDir, envPeriod, envTimer;
    int lenCnt, lenEn, lastLenReg;
    int timer, period, pos;
    int freq, lastRegFreq;                         /* freq de 11 bits (canais 1-3) */
    int swEn, swShadow, swTimer, swPeriod, swShift, swDir;   /* sweep do canal 1 */
    uint32_t lfsr; int lfsrWidth;                  /* canal 4 */
} Psg;

static Psg ch[4];
static int fsCycles, fsStep;
static const uint8_t kDuty[4][8] = {{0,0,0,0,0,0,0,1},{1,0,0,0,0,0,0,1},{1,0,0,0,0,1,1,1},{0,1,1,1,1,1,1,0}};

static void EnvTick(Psg *c) {
    if (!c->on || c->envPeriod == 0) return;
    if (--c->envTimer <= 0) {
        c->envTimer = c->envPeriod;
        if (c->envDir) { if (c->vol < 15) c->vol++; } else if (c->vol > 0) c->vol--;
    }
}
static void LenTick(Psg *c) { if (c->lenEn && c->lenCnt > 0 && --c->lenCnt == 0) c->on = 0; }

static void SweepTick(void) {
    Psg *c = &ch[0];
    if (!c->on || !c->swEn || c->swPeriod == 0) return;
    if (--c->swTimer > 0) return;
    c->swTimer = c->swPeriod;
    int delta = c->swShadow >> c->swShift;
    int nf = c->swDir ? c->swShadow - delta : c->swShadow + delta;
    if (nf > 2047) { c->on = 0; return; }
    if (c->swShift && nf >= 0) { c->swShadow = nf; c->freq = nf; }
}

static void FrameSeqTick(void) {
    if ((fsStep & 1) == 0) { LenTick(&ch[0]); LenTick(&ch[1]); LenTick(&ch[2]); LenTick(&ch[3]); }
    if (fsStep == 2 || fsStep == 6) SweepTick();
    if (fsStep == 7) { EnvTick(&ch[0]); EnvTick(&ch[1]); EnvTick(&ch[3]); }
    fsStep = (fsStep + 1) & 7;
}

static int Freq11(int lo, int hi) { return lo | ((hi & 7) << 8); }

// Lê registradores, detecta gatilhos (bit 7 de NRx4) e atualiza os parâmetros dos canais. Uma vez por quadro.
static void LoadRegs(void) {
    /* ---- canal 1 ---- */
    {
        Psg *c = &ch[0]; int nr10 = IO8(0x60), nr11 = IO8(0x62), nr12 = IO8(0x63), nr13 = IO8(0x64), nr14 = IO8(0x65);
        int rf = Freq11(nr13, nr14);
        if (rf != c->lastRegFreq) { c->freq = rf; c->lastRegFreq = rf; }
        c->dac = (nr12 & 0xF8) != 0;
        if (!c->dac) c->on = 0;
        if (nr14 & 0x80) {
            IO8(0x65) = (uint8_t)(nr14 & 0x7F);
            c->lenEn = (nr14 >> 6) & 1;
            if (c->lenEn && (!c->on || nr11 != c->lastLenReg)) c->lenCnt = 64 - (nr11 & 63);
            else if (c->lenCnt == 0) c->lenCnt = 64;
            c->lastLenReg = nr11;
            c->freq = rf; c->lastRegFreq = rf;
            c->vol = nr12 >> 4; c->envDir = (nr12 >> 3) & 1; c->envPeriod = nr12 & 7; c->envTimer = c->envPeriod ? c->envPeriod : 8;
            c->swShift = nr10 & 7; c->swDir = (nr10 >> 3) & 1; c->swPeriod = (nr10 >> 4) & 7;
            c->swShadow = rf; c->swTimer = c->swPeriod ? c->swPeriod : 8; c->swEn = (c->swPeriod || c->swShift);
            c->timer = (2048 - c->freq) * 4;
            c->on = c->dac;
            if (c->swShift) { int nf = c->swDir ? rf - (rf >> c->swShift) : rf + (rf >> c->swShift); if (nf > 2047) c->on = 0; }
        } else {
            c->lenEn = (nr14 >> 6) & 1;
        }
        c->period = (2048 - c->freq) * 4; if (c->period < 4) c->period = 4;
    }
    /* ---- canal 2 ---- */
    {
        Psg *c = &ch[1]; int nr21 = IO8(0x68), nr22 = IO8(0x69), nr23 = IO8(0x6C), nr24 = IO8(0x6D);
        int rf = Freq11(nr23, nr24);
        c->freq = rf;
        c->dac = (nr22 & 0xF8) != 0;
        if (!c->dac) c->on = 0;
        if (nr24 & 0x80) {
            IO8(0x6D) = (uint8_t)(nr24 & 0x7F);
            c->lenEn = (nr24 >> 6) & 1;
            if (c->lenEn && (!c->on || nr21 != c->lastLenReg)) c->lenCnt = 64 - (nr21 & 63);
            else if (c->lenCnt == 0) c->lenCnt = 64;
            c->lastLenReg = nr21;
            c->vol = nr22 >> 4; c->envDir = (nr22 >> 3) & 1; c->envPeriod = nr22 & 7; c->envTimer = c->envPeriod ? c->envPeriod : 8;
            c->timer = (2048 - c->freq) * 4;
            c->on = c->dac;
        } else {
            c->lenEn = (nr24 >> 6) & 1;
        }
        c->period = (2048 - c->freq) * 4; if (c->period < 4) c->period = 4;
    }
    /* ---- canal 3 (onda) ---- */
    {
        Psg *c = &ch[2]; int nr30 = IO8(0x70), nr31 = IO8(0x72), nr33 = IO8(0x74), nr34 = IO8(0x75);
        int rf = Freq11(nr33, nr34);
        c->freq = rf;
        c->dac = (nr30 & 0x80) != 0;
        if (!c->dac) c->on = 0;
        if (nr34 & 0x80) {
            IO8(0x75) = (uint8_t)(nr34 & 0x7F);
            c->lenEn = (nr34 >> 6) & 1;
            if (c->lenEn && (!c->on || nr31 != c->lastLenReg)) c->lenCnt = 256 - nr31;
            else if (c->lenCnt == 0) c->lenCnt = 256;
            c->lastLenReg = nr31;
            c->timer = (2048 - c->freq) * 2; c->pos = 0;
            c->on = c->dac;
        } else {
            c->lenEn = (nr34 >> 6) & 1;
        }
        c->period = (2048 - c->freq) * 2; if (c->period < 2) c->period = 2;
    }
    /* ---- canal 4 (ruído) ---- */
    {
        Psg *c = &ch[3]; int nr41 = IO8(0x78), nr42 = IO8(0x79), nr43 = IO8(0x7C), nr44 = IO8(0x7D);
        c->dac = (nr42 & 0xF8) != 0;
        if (!c->dac) c->on = 0;
        int r = nr43 & 7, s = nr43 >> 4;
        int div = r ? r * 16 : 8;
        c->period = div << s; if (c->period < 8) c->period = 8;
        c->lfsrWidth = (nr43 >> 3) & 1;
        if (nr44 & 0x80) {
            IO8(0x7D) = (uint8_t)(nr44 & 0x7F);
            c->lenEn = (nr44 >> 6) & 1;
            if (c->lenEn && (!c->on || nr41 != c->lastLenReg)) c->lenCnt = 64 - (nr41 & 63);
            else if (c->lenCnt == 0) c->lenCnt = 64;
            c->lastLenReg = nr41;
            c->vol = nr42 >> 4; c->envDir = (nr42 >> 3) & 1; c->envPeriod = nr42 & 7; c->envTimer = c->envPeriod ? c->envPeriod : 8;
            c->lfsr = 0x7FFF; c->timer = c->period;
            c->on = c->dac;
        } else {
            c->lenEn = (nr44 >> 6) & 1;
        }
    }
}

// Roda um canal por `cycles` ciclos e devolve a soma (nível * ciclos); o nível médio é soma / cycles.
static int RunChannel(int idx, int cycles) {
    Psg *c = &ch[idx];
    if (!c->on) return 0;
    int acc = 0;
    if (idx == 0 || idx == 1) {
        int duty = (idx == 0 ? IO8(0x62) : IO8(0x68)) >> 6;
        while (cycles > 0) {
            int step = c->timer < cycles ? c->timer : cycles;
            acc += kDuty[duty][c->pos] * c->vol * step;
            c->timer -= step; cycles -= step;
            if (c->timer <= 0) { c->pos = (c->pos + 1) & 7; c->timer = c->period; }
        }
    } else if (idx == 2) {
        int vreg = IO16(0x72);
        int code = (vreg >> 13) & 3, force75 = (vreg >> 15) & 1;
        while (cycles > 0) {
            int step = c->timer < cycles ? c->timer : cycles;
            uint8_t b = IO8(0x90 + (c->pos >> 1));
            int smp = (c->pos & 1) ? (b & 15) : (b >> 4);
            if (force75) smp = smp * 3 / 4; else if (code == 0) smp = 0; else if (code == 2) smp >>= 1; else if (code == 3) smp >>= 2;
            acc += smp * step;
            c->timer -= step; cycles -= step;
            if (c->timer <= 0) { c->pos = (c->pos + 1) & 31; c->timer = c->period; }
        }
    } else {
        while (cycles > 0) {
            int step = c->timer < cycles ? c->timer : cycles;
            acc += (int)((~c->lfsr) & 1) * c->vol * step;
            c->timer -= step; cycles -= step;
            if (c->timer <= 0) {
                uint32_t x = (c->lfsr & 1) ^ ((c->lfsr >> 1) & 1);
                c->lfsr = (c->lfsr >> 1) | (x << 14);
                if (c->lfsrWidth) c->lfsr = (c->lfsr & ~(1u << 6)) | (x << 6);
                c->timer = c->period;
            }
        }
    }
    return acc;
}

/* ------------------------------- saída / anel ------------------------------- */

static double srcPos;                              /* posição fracionária no PCM de entrada (em amostras) */
static float lastR, lastL;                         /* última amostra PCM do quadro anterior (para interpolar) */
static float dcR, dcL, prevInR, prevInL;           /* filtro passa-altas (remove o DC do PSG) */
static FILE *dumpFile; static int dumpInit;

static int16_t Clip16(float v) { if (v > 511.0f) v = 511.0f; if (v < -512.0f) v = -512.0f; v *= 48.0f; return (int16_t)v; }   /* 10 bits, como o mixer do GBA */

void PcAudioEmit(const int8_t *right, const int8_t *left, int n, int pcmFreq) {
    if (n <= 0 || pcmFreq <= 0) return;
    if (!dumpInit) { dumpInit = 1; const char *p = getenv("PC_AUDIO_DUMP"); if (p) dumpFile = fopen(p, "wb"); }
    LoadRegs();

    static int mask = -1; if (mask < 0) { const char *e = getenv("PC_AUDIO_MASK"); mask = e ? (int)strtol(e, NULL, 0) : 0x3F; }   /* depuração: bits 0-3 = PSG 1-4, 4 = FIFO A, 5 = FIFO B */
    int master = (IO8(0x84) & 0x80) != 0;          /* SOUNDCNT_X bit 7 */
    int nr50 = IO8(0x80), nr51 = IO8(0x81);
    int volR = (nr50 & 7) + 1, volL = ((nr50 >> 4) & 7) + 1;
    int cnth = IO16(0x82);
    /* Escala igual à do mGBA: PSG = (nível*8)*(1+vol) >> (4 - código); FIFO = amostra*4 (100%) ou *2 (50%); soma limitada a 10 bits; saída = unidades*48 */
    static const float kRatio[4] = {0.5f, 1.0f, 2.0f, 0.5f};
    float psgRatio = kRatio[cnth & 3];
    float dsA = (cnth & 4) ? 4.0f : 2.0f, dsB = (cnth & 8) ? 4.0f : 2.0f;
    int aR = (cnth >> 8) & 1, aL = (cnth >> 9) & 1, bR = (cnth >> 12) & 1, bL = (cnth >> 13) & 1;

    double step = (double)pcmFreq / (double)PC_AUDIO_RATE;
    static int16_t out[2 * 1200];
    int nOut = 0;
    while (srcPos < n && nOut < 1200) {
        int i0 = (int)srcPos; float fr = (float)(srcPos - i0);
        float aS0 = i0 >= 0 ? right[i0] : lastR, aS1 = (i0 + 1 < n) ? right[i0 + 1] : right[i0];
        float bS0 = i0 >= 0 ? left[i0] : lastL,  bS1 = (i0 + 1 < n) ? left[i0 + 1] : left[i0];
        float A = aS0 + (aS1 - aS0) * fr, B = bS0 + (bS1 - bS0) * fr;
        srcPos += step;

        fsCycles += CYC_PER_SAMPLE;
        while (fsCycles >= 8192) { fsCycles -= 8192; FrameSeqTick(); }
        float chL = 0, chR = 0;
        for (int k = 0; k < 4; k++) {
            float lvl = (float)RunChannel(k, CYC_PER_SAMPLE) / (float)CYC_PER_SAMPLE;
            if (!((mask >> k) & 1)) continue;
            if (nr51 & (1 << k)) chR += lvl;
            if (nr51 & (1 << (k + 4))) chL += lvl;
        }
        float psgR = chR * volR * psgRatio, psgL = chL * volL * psgRatio;
        float Am = (mask & 0x10) ? A : 0, Bm = (mask & 0x20) ? B : 0;
        float dsR = (aR ? Am * dsA : 0) + (bR ? Bm * dsB : 0);
        float dsL = (aL ? Am * dsA : 0) + (bL ? Bm * dsB : 0);
        float inR = master ? (dsR + psgR) : 0.0f, inL = master ? (dsL + psgL) : 0.0f;
        dcR = inR - prevInR + 0.995f * dcR; prevInR = inR;
        dcL = inL - prevInL + 0.995f * dcL; prevInL = inL;
        out[nOut * 2] = Clip16(dcL); out[nOut * 2 + 1] = Clip16(dcR);
        nOut++;
    }
    srcPos -= n;
    lastR = right[n - 1]; lastL = left[n - 1];

    if (dumpFile) { fwrite(out, 4, nOut, dumpFile); fflush(dumpFile); }
    struct PcShared *sh = PcGetShared();
    if (sh) {
        uint32_t w = sh->audioWrite, r = sh->audioRead;
        if (w - r + (uint32_t)nOut <= PC_AUDIO_RING_FRAMES) {
            for (int i = 0; i < nOut; i++) {
                uint32_t pos = (w + i) % PC_AUDIO_RING_FRAMES;
                sh->audio[pos * 2] = out[i * 2]; sh->audio[pos * 2 + 1] = out[i * 2 + 1];
            }
            __atomic_store_n(&sh->audioWrite, w + nOut, __ATOMIC_RELEASE);
        }                                          /* anel cheio (ninguém consumindo): descarta */
    }
}
