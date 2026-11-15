#ifndef GUARD_COOP_H
#define GUARD_COOP_H

#include "global.h"
#include "link.h"

// O comando de link tem CMD_LENGTH (8) palavras de 16 bits:
// palavra 0 = codigo do comando, palavras 1..7 = dados.
// O pacote de posicao usa as palavras 1..5; 6 e 7 ficam livres.
#define COOP_POS_WORDS 5

struct CoopRemotePlayer
{
    s16 x;              // currentCoords.x do ObjectEvent do jogador remoto
    s16 y;              // currentCoords.y
    u8 mapGroup;
    u8 mapNum;
    u8 facing;          // ObjectEvent.facingDirection
    u8 avatarFlags;     // gPlayerAvatar.flags (andando, bike, surf...)
    u8 elevation;
    u16 packetCount;    // quantos pacotes de posicao chegaram (para depurar)
};

extern struct CoopRemotePlayer gCoopRemote[MAX_LINK_PLAYERS];

// Chamadas pelo link.c
void Coop_BuildPosCmd(u16 *out);                 // preenche gSendCmd[1..]
void Coop_OnPosCmd(u8 playerId, const u16 *in);  // le gRecvCmds[id][1..]

// Chamada todo frame pelo OverworldBasic (overworld.c)
void Coop_OverworldTick(void);

bool8 Coop_IsActive(void);

#endif // GUARD_COOP_H