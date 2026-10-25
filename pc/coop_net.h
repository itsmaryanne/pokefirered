#ifndef PC_COOP_NET_H
#define PC_COOP_NET_H
#include <stdint.h>
struct PcCoopPlayerState {
    uint8_t mapGroup, mapNum;
    uint8_t facing;       /* direcao do jogo: 1=baixo 2=cima 3=esquerda 4=direita */
    uint8_t gfx;          /* graphicsId do sprite (ainda nao usado pelo fantasma) */
    uint8_t elevation;
    uint8_t valid;
    int16_t x, y;         /* currentCoords do object event (com o deslocamento do mapa) */
};
/* eventos de progresso: kind 0 = flag (index = bit, value 0/1), 1 = var (index no vars[], value), 2 = var mesclada (fica o maior) */
struct PcCoopSyncEvent { uint8_t kind; uint16_t index; uint16_t value; };
void PcCoopNetSendEvent(const struct PcCoopSyncEvent *e);
int  PcCoopNetPollEvent(struct PcCoopSyncEvent *e);      /* 1 se havia um evento recebido */
int  PcCoopNetJustConnected(void);                       /* 1 uma vez a cada (re)conexao */
void PcCoopNetStart(void);
int  PcCoopNetActive(void);                              /* 1 se PC_COOP_HOST ou PC_COOP_JOIN existe */
void PcCoopNetSetLocal(const struct PcCoopPlayerState *s);
int  PcCoopNetGetRemote(struct PcCoopPlayerState *out);  /* 1 se conectado e com estado remoto valido */
#endif
