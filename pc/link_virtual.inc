/* ===== PC: cabo link virtual (coop), acrescentado em CÓPIA do link.c ===== */
#include "../../pc/coop_net.h"

#define PCVL_HELLO   0x10
#define PCVL_DATA    0x11
#define PCVL_STANDBY 0x12
#define PCVL_CLOSE   0x13
#define PCVL_MAX     4096
#define PCVL_HELLO_SIZE (1 + sizeof(struct LinkPlayer) + 3)

static int sVLActive, sVLMyId, sVLIHello, sVLSeen;
static int sVLWaitStandby, sVLWaitClose, sVLStandbySeen, sVLCloseSeen, sVLHelloAge;
static int sVLMode;                          /* 0 = PvP 1x1; 1 = dois jogadores contra um treinador NPC */
static unsigned sVLTrainer;
static int sVLStartBlocks;
static unsigned sVLMonSize;
static uint8_t sVLTrainerBytes[6 * 160];

static int VLRealMask(void) { return sVLMode ? 5 : 3; }   /* jogadores reais: ids 0 e 1, ou ids 0 e 2 */

int PcVLActive(void) { return sVLActive; }
int PcVLMyId(void) { return sVLMyId; }
int PcVLPlayers(void) { return sVLMode ? 4 : 2; }

/* Modo treinador: o host fabrica os blocos dos dois "jogadores" que são o treinador NPC (ids 1 e 3). */
static void PcVLPhantoms(int k, const void *src, unsigned size)
{
    static uint8_t pkt[1 + PCVL_MAX];

    pkt[0] = PCVL_DATA;
    if (k == 0)
    {
        sVLMonSize = (unsigned)PcCoopBuildTrainerSlots(sVLTrainer, sVLTrainerBytes, sizeof sVLTrainerBytes);
        memcpy(pkt + 1, src, size);                  /* cabeçalho de abertura: igual ao do host */
        PcLinkSubmitAs(3, pkt, size + 1);
        PcLinkSubmitAs(1, pkt, size + 1);
        PcLog("[pc] link: fantasmas do treinador %u: cabecalho enviado (pokemon de %u bytes)\n", sVLTrainer, sVLMonSize);
    }
    else if (sVLMonSize != 0)
    {
        unsigned bytes = (k == 1) ? 2 * sVLMonSize : sVLMonSize;
        unsigned off3 = (k == 1) ? 0 : 2 * sVLMonSize;                 /* id 3: slots 0,1 e depois 2 */
        unsigned off1 = (k == 1) ? 3 * sVLMonSize : 5 * sVLMonSize;    /* id 1: slots 3,4 e depois 5 */

        if (size != bytes)
            PcLog("[pc] link: AVISO bloco de abertura %d com %u bytes (esperava %u)\n", k, size, bytes);
        memcpy(pkt + 1, sVLTrainerBytes + off3, bytes);
        PcLinkSubmitAs(3, pkt, bytes + 1);
        memcpy(pkt + 1, sVLTrainerBytes + off1, bytes);
        PcLinkSubmitAs(1, pkt, bytes + 1);
    }
}

int PcVLSend(const void *src, unsigned size)
{
    static uint8_t pkt[1 + PCVL_MAX];
    int ok;

    if (size > PCVL_MAX)
        return 0;
    pkt[0] = PCVL_DATA;
    memcpy(pkt + 1, src, size);
    ok = PcLinkSubmit(pkt, size + 1);
    if (ok && sVLMode == 1 && PcLinkIsHost() && sVLStartBlocks < 3)
    {
        PcVLPhantoms(sVLStartBlocks, src, size);
        sVLStartBlocks++;
    }
    return ok;
}

int PcVLTaskFinished(void)
{
    return !sVLWaitStandby && !sVLWaitClose && PcLinkSubmitPending() == 0;
}

int PcVLRecvStatus(void)
{
    return (gBlockReceivedStatus[3] << 3) | (gBlockReceivedStatus[2] << 2) | (gBlockReceivedStatus[1] << 1) | gBlockReceivedStatus[0];
}

void PcVLResetFlags(void)
{
    int i;

    for (i = 0; i < MAX_LINK_PLAYERS; i++)
        gBlockReceivedStatus[i] = FALSE;
}

void PcVLResetFlag(int who)
{
    gBlockReceivedStatus[who] = FALSE;
}

static void PcVLSubmitSimple(uint8_t kind)
{
    PcLinkSubmit(&kind, 1);
}

void PcVLStandby(void)
{
    sVLWaitStandby = 1;
    PcVLSubmitSimple(PCVL_STANDBY);
}

void PcVLClose(void)
{
    sVLWaitClose = 1;
    PcVLSubmitSimple(PCVL_CLOSE);
}

static void PcVLSubmitHello(void)
{
    uint8_t pkt[PCVL_HELLO_SIZE];

    gLinkType = LINKTYPE_BATTLE;
    InitLocalLinkPlayer();
    gLocalLinkPlayer.id = PcLinkIsHost() ? 0 : (sVLMode ? 2 : 1);
    gLocalLinkPlayer.linkType = LINKTYPE_BATTLE;
    PcLinkSetMyId(gLocalLinkPlayer.id);
    pkt[0] = PCVL_HELLO;
    memcpy(pkt + 1, &gLocalLinkPlayer, sizeof(struct LinkPlayer));
    pkt[1 + sizeof(struct LinkPlayer)] = (uint8_t)sVLMode;
    pkt[2 + sizeof(struct LinkPlayer)] = (uint8_t)(sVLTrainer & 0xFF);
    pkt[3 + sizeof(struct LinkPlayer)] = (uint8_t)(sVLTrainer >> 8);
    PcLinkSubmit(pkt, sizeof pkt);
    sVLIHello = 1;
}

static void PcVLBegin(void)
{
    sVLActive = 1;
    sVLStartBlocks = 0;
    sVLMyId = PcLinkIsHost() ? 0 : (sVLMode ? 2 : 1);
    if (sVLMode)
    {
        int i;

        for (i = 1; i <= 3; i += 2)                  /* ids 1 e 3: o treinador NPC */
        {
            gLinkPlayers[i] = gLinkPlayers[0];
            gLinkPlayers[i].id = i;
            gLinkPlayers[i].linkType = LINKTYPE_BATTLE;
            PcCoopTrainerName(sVLTrainer, gLinkPlayers[i].name, PLAYER_NAME_LENGTH + 1);
        }
    }
    gReceivedRemoteLinkPlayers = TRUE;
    SetLocalLinkPlayerId(sVLMyId);
    PcLog("[pc] link: comecando a batalha virtual (eu sou o jogador %d, modo %d)\n", sVLMyId, sVLMode);
    if (sVLMode)
        PcCoopStartCoopTrainerBattle(sVLTrainer);
    else
        PcCoopStartVirtualBattle();
}

static void VLTrigger(void)
{
    if (sVLActive || sVLIHello || !PcLinkConnected())
        return;
    PcVLSubmitHello();
}

void PcVLTrigger(void)
{
    sVLMode = 0;
    VLTrigger();
}

void PcVLTriggerTrainer(unsigned trainerId)
{
    sVLMode = 1;
    sVLTrainer = trainerId;
    VLTrigger();
}

/* Chamada todo quadro: entrega, na ordem do host, o que chegou pela rede. */
void PcVLPoll(void)
{
    static uint8_t buf[4200];
    static uint8_t sender;
    static unsigned size;
    static int stashed;
    int n;

    if (sVLIHello && !sVLActive)
    {
        if (++sVLHelloAge > 600)                     /* ~10 s sem resposta: desiste, para poder tentar de novo */
        {
            sVLHelloAge = 0;
            sVLIHello = 0;
            sVLSeen = 0;
            PcLog("[pc] link: handshake sem resposta, resetado (tente de novo com os dois livres no mapa)\n");
        }
    }
    else
        sVLHelloAge = 0;

    for (n = 0; n < 8; n++)
    {
        if (!stashed)
        {
            if (!PcLinkPopDelivered(&sender, buf, &size))
                return;
            stashed = 1;
        }
        if (size >= 1 && sender < MAX_LINK_PLAYERS)
        {
            switch (buf[0])
            {
            case PCVL_DATA:
                if (sVLActive)
                {
                    unsigned bytes = size - 1;

                    if (gBlockReceivedStatus[sender])
                        return;                      /* o jogo ainda não leu o bloco anterior: mantém a ordem e espera */
                    if (bytes > BLOCK_BUFFER_SIZE)
                        bytes = BLOCK_BUFFER_SIZE;
                    memcpy(gBlockRecvBuffer[sender], buf + 1, bytes);
                    gBlockReceivedStatus[sender] = TRUE;
                }
                break;
            case PCVL_HELLO:
                if (!sVLActive && size == PCVL_HELLO_SIZE && PcCoopCanLink())
                {
                    if (sender == 0)
                    {
                        sVLMode = buf[1 + sizeof(struct LinkPlayer)];
                        sVLTrainer = buf[2 + sizeof(struct LinkPlayer)] | (buf[3 + sizeof(struct LinkPlayer)] << 8);
                    }
                    memcpy(&gLinkPlayers[sender], buf + 1, sizeof(struct LinkPlayer));
                    gLinkPlayers[sender].id = sender;
                    gLinkPlayers[sender].linkType = LINKTYPE_BATTLE;
                    sVLSeen |= 1 << sender;
                    PcLog("[pc] link: HELLO do jogador %d recebido (vistos=%d, modo=%d)\n", sender, sVLSeen, sVLMode);
                    if (!sVLIHello)
                        PcVLSubmitHello();
                    if (sVLSeen == VLRealMask())
                    {
                        sVLSeen = 0;
                        PcVLBegin();
                    }
                }
                else
                    PcLog("[pc] link: HELLO do jogador %d IGNORADO (ativo=%d, livre no overworld=%d)\n", sender, sVLActive, PcCoopCanLink());
                break;
            case PCVL_STANDBY:
                sVLStandbySeen |= 1 << sender;
                if (sVLStandbySeen == VLRealMask())
                {
                    sVLStandbySeen = 0;
                    sVLWaitStandby = 0;
                }
                break;
            case PCVL_CLOSE:
                sVLCloseSeen |= 1 << sender;
                if (sVLCloseSeen == VLRealMask())
                {
                    sVLCloseSeen = 0;
                    sVLWaitClose = 0;
                    sVLActive = 0;
                    sVLIHello = 0;
                    sVLSeen = 0;
                    sVLMode = 0;
                    sVLStartBlocks = 0;
                    gReceivedRemoteLinkPlayers = FALSE;
                    PcLog("[pc] link: batalha virtual encerrada\n");
                }
                break;
            }
        }
        stashed = 0;
    }
}
