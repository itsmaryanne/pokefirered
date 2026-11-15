#include "global.h"
#include "coop.h"
#include "link.h"
#include "main.h"
#include "sound.h"
#include "task.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "constants/songs.h"

// Conecta sozinho ao entrar no overworld (sem atalho de teclas).
// Mude para 0 para jogar normalmente, sem tentar abrir o link.
#define COOP_AUTOSTART 1
#define COOP_AUTOSTART_DELAY 120   // frames de espera depois que o overworld comeca
#define COOP_RETRY_DELAY 300       // frames extras de espera depois de uma tentativa que falhou

// Quantos consoles esperamos para a conexao (por enquanto so 2).
#define COOP_PLAYERS_WANTED 2

// Tempo maximo de cada tentativa de conexao, em frames (~25 s).
#define COOP_CONNECT_TIMEOUT 1500

// Toca um "pin" quando o OUTRO jogador da um passo (teste sem avatares).
#define COOP_DEBUG_SOUND 1

// Escreve mensagens no log do mGBA (Tools > View logs).
#define COOP_DEBUG_LOG 1

enum
{
    COOP_IDLE,
    COOP_CONNECTING,
    COOP_ACTIVE,
};

#define tState    data[0]
#define tTimer    data[1]
#define tStable   data[2]
#define tWait     data[3]
#define tSentReq  data[4]

extern void BuildSendCmd(u16 command);

EWRAM_DATA struct CoopRemotePlayer gCoopRemote[MAX_LINK_PLAYERS] = {};
static EWRAM_DATA u8 sCoopState = COOP_IDLE;
static EWRAM_DATA s16 sAutoDelay = 0;
static EWRAM_DATA u16 sActiveTimer = 0;

static void Task_CoopConnect(u8 taskId);
static void Coop_CancelConnect(u8 taskId);

#if COOP_DEBUG_LOG
// Protocolo de depuracao do mGBA: habilita escrevendo 0xC0DE em 0x04FFF780,
// escreve o texto em 0x04FFF600 e envia com (nivel | 0x100) em 0x04FFF700.
// Em outros emuladores ou no hardware, nada acontece.
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

// Chamado todo frame pelo OverworldBasic.
// Parado: espera um pouco e comeca a conectar sozinho.
// Ativo: religa o envio de posicao quando o link fica ocioso
// (o SendBlock e o standby terminam com gLinkCallback = NULL).
void Coop_OverworldTick(void)
{
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
        if (++sActiveTimer >= 180)
        {
            sActiveTimer = 0;
            Coop_Log("pkts-p0", gCoopRemote[0].packetCount);
            Coop_Log("pkts-p1", gCoopRemote[1].packetCount);
        }
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
        if (tTimer % 90 == 0)
            Coop_Log("wait-cnt", GetLinkPlayerCount_2());
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
            sActiveTimer = 0;
            PlaySE(SE_PIN);
            DestroyTask(taskId);
            break;
        }
        tWait++;
        if (tWait % 90 == 0)
            Coop_Log("wait-recv", gReceivedRemoteLinkPlayers);
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
