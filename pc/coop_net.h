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
struct PcCoopSyncEvent { uint8_t kind; uint16_t index; uint16_t value; uint16_t a, b; };   /* kind 3 = NPC */
void PcCoopNetSendEvent(const struct PcCoopSyncEvent *e);
int  PcCoopNetPollEvent(struct PcCoopSyncEvent *e);      /* 1 se havia um evento recebido */
int  PcCoopNetJustConnected(void);                       /* 1 uma vez a cada (re)conexao */
/* cabo link virtual: o host numera os blocos e entrega a mesma sequência aos dois jogos */
int  PcLinkAvailable(void);                                            /* coop de rede ativo */
int  PcLinkIsHost(void);
int  PcLinkConnected(void);
int  PcLinkSubmit(const void *data, unsigned size);                    /* 1 = aceito */
int  PcLinkPopDelivered(uint8_t *sender, void *buf, unsigned *size);   /* 1 = havia um bloco */
int  PcLinkSubmitPending(void);                                        /* blocos nossos ainda nao devolvidos pelo host */
int  PcLinkSubmitAs(int sender, const void *data, unsigned size);      /* só o host: bloco em nome de um jogador fantasma */
void PcLinkSetMyId(int id);                                            /* id do convidado no cabo (1 no PvP, 2 no modo treinador) */
void PcCoopNetStart(void);
int  PcCoopNetActive(void);                              /* 1 se PC_COOP_HOST ou PC_COOP_JOIN existe */
void PcCoopNetSetLocal(const struct PcCoopPlayerState *s);
int  PcCoopNetGetRemote(struct PcCoopPlayerState *out);  /* 1 se conectado e com estado remoto valido */
#endif
