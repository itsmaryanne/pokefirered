#include "global.h"
#include "coop.h"
#include "link.h"
#include "main.h"
#include "sound.h"
#include "task.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "constants/songs.h"
#include "constants/event_object_movement.h"

// Conecta sozinho ao entrar no overworld (sem atalho de teclas).
// Mude para 0 para jogar normalmente, sem tentar abrir o link.
#define COOP_AUTOSTART 1
#define COOP_AUTOSTART_DELAY 120 // frames de espera depois que o overworld comeca
#define COOP_RETRY_DELAY 300     // frames extras de espera depois de uma tentativa que falhou

// Quantos consoles esperamos para a conexao (por enquanto so 2).
#define COOP_PLAYERS_WANTED 2

// Tempo maximo de cada tentativa de conexao, em frames (~25 s).
#define COOP_CONNECT_TIMEOUT 1500

// LocalId dos avatares remotos no mapa (200 a 203).
#define COOP_LOCAL_ID_BASE 200

// Toca um "pin" quando o OUTRO jogador da um passo.
#define COOP_DEBUG_SOUND 0

// Escreve mensagens no log do mGBA (so eventos principais).
#define COOP_DEBUG_LOG 1

enum
{
    COOP_IDLE,
    COOP_CONNECTING,
    COOP_ACTIVE,
};

#define tState data[0]
#define tTimer data[1]
#define tStable data[2]
#define tWait data[3]
#define tSentReq data[4]

extern void BuildSendCmd(u16 command);

EWRAM_DATA struct CoopRemotePlayer gCoopRemote[MAX_LINK_PLAYERS] = {};
static EWRAM_DATA u8 sCoopState = COOP_IDLE;
static EWRAM_DATA s16 sAutoDelay = 0;

static void Task_CoopConnect(u8 taskId);
static void Coop_CancelConnect(u8 taskId);

#if COOP_DEBUG_LOG
// Protocolo de depuracao do mGBA (nivel Aviso).
static void Coop_Log(const char *label, u32 value)
{
    vu8 *out = (vu8 *)0x04FFF600;
    const char *prefix = "COOP ";
    u8 i = 0;
    s8 shift;

    *(vu16 *)0x04FFF780 = 0xC0DE;
    if (*(vu16 *)0x04FFF780 != 0x1DEA)
        return;

    while (*prefix != '\0')
        out[i++] = *prefix++;
    while (*label != '\0' && i < 200)
        out[i++] = *label++;
    out[i++] = '=';
    for (shift = 12; shift >= 0; shift -= 4)
        out[i++] = "0123456789ABCDEF"[(value >> shift) & 0xF];
    out[i] = '\0';
    *(vu16 *)0x04FFF700 = 2 | 0x100;
}
#else
#define Coop_Log(label, value)
#endif

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

    if (remote->packetCount == 0)
        Coop_Log("first-pkt-from", playerId);

#if COOP_DEBUG_SOUND
    if (playerId != GetMultiplayerId() && (remote->x != (s16)in[0] || remote->y != (s16)in[1]))
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

// Cria/atualiza o avatar do jogador remoto "id" no mapa atual.
static void Coop_UpdateAvatar(u8 id)
{
    struct CoopRemotePlayer *remote = &gCoopRemote[id];
    u8 localId = COOP_LOCAL_ID_BASE + id;
    u8 mapGroup = gSaveBlock1Ptr->location.mapGroup;
    u8 mapNum = gSaveBlock1Ptr->location.mapNum;
    u8 objId;
    struct ObjectEvent *obj;
    s16 dx;
    s16 dy;
    u8 dir;

    if (id == GetMultiplayerId() || remote->packetCount == 0)
        return;

    objId = GetObjectEventIdByLocalIdAndMap(localId, mapNum, mapGroup);

    // Jogador remoto em outro mapa: sem avatar aqui.
    if (remote->mapGroup != mapGroup || remote->mapNum != mapNum)
    {
        if (objId < OBJECT_EVENTS_COUNT)
            RemoveObjectEventByLocalIdAndMap(localId, mapNum, mapGroup);
        return;
    }

    // Cria o avatar se ainda nao existe neste mapa.
    if (objId >= OBJECT_EVENTS_COUNT)
    {
        objId = SpawnSpecialObjectEventParameterized(
            GetRivalAvatarGraphicsIdByStateIdAndGender(PLAYER_AVATAR_STATE_NORMAL, gLinkPlayers[id].gender),
            MOVEMENT_TYPE_NONE,
            localId,
            remote->x,
            remote->y,
            remote->elevation);
        if (objId >= OBJECT_EVENTS_COUNT)
            return;
    }

    obj = &gObjectEvents[objId];

    // Ainda terminando um movimento: espera.
    if (ObjectEventClearHeldMovementIfFinished(obj) == 0)
        return;

    dx = remote->x - obj->currentCoords.x;
    dy = remote->y - obj->currentCoords.y;

    if (dx == 0 && dy == 0)
    {
        // Parado no lugar certo: so ajusta para onde esta olhando.
        if (obj->facingDirection != remote->facing)
            ObjectEventSetHeldMovement(obj, GetFaceDirectionMovementAction(remote->facing));
    }
    else if ((dx == 0 && (dy == 1 || dy == -1)) || (dy == 0 && (dx == 1 || dx == -1)))
    {
        // Um tile de distancia: anda (ou corre) ate la.
        if (dx > 0)
            dir = DIR_EAST;
        else if (dx < 0)
            dir = DIR_WEST;
        else if (dy > 0)
            dir = DIR_SOUTH;
        else
            dir = DIR_NORTH;

        if (remote->avatarFlags & PLAYER_AVATAR_FLAG_DASH)
            ObjectEventSetHeldMovement(obj, GetWalkFastMovementAction(dir));
        else
            ObjectEventSetHeldMovement(obj, GetWalkNormalMovementAction(dir));
    }
    else
    {
        // Longe demais (spawn, warp, atraso): teletransporta.
        MoveObjectEventToMapCoords(obj, remote->x, remote->y);
    }
}

// Chamado todo frame pelo OverworldBasic.
// Parado: espera um pouco e comeca a conectar sozinho.
// Ativo: religa o envio de posicao quando o link fica ocioso
// (o SendBlock e o standby terminam com gLinkCallback = NULL)
// e atualiza os avatares dos outros jogadores.
void Coop_OverworldTick(void)
{
    u8 i;

    switch (sCoopState)
    {
    case COOP_IDLE:
#if COOP_AUTOSTART
        if (++sAutoDelay > COOP_AUTOSTART_DELAY)
        {
            sAutoDelay = 0;
            sCoopState = COOP_CONNECTING;
            CreateTask(Task_CoopConnect, 80);
        }
#endif
        break;
    case COOP_ACTIVE:
        if (IsLinkTaskFinished())
            StartSendingCoopPos();
        for (i = 0; i < MAX_LINK_PLAYERS; i++)
            Coop_UpdateAvatar(i);
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
        Coop_Log("timeout-state", tState);
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
        Coop_Log("open", 0);
        tState = 1;
        break;
    case 1: // espera os consoles se enxergarem
        if (tTimer > 10 && GetLinkPlayerCount_2() >= COOP_PLAYERS_WANTED)
        {
            Coop_Log("found-cnt", GetLinkPlayerCount_2());
            PlaySE(SE_SELECT);
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
                Coop_Log("advance-master", 1);
                tState = 3;
            }
        }
        else
        {
            Coop_Log("advance-slave", 0);
            tState = 3;
        }
        break;
    case 3: // espera gLinkPlayers ser preenchido nos dois consoles
        if (gReceivedRemoteLinkPlayers == TRUE && IsLinkPlayerDataExchangeComplete() == TRUE)
        {
            Coop_Log("connected-id-cnt", (GetMultiplayerId() << 8) | GetLinkPlayerCount_2());
            StartSendingCoopPos();
            sCoopState = COOP_ACTIVE;
            PlaySE(SE_PIN);
            DestroyTask(taskId);
            break;
        }
        tWait++;
        // Se a troca de dados dos jogadores nao comecou sozinha, o mestre a pede.
        if (tWait > 120 && !tSentReq && IsLinkMaster() == TRUE && gReceivedRemoteLinkPlayers != TRUE)
        {
            tSentReq = TRUE;
            BuildSendCmd(LINKCMD_SEND_LINK_TYPE);
            Coop_Log("request-linktype", 1);
        }
        break;
    }
}

static void Coop_CancelConnect(u8 taskId)
{
    CloseLink();
    sCoopState = COOP_IDLE;
    sAutoDelay = -COOP_RETRY_DELAY;
    PlaySE(SE_BOO);
    DestroyTask(taskId);
}

#undef tState
#undef tTimer
#undef tStable
#undef tWait
#undef tSentReq