// Pedaços do motor de som m4a reescritos em C a partir de src/m4a_1.s (usam as structs reais do jogo).
#include "gba/m4a_internal.h"
#include <string.h>

extern void *const gMPlayJumpTableTemplate[];

// Copia as 36 (0x24) entradas do template para a tabela de saltos em RAM.
// (a versão em asm ainda checava endereços da BIOS; isso não existe no PC)
void MPlayJumpTableCopy(MPlayFunc *mplayJumpTable) {
    memcpy((void *)mplayJumpTable, gMPlayJumpTableTemplate, 0x24 * sizeof(void *));
}

// Remove um canal da lista encadeada da sua trilha (porta fiel de RealClearChain).
void RealClearChain(void *x) {
    struct SoundChannel *chan = x;
    struct MusicPlayerTrack *track = chan->track;
    if (track == NULL) return;
    struct SoundChannel *next = chan->nextChannelPointer, *prev = chan->prevChannelPointer;
    if (prev != NULL) prev->nextChannelPointer = next; else track->chan = next;
    if (next != NULL) next->prevChannelPointer = prev;
    chan->track = NULL;
}
