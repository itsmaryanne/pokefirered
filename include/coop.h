#ifndef GUARD_COOP_H
#define GUARD_COOP_H

#include "global.h"
#include "link.h"

// O comando de link tem CMD_LENGTH (8) palavras de 16 bits:
// palavra 0 = codigo do comando, palavras 1..7 = dados.
// O pacote de posicao usa as palavras 1..6; a 7 fica livre.
#define COOP_POS_WORDS 6

struct CoopRemotePlayer
{
    s16 x;              // currentCoords.x do ObjectEvent do jogador remoto
    s16 y;              // currentCoords.y
    u8 mapGroup;
    u8 mapNum;
    u8 facing;          // ObjectEvent.facingDirection
    u8 avatarFlags;     // gPlayerAvatar.flags (andando, bike, surf...)
    u8 elevation;
    u8 hidden;          // 1 = personagem escondido (porta, tapete, warp)
    u16 packetCount;    // quantos pacotes de posicao chegaram (para depurar)
};

extern struct CoopRemotePlayer gCoopRemote[MAX_LINK_PLAYERS];

// Chamadas pelo link.c
bool8 Coop_BuildPosCmd(u16 *out);                // preenche gSendCmd[1..]; FALSE = nada a enviar
void Coop_OnPosCmd(u8 playerId, const u16 *in);  // le gRecvCmds[id][1..]
void Coop_OnLinkError(u32 status, u8 recvQueue, u8 sendQueue);

// Chamada pelo main.c
void Coop_LinkCatchUp(void);

// Chamadas pelo overworld.c
void Coop_SnapshotLocal(void);
void Coop_OverworldTick(void);

// Chamada pelo event_object_movement.c
bool8 Coop_ObjectsPassThrough(struct ObjectEvent *mover, struct ObjectEvent *other);

bool8 Coop_IsActive(void);

#endif // GUARD_COOP_H