// Memória compartilhada entre o jogo (32 bits) e o visualizador (64 bits). Só tipos de tamanho fixo.
#ifndef PC_SHARED_H
#define PC_SHARED_H
#include <stdint.h>
#define PC_SHM_NAME "/pokefirered_pc"
#define PC_SHM_MAGIC 0x50464332u            /* "PFC2" */
#define PC_AUDIO_RATE 32768
#define PC_AUDIO_RING_FRAMES 16384          /* 0,5 s de áudio estéreo */
struct PcShared {
    volatile uint32_t magic;
    volatile uint32_t frameCounter;         /* incrementa a cada quadro pronto */
    volatile uint32_t keyInput;             /* teclas no formato do REG_KEYINPUT (ativo em zero; bits 0-9) */
    volatile uint32_t quit;                 /* o visualizador põe 1 ao fechar; o jogo encerra */
    volatile uint32_t gamePid;              /* pid do processo do jogo que está escrevendo (o visualizador confere se está vivo) */
    volatile uint32_t audioWrite;           /* quadros estéreo escritos (contador total) */
    volatile uint32_t speed;                /* avanço rápido: 1 = normal */
    volatile uint32_t audioRead;            /* quadros estéreo consumidos pelo visualizador (contador total) */
    volatile uint32_t coopKeyInput;         /* segundo jogador, REG_KEYINPUT (ativo em zero) */
    volatile uint32_t coopConnected;        /* 1 quando o segundo visualizador está conectado */
    uint32_t frame[240 * 160];              /* ARGB8888 */
    int16_t audio[PC_AUDIO_RING_FRAMES * 2];/* anel estéreo intercalado, s16 */
};
#endif
