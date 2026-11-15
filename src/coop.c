#include "global.h"
#include "coop.h"
#include "link.h"
#include "main.h"
#include "sound.h"
#include "task.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "constants/songs.h"

// Quantos consoles esperamos para a conexao (por enquanto so 2).
#define COOP_PLAYERS_WANTED 2

// Tempo maximo de conexao, em frames (~25 s).
#define COOP_CONNECT_TIMEOUT 1500

// Para testar sem avatares: toca um "pin" quando o OUTRO jogador da um passo.
#define COOP_DEBUG_SOUND 1

enum
{
    COOP_IDLE,
    COOP_CONNECTING,
    COOP_ACTIVE,
};

#define tState  data[0]
#define tTimer  data[1]
#define tStable data[2]

EWRAM_DATA struct CoopRemotePlayer gCoopRemote[MAX_LINK_PLAYERS] = {};
static EWRAM_DATA u8 sCoopState = COOP_IDLE;

static void Task_CoopConnect(u8 taskId);
static void Coop_CancelConnect(u8 taskId);

bool8 Coop_IsActive(void)
{
    return sCoopState == COOP_ACTIVE;
}

// Monta o pacote de posicao do jogador local em out[0..COOP_POS_WORDS-1].
// Chamado pelo BuildSendCmd (link.c) quando o comando e LINKCMD_COOP_POS.
void Coop_BuildPosCmd(u16 *out)
{
    struct ObjectEvent *obj = &gObjectEvents[gPlayerAvatar.objectEventId];

    out[0] = (u16)obj->currentCoords.x;
    out[1] = (u16)obj->currentCoords.y;
    out[2] = gSaveBlock1Ptr->location.mapGroup | (gSaveBlock1Ptr->location.mapNum << 8);
    out[3] = obj->facingDirection | (gPlayerAvatar.flags << 8);
    out[4] = obj->currentElevation;
}

// Guarda o pacote recebido do jogador playerId.
// Chamado pelo ProcessRecvCmds (link.c).
void Coop_OnPosCmd(u8 playerId, const u16 *in)
{
    struct CoopRemotePlayer *remote;

    if (playerId >= MAX_LINK_PLAYERS)
        return;

    remote = &gCoopRemote[playerId];

#if COOP_DEBUG_SOUND
    if (playerId != GetMultiplayerId()
     && (remote->x != (s16)in[0] || remote->y != (s16)in[1]))
        PlaySE(SE_PIN);
#endif

    remote->x = (s16)in[0];
    remote->y = (s16)in[1];
    remote->mapGroup = in[2] & 0xFF;
    remote->mapNum = in[2] >> 8;
    remote->facing = in[3] & 0xFF;
    remote->avatarFlags = in[3] >> 8;
    remote->elevation = in[4] & 0xF;
    remote->packetCount++;
}

// Atalho de teste: segurar L e R juntos no overworld abre a conexao.
// Enquanto ativo, religa o envio de posicao quando o link fica ocioso
// (o SendBlock e o standby terminam com gLinkCallback = NULL).
void Coop_OverworldTick(void)
{
    switch (sCoopState)
    {
    case COOP_IDLE:
        if ((gMain.heldKeys & (L_BUTTON | R_BUTTON)) == (L_BUTTON | R_BUTTON)
         && (gMain.newKeys & (L_BUTTON | R_BUTTON)))
        {
            sCoopState = COOP_CONNECTING;
            CreateTask(Task_CoopConnect, 80);
        }
        break;
    case COOP_ACTIVE:
        if (IsLinkTaskFinished())
            StartSendingCoopPos();
        break;
    }
}

// Mesma sequencia que o Cable Club usa para conectar (Task_LinkupStart,
// Task_LinkupAwaitConnection e Task_ReestablishLink), mas sem sair do mapa.
static void Task_CoopConnect(u8 taskId)
{
    s16 *data = gTasks[taskId].data;

    if (++tTimer > COOP_CONNECT_TIMEOUT)
    {
        Coop_CancelConnect(taskId);
        return;
    }

    switch (tState)
    {
    case 0: // abre o link; todos os consoles precisam declarar o mesmo gLinkType
        gLinkType = LINKTYPE_COOP;
        OpenLinkTimed();
        ResetLinkPlayerCount();
        ResetLinkPlayers();
        tState = 1;
        break;
    case 1: // espera os consoles se enxergarem
        if (tTimer > 10 && GetLinkPlayerCount_2() >= COOP_PLAYERS_WANTED)
        {
            tStable = 0;
            tState = 2;
        }
        break;
    case 2: // o mestre libera a troca de dados dos jogadores
        if (IsLinkMaster() == TRUE)
        {
            if (++tStable > 30)
            {
                CheckShouldAdvanceLinkState();
                tState = 3;
            }
        }
        else
        {
            tState = 3;
        }
        break;
    case 3: // espera gLinkPlayers ser preenchido nos dois consoles
        if (gReceivedRemoteLinkPlayers == TRUE && IsLinkPlayerDataExchangeComplete() == TRUE)
        {
            StartSendingCoopPos();
            sCoopState = COOP_ACTIVE;
            PlaySE(SE_PIN);
            DestroyTask(taskId);
        }
        break;
    }
}

static void Coop_CancelConnect(u8 taskId)
{
    CloseLink();
    sCoopState = COOP_IDLE;
    PlaySE(SE_BOO);
    DestroyTask(taskId);
}

#undef tState
#undef tTimer
#undef tStable