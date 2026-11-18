#include "global.h"
#include "coop.h"
#include "link.h"
#include "main.h"
#include "sound.h"
#include "task.h"
#include "palette.h"
#include "sprite.h"
#include "fieldmap.h"
#include "field_door.h"
#include "metatile_behavior.h"
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

// Com o jogador parado, reenvia a posicao a cada tantos frames (~0,5 s).
#define COOP_HEARTBEAT_FRAMES 30

// LocalId dos avatares remotos no mapa (200 a 203).
#define COOP_LOCAL_ID_BASE 200

// Quantos frames empurrando contra um avatar ate poder atravessa-lo (~1,5 s).
#define COOP_PUSH_FRAMES 90

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

// Etapas da animacao de porta de cada avatar.
enum
{
    DOOR_NONE,
    DOOR_OPEN_THEN_APPEAR, // saiu de um predio: espera a porta abrir para aparecer
    DOOR_WAIT_LEAVE,       // visivel na porta aberta: fecha quando sair do tile
    DOOR_CLOSING_EXIT,     // porta fechando depois que ele saiu
    DOOR_OPEN_THEN_ENTER,  // vai entrar num predio: espera a porta abrir
    DOOR_WALKING_IN,       // andando ate o tile da porta
    DOOR_CLOSING_ENTER,    // porta fechando atras dele (ele ja esta escondido)
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
static EWRAM_DATA u8 sPassThrough[MAX_LINK_PLAYERS] = {};
static EWRAM_DATA u8 sPushFrames[MAX_LINK_PLAYERS] = {};
static EWRAM_DATA u16 sLocalPos[COOP_POS_WORDS] = {};
static EWRAM_DATA u16 sLastSentPos[COOP_POS_WORDS] = {};
static EWRAM_DATA u8 sSendIdle = 0;
static EWRAM_DATA u8 sDoorState[MAX_LINK_PLAYERS] = {};
static EWRAM_DATA s16 sDoorX[MAX_LINK_PLAYERS] = {};
static EWRAM_DATA s16 sDoorY[MAX_LINK_PLAYERS] = {};

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

// Chamado pelo CheckErrorStatus (link.c) quando o link acusa erro.
void Coop_OnLinkError(u32 status, u8 recvQueue, u8 sendQueue)
{
    Coop_Log("link-err-hi", status >> 16);
    Coop_Log("link-err-lo", status & 0xFFFF);
    Coop_Log("recv-queue", recvQueue);
    Coop_Log("send-queue", sendQueue);
}

// Chamado no fim de cada volta do loop principal (main.c).
// Quando uma volta demora (carregar mapa), os pacotes se acumulam na fila de
// recebimento; aqui ela e esvaziada com voltas extras so do link, como o frame
// duplo do Cable Club. Tambem mantem a tela de erro suprimida.
void Coop_LinkCatchUp(void)
{
    u8 i;

    if (sCoopState == COOP_IDLE)
        return;

    SetSuppressLinkErrorMessage(TRUE);

    for (i = 0; i < 4 && GetLinkRecvQueueLength() > 2; i++)
    {
        gLinkTransferringData = TRUE;
        HandleLinkConnection();
        gLinkTransferringData = FALSE;
    }
}

// Chamado pela funcao de colisao entre objetos (event_object_movement.c).
// Retorna TRUE se "mover" pode atravessar "other".
bool8 Coop_ObjectsPassThrough(struct ObjectEvent *mover, struct ObjectEvent *other)
{
    if (sCoopState != COOP_ACTIVE)
        return FALSE;

    // Avatares remotos nunca ficam barrados: so repetem o que o outro jogador fez.
    if (mover->localId >= COOP_LOCAL_ID_BASE && mover->localId < COOP_LOCAL_ID_BASE + MAX_LINK_PLAYERS)
        return TRUE;

    // O jogador local atravessa um avatar depois de empurrar contra ele.
    if (other->localId >= COOP_LOCAL_ID_BASE && other->localId < COOP_LOCAL_ID_BASE + MAX_LINK_PLAYERS && mover == &gObjectEvents[gPlayerAvatar.objectEventId])
        return sPassThrough[other->localId - COOP_LOCAL_ID_BASE];

    return FALSE;
}

// TRUE quando o personagem local esta escondido por uma transicao (porta, tapete, warp).
static bool8 Coop_LocalPlayerHidden(void)
{
    struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];

    return player->invisible || gSprites[player->spriteId].invisible;
}

// Chamado no INICIO de cada frame do OverworldBasic (overworld.c), antes do RunTasks.
// Nesse ponto mapa, posicao e direcao estao sempre coerentes. Durante um warp o mapa
// novo ja esta gravado enquanto a posicao ainda e a antiga; por isso o pacote usa esta
// copia, e nunca os valores "ao vivo".
void Coop_SnapshotLocal(void)
{
    struct ObjectEvent *obj = &gObjectEvents[gPlayerAvatar.objectEventId];

    sLocalPos[0] = (u16)obj->currentCoords.x;
    sLocalPos[1] = (u16)obj->currentCoords.y;
    sLocalPos[2] = gSaveBlock1Ptr->location.mapGroup | (gSaveBlock1Ptr->location.mapNum << 8);
    sLocalPos[3] = obj->facingDirection | (gPlayerAvatar.flags << 8);
    sLocalPos[4] = obj->currentElevation;
    sLocalPos[5] = Coop_LocalPlayerHidden();
}

// Monta o pacote de posicao do jogador local em out[0..COOP_POS_WORDS-1].
// Chamado pelo BuildSendCmd (link.c). So envia quando algo mudou ou como
// batimento a cada COOP_HEARTBEAT_FRAMES; retorna FALSE se nao ha nada a enviar.
bool8 Coop_BuildPosCmd(u16 *out)
{
    u8 i;
    bool8 changed = FALSE;

    for (i = 0; i < COOP_POS_WORDS; i++)
    {
        if (sLocalPos[i] != sLastSentPos[i])
            changed = TRUE;
    }

    if (!changed && sSendIdle < COOP_HEARTBEAT_FRAMES)
    {
        sSendIdle++;
        return FALSE;
    }

    for (i = 0; i < COOP_POS_WORDS; i++)
    {
        out[i] = sLocalPos[i];
        sLastSentPos[i] = sLocalPos[i];
    }
    sSendIdle = 0;
    return TRUE;
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
    remote->hidden = in[5] & 1;
    remote->packetCount++;
}

// Antistuck: libera a passagem entre o jogador local e o avatar "id".
static void Coop_UpdatePush(u8 id, struct ObjectEvent *avatar)
{
    struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];
    s16 dx = avatar->currentCoords.x - player->currentCoords.x;
    s16 dy = avatar->currentCoords.y - player->currentCoords.y;
    bool8 pushing;

    // Atravessando: continua ate se afastarem.
    if (sPassThrough[id])
    {
        if (abs(dx) + abs(dy) >= 2)
            sPassThrough[id] = FALSE;
        sPushFrames[id] = 0;
        return;
    }

    // No mesmo tile: libera para poder continuar andando.
    if (dx == 0 && dy == 0)
    {
        sPassThrough[id] = TRUE;
        return;
    }

    // Segurando o direcional em direcao ao avatar colado em voce.
    pushing = (dx == 0 && dy == -1 && (gMain.heldKeys & DPAD_UP)) || (dx == 0 && dy == 1 && (gMain.heldKeys & DPAD_DOWN)) || (dy == 0 && dx == -1 && (gMain.heldKeys & DPAD_LEFT)) || (dy == 0 && dx == 1 && (gMain.heldKeys & DPAD_RIGHT));

    if (pushing)
    {
        if (++sPushFrames[id] >= COOP_PUSH_FRAMES)
        {
            sPassThrough[id] = TRUE;
            sPushFrames[id] = 0;
        }
    }
    else
    {
        sPushFrames[id] = 0;
    }
}

static bool8 Coop_IsDoorTile(s16 x, s16 y)
{
    return MetatileBehavior_IsWarpDoor(MapGridGetMetatileBehaviorAt(x, y));
}

static void Coop_OpenDoor(u8 id, s16 x, s16 y)
{
    sDoorX[id] = x;
    sDoorY[id] = y;
    PlaySE(GetDoorSoundEffect(x, y));
    FieldAnimateDoorOpen(x, y);
}

static bool8 Coop_IsAdjacent(s16 dx, s16 dy)
{
    return (dx == 0 && (dy == 1 || dy == -1)) || (dy == 0 && (dx == 1 || dx == -1));
}

static u8 Coop_DirectionTo(s16 dx, s16 dy)
{
    if (dx > 0)
        return DIR_EAST;
    if (dx < 0)
        return DIR_WEST;
    if (dy > 0)
        return DIR_SOUTH;
    return DIR_NORTH;
}

static void Coop_AvatarWalk(struct ObjectEvent *obj, struct CoopRemotePlayer *remote, u8 dir)
{
    if (remote->avatarFlags & PLAYER_AVATAR_FLAG_DASH)
        ObjectEventSetHeldMovement(obj, GetWalkFastMovementAction(dir));
    else
        ObjectEventSetHeldMovement(obj, GetWalkNormalMovementAction(dir));
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

    if (id == GetMultiplayerId() || remote->packetCount == 0)
        return;

    objId = GetObjectEventIdByLocalIdAndMap(localId, mapNum, mapGroup);

    // Jogador remoto em outro mapa: sem avatar aqui.
    if (remote->mapGroup != mapGroup || remote->mapNum != mapNum)
    {
        if (objId < OBJECT_EVENTS_COUNT)
        {
            if (sDoorState[id] != DOOR_NONE)
                FieldSetDoorClosed(sDoorX[id], sDoorY[id]);
            RemoveObjectEventByLocalIdAndMap(localId, mapNum, mapGroup);
        }
        sDoorState[id] = DOOR_NONE;
        sPassThrough[id] = FALSE;
        sPushFrames[id] = 0;
        return;
    }

    // Cria o avatar se ainda nao existe neste mapa.
    if (objId >= OBJECT_EVENTS_COUNT)
    {
        sDoorState[id] = DOOR_NONE;

        // Nao cria durante fade, nem enquanto o seu personagem ou o do outro
        // estiverem escondidos por uma transicao (porta, tapete, warp).
        if (gPaletteFade.active || remote->hidden || Coop_LocalPlayerHidden())
            return;

        objId = SpawnSpecialObjectEventParameterized(
            GetRivalAvatarGraphicsIdByStateIdAndGender(PLAYER_AVATAR_STATE_NORMAL, gLinkPlayers[id].gender),
            MOVEMENT_TYPE_NONE,
            localId,
            remote->x,
            remote->y,
            remote->elevation);
        if (objId >= OBJECT_EVENTS_COUNT)
            return;

        // Saiu de um predio por uma porta animada: abre a porta antes de aparecer.
        if (Coop_IsDoorTile(remote->x, remote->y))
        {
            gObjectEvents[objId].invisible = TRUE;
            Coop_OpenDoor(id, remote->x, remote->y);
            sDoorState[id] = DOOR_OPEN_THEN_APPEAR;
        }
    }

    obj = &gObjectEvents[objId];

    // Visivel so quando o outro jogador esta visivel.
    if (sDoorState[id] == DOOR_NONE || sDoorState[id] == DOOR_WAIT_LEAVE || sDoorState[id] == DOOR_CLOSING_EXIT)
        obj->invisible = remote->hidden;

    Coop_UpdatePush(id, obj);

    // Ainda terminando um movimento: espera.
    if (ObjectEventClearHeldMovementIfFinished(obj) == 0)
        return;

    switch (sDoorState[id])
    {
    case DOOR_OPEN_THEN_APPEAR:
        if (FieldIsDoorAnimationRunning())
            return;
        obj->invisible = remote->hidden;
        sDoorState[id] = DOOR_WAIT_LEAVE;
        break;
    case DOOR_WAIT_LEAVE:
        if (obj->currentCoords.x != sDoorX[id] || obj->currentCoords.y != sDoorY[id])
        {
            FieldAnimateDoorClose(sDoorX[id], sDoorY[id]);
            sDoorState[id] = DOOR_CLOSING_EXIT;
        }
        break;
    case DOOR_CLOSING_EXIT:
    case DOOR_CLOSING_ENTER:
        if (FieldIsDoorAnimationRunning())
            return;
        sDoorState[id] = DOOR_NONE;
        break;
    case DOOR_OPEN_THEN_ENTER:
        if (FieldIsDoorAnimationRunning())
            return;
        dx = sDoorX[id] - obj->currentCoords.x;
        dy = sDoorY[id] - obj->currentCoords.y;
        if (Coop_IsAdjacent(dx, dy))
        {
            Coop_AvatarWalk(obj, remote, Coop_DirectionTo(dx, dy));
            sDoorState[id] = DOOR_WALKING_IN;
        }
        else
        {
            // Longe demais: desiste da animacao e fecha a porta.
            FieldAnimateDoorClose(sDoorX[id], sDoorY[id]);
            sDoorState[id] = DOOR_CLOSING_EXIT;
        }
        return;
    case DOOR_WALKING_IN:
        // Parou de andar: chegou na porta (ou nao conseguiu).
        if (obj->currentCoords.x == sDoorX[id] && obj->currentCoords.y == sDoorY[id])
        {
            obj->invisible = TRUE;
            sDoorState[id] = DOOR_CLOSING_ENTER;
        }
        else
        {
            sDoorState[id] = DOOR_CLOSING_EXIT;
        }
        FieldAnimateDoorClose(sDoorX[id], sDoorY[id]);
        return;
    }

    dx = remote->x - obj->currentCoords.x;
    dy = remote->y - obj->currentCoords.y;

    if (dx == 0 && dy == 0)
    {
        // Parado no lugar certo: so ajusta para onde esta olhando.
        if (obj->facingDirection != remote->facing)
            ObjectEventSetHeldMovement(obj, GetFaceDirectionMovementAction(remote->facing));
    }
    else if (Coop_IsAdjacent(dx, dy))
    {
        if (sDoorState[id] == DOOR_NONE && Coop_IsDoorTile(remote->x, remote->y))
        {
            // Vai entrar numa porta animada: abre antes de andar.
            Coop_OpenDoor(id, remote->x, remote->y);
            sDoorState[id] = DOOR_OPEN_THEN_ENTER;
        }
        else
        {
            // Um tile de distancia: anda (ou corre) ate la.
            Coop_AvatarWalk(obj, remote, Coop_DirectionTo(dx, dy));
        }
    }
    else
    {
        // Longe demais (spawn, warp, atraso): teletransporta.
        MoveObjectEventToMapCoords(obj, remote->x, remote->y);
    }
}

// Chamado todo frame pelo OverworldBasic.
// Parado: espera um pouco e comeca a conectar sozinho.
// Ativo: atualiza os avatares, religa o envio de posicao quando o link fica ocioso
// (o SendBlock e o standby terminam com gLinkCallback = NULL) e reconecta se o link cair.
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
    case COOP_CONNECTING:
        // Um warp (ResetTasks) pode ter destruido a task de conexao.
        if (!FuncIsActiveTask(Task_CoopConnect))
        {
            Coop_Log("task-lost", 0);
            CloseLink();
            sCoopState = COOP_IDLE;
            sAutoDelay = 0;
        }
        break;
    case COOP_ACTIVE:
        if (!gReceivedRemoteLinkPlayers)
        {
            // O link caiu (por exemplo durante um warp): reconecta sozinho.
            Coop_Log("link-lost", 0);
            sCoopState = COOP_IDLE;
            sAutoDelay = COOP_AUTOSTART_DELAY - 30;
            break;
        }
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
        SetSuppressLinkErrorMessage(TRUE); // sem tela de erro: o coop reconecta sozinho
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
            sSendIdle = COOP_HEARTBEAT_FRAMES; // forca o envio imediato da posicao
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