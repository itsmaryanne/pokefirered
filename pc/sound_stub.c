// Restos do motor de som: símbolos de assembly que sobram (SoundMainBTM, SoundMainRAM, umul3232H32).
// O motor de verdade está em m4a_engine.c; a saída de áudio, em audio.c.
#include <stdint.h>
#include <string.h>
// Entrada 35 da tabela de saltos: no asm original é "SoundMainBTM", que zera 64 bytes a partir do ponteiro.
// (fica aqui, sem incluir os headers do jogo, porque o header o declara com outra assinatura)
void SoundMainBTM(void *p) { memset(p, 0, 64); }
void SoundMainRAM(void) {}
uint32_t umul3232H32(uint32_t a, uint32_t b) { return (uint32_t)(((uint64_t)a * b) >> 32); }
