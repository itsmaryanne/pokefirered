// Despacho de interrupções em C (porta da lógica do IntrMain de src/crt0.s).
// A thread de temporização marca os flags em REG_IF e avisa a thread principal por sinal;
// o handler do sinal chama PcDeliverIntrs(), como o IRQ do hardware interromperia a CPU.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define REG_IE  (*(volatile uint16_t *)0x04000200)
#define REG_IF_PTR ((uint16_t *)0x04000202)
#define REG_IME (*(volatile uint16_t *)0x04000208)
extern void (*gIntrTable[])(void);

// Ordem exata da tabela do jogo: VCOUNT, SERIAL, TIMER3, HBLANK, VBLANK, TIMER0..2, DMA0..3, KEYPAD, GAMEPAK
static const uint16_t kFlags[14] = { 1<<2, 1<<7, 1<<6, 1<<1, 1<<0, 1<<3, 1<<4, 1<<5, 1<<8, 1<<9, 1<<10, 1<<11, 1<<12, 1<<13 };
static unsigned PcCountNonZero(const uint8_t *p, unsigned n) { unsigned c = 0; for (unsigned i = 0; i < n; i++) c += p[i] != 0; return c; }
volatile unsigned long gPcVBlankServiced;                // quantos V-blanks o jogo já tratou

// Trata UMA interrupção pendente e habilitada; retorna 1 se tratou alguma.
int PcDispatchOneIntr(void) {
    uint16_t pending = REG_IE & *(volatile uint16_t *)REG_IF_PTR;
    for (int i = 0; i < 14; i++) {
        if (pending & kFlags[i]) {
            __atomic_fetch_and(REG_IF_PTR, (uint16_t)~kFlags[i], __ATOMIC_SEQ_CST);   // "write 1 to clear" do hardware
            if (i != 1 && i != 2) gIntrTable[i]();   // PC: serial e timer3 so o RFU usa; handler em RAM nao executavel
            if (i == 4) {
                gPcVBlankServiced++;
                static int trace = -1; if (trace < 0) trace = getenv("PC_TRACE") != NULL;
                if (trace && gPcVBlankServiced % 60 == 0)
                    fprintf(stderr, "[pc] V-blank #%lu  DISPCNT=0x%04x  VRAM!=0: %u  PLTT!=0: %u  OAM!=0: %u bytes\n", gPcVBlankServiced,
                            *(volatile uint16_t *)0x04000000, PcCountNonZero((uint8_t *)0x06000000, 0x18000),
                            PcCountNonZero((uint8_t *)0x05000000, 0x400), PcCountNonZero((uint8_t *)0x07000000, 0x400));
            }
            return 1;
        }
    }
    return 0;
}

// Equivalente a uma entrada de IRQ: só age se IME estiver ligado; fica com IME=0 durante o tratamento.
void PcDeliverIntrs(void) {
    uint16_t ime = REG_IME;
    if (!(ime & 1)) return;
    REG_IME = 0;
    while (PcDispatchOneIntr()) {}
    REG_IME = ime;
}
void intr_main(void) { PcDeliverIntrs(); }                // símbolo que o jogo coloca em INTR_VECTOR
