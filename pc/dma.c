// Controlador de DMA simulado. No GBA, escrever o registrador de controle com ENABLE + "começar agora"
// copia a memória na hora (a CPU para). Aqui o macro DmaSet do jogo é redirecionado para PcDmaSet.
// DMAs com outro gatilho (V-blank, H-blank, especial) ficam só registradas; a camada de vídeo as dispara depois.
#include <stdint.h>
#include <stddef.h>
#define DMA_REGS(n) ((volatile uint32_t *)(0x040000B0 + (n) * 12))   // SAD, DAD, CNT_L|CNT_H<<16
#define REG_IF_PTR  ((uint16_t *)0x04000202)

void PcDmaRun(int n) {
    volatile uint32_t *r = DMA_REGS(n);
    uint32_t src = r[0], dst = r[1], ctl = r[2] >> 16, count = r[2] & 0xFFFF;
    if (count == 0) count = (n == 3) ? 0x10000 : 0x4000;
    int unit = (ctl & 0x0400) ? 4 : 2;
    int dctl = (ctl >> 5) & 3, sctl = (ctl >> 7) & 3;
    src &= ~(uint32_t)(unit - 1); dst &= ~(uint32_t)(unit - 1);
    uint32_t dst0 = dst;
    for (uint32_t i = 0; i < count; i++) {
        if (unit == 4) *(volatile uint32_t *)(uintptr_t)dst = *(volatile uint32_t *)(uintptr_t)src;
        else           *(volatile uint16_t *)(uintptr_t)dst = *(volatile uint16_t *)(uintptr_t)src;
        if (sctl == 0) src += unit; else if (sctl == 1) src -= unit;      // 2 = fixo
        if (dctl == 0 || dctl == 3) dst += unit; else if (dctl == 1) dst -= unit;
    }
    r[0] = src;                                               // como no hardware: endereços ficam avançados
    r[1] = (dctl == 3) ? dst0 : dst;                          // 3 = incrementa e recarrega no fim
    if (!(ctl & 0x0200)) r[2] &= ~(uint32_t)(0x8000u << 16);  // sem repeat: desliga ENABLE
    if (ctl & 0x4000) __atomic_fetch_or(REG_IF_PTR, (uint16_t)(1u << (8 + n)), __ATOMIC_SEQ_CST);
}

void PcDmaSet(int n, uint32_t src, uint32_t dest, uint32_t control) {
    volatile uint32_t *r = DMA_REGS(n);
    r[0] = src; r[1] = dest; r[2] = control;
    uint32_t ctl = control >> 16;
    if (!(ctl & 0x8000)) return;                              // não habilitado
    if (((ctl >> 12) & 3) != 0) return;                       // gatilho não imediato: a camada de vídeo trata
    PcDmaRun(n);
}

// DMAs com gatilho de H-blank (timing 2) e de V-blank (timing 1). No hardware o H-blank dispara no fim de cada linha visível;
// o renderizador chama PcHBlankDma() antes de desenhar cada linha a partir da 1. O V-blank dispara no começo do V-blank.
void PcHBlankDma(void) {
    for (int n = 0; n < 4; n++) {
        uint32_t ctl = DMA_REGS(n)[2] >> 16;
        if ((ctl & 0x8000) && ((ctl >> 12) & 3) == 2) PcDmaRun(n);
    }
}
void PcVBlankDma(void) {
    for (int n = 0; n < 4; n++) {
        uint32_t ctl = DMA_REGS(n)[2] >> 16;
        if ((ctl & 0x8000) && ((ctl >> 12) & 3) == 1) PcDmaRun(n);
    }
}
