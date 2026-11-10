// Coop PC, fase 1: cada jogador roda o PROPRIO jogo; o outro aparece como um object event "fantasma".
// Rede em pc/coop_net.c (PC_COOP_HOST=porta no host, PC_COOP_JOIN=ip:porta no convidado).
#include "global.h"
#include <string.h>
#include "main.h"
#include "overworld.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "pc_compat.h"
#include "coop_net.h"
#include "constants/event_object_movement.h"
#include "constants/event_objects.h"
#include "constants/flags.h"
#include "event_data.h"
#include "pokemon.h"
#include "battle.h"
#include "constants/trainers.h"
#include "script.h"
#include "constants/vars.h"

#include "characters.h"
extern char *getenv(const char *name);   /* sem stdlib.h: ele conflita com o abs() do jogo */

#define PC_COOP_LOCAL_ID 0xFE

static u8 sGhostObjectId = OBJECT_EVENTS_COUNT;
static u8 sGhostMapNum;
static u8 sGhostMapGroup;

static int GhostValid(void)
{
    struct ObjectEvent *o;
    if (sGhostObjectId >= OBJECT_EVENTS_COUNT)
        return 0;
    o = &gObjectEvents[sGhostObjectId];
    return o->active && o->localId == PC_COOP_LOCAL_ID;
}

static void RemoveGhost(void)
{
    if (GhostValid())
        RemoveObjectEventByLocalIdAndMap(PC_COOP_LOCAL_ID, sGhostMapNum, sGhostMapGroup);
    sGhostObjectId = OBJECT_EVENTS_COUNT;
}

/* ---------- progresso compartilhado (fase 3) ---------- */
#ifdef TEMP_FLAGS_END
#define SYNC_FLAG_FIRST (TEMP_FLAGS_END + 1)
#else
#define SYNC_FLAG_FIRST 0x20
#endif
#ifdef TRAINER_FLAGS_END
#define SYNC_FLAG_LAST TRAINER_FLAGS_END          /* depois dos treinadores vêm as flags de sistema (por jogador) */
#else
#define SYNC_FLAG_LAST 0x7FF
#endif
#if defined(TEMP_VARS_END) && defined(VARS_START)
#define SYNC_VAR_FIRST (TEMP_VARS_END - VARS_START + 1)
#else
#define SYNC_VAR_FIRST 16
#endif
#define PC_FLAG_BYTES (sizeof(((struct SaveBlock1 *)0)->flags))
#define PC_VAR_COUNT (sizeof(((struct SaveBlock1 *)0)->vars) / sizeof(u16))

static u8 sShadowFlags[PC_FLAG_BYTES];
static u16 sShadowVars[PC_VAR_COUNT];
static int sShadowReady;
static int sLogBudget = 300;
static u8 sVarMuted[PC_VAR_COUNT];
static u8 sVarChanges[PC_VAR_COUNT];
static u8 sFlagMuted[PC_FLAG_BYTES * 8];
static u8 sFlagChanges[PC_FLAG_BYTES * 8];
static unsigned sWindowTicks;
static int sFlagsApplied;

static int FlagSyncable(unsigned id)
{
    return id >= SYNC_FLAG_FIRST && id <= SYNC_FLAG_LAST && id < PC_FLAG_BYTES * 8;
}

static int VarSyncable(unsigned i)
{
    return i >= SYNC_VAR_FIRST && i < PC_VAR_COUNT;
}

static void QueueEv(u8 kind, unsigned index, unsigned value)
{
    struct PcCoopSyncEvent e;
    e.kind = kind;
    e.index = (uint16_t)index;
    e.value = (uint16_t)value;
    PcCoopNetSendEvent(&e);
}

static void SendEv(u8 kind, unsigned index, unsigned value)
{
    QueueEv(kind, index, value);
    if (sLogBudget > 0)
    {
        sLogBudget--;
        PcLog("[pc] coop: envia %s %u = %u\n", kind == 0 ? "flag" : "var", index, value);
    }
}

/* ---------- NPCs que se mexem: o host manda, o convidado espelha ---------- */
static u8 sNpcHas[OBJECT_EVENTS_COUNT];
static u8 sNpcLocal[OBJECT_EVENTS_COUNT];
static u8 sNpcFacing[OBJECT_EVENTS_COUNT];
static s16 sNpcX[OBJECT_EVENTS_COUNT];
static s16 sNpcY[OBJECT_EVENTS_COUNT];
static int sNpcLogBudget = 20;

static void ApplyNpc(const struct PcCoopSyncEvent *e)
{
    u8 group = e->index >> 8, num = e->index & 0xFF, localId = e->value >> 8;
    int n;
    static int sRecv;

    if (sRecv < 5)
    {
        sRecv++;
        PcLog("[pc] coop: convidado recebeu npc %u (mapa %u/%u, aqui %d/%d)\n", e->value >> 8, e->index >> 8, e->index & 0xFF,
              gSaveBlock1Ptr->location.mapGroup, gSaveBlock1Ptr->location.mapNum);
    }
    if (group != gSaveBlock1Ptr->location.mapGroup || num != gSaveBlock1Ptr->location.mapNum)
        return;
    for (n = 0; n < OBJECT_EVENTS_COUNT; n++)
    {
        struct ObjectEvent *o = &gObjectEvents[n];

        if (!o->active || o->localId != localId || o->mapNum != num || o->mapGroup != group)
            continue;
        if (n == gPlayerAvatar.objectEventId || localId == PC_COOP_LOCAL_ID)
            return;
        sNpcHas[n] = 1;
        sNpcLocal[n] = localId;
        sNpcFacing[n] = e->value & 0xFF;
        sNpcX[n] = e->a;
        sNpcY[n] = e->b;
        if (sNpcLogBudget > 0)
        {
            sNpcLogBudget--;
            PcLog("[pc] coop: npc %u vai para (%d,%d)\n", localId, (int)e->a, (int)e->b);
        }
        return;
    }
}

static void ApplyEv(const struct PcCoopSyncEvent *e)
{
    unsigned i = e->index;

    if (e->kind == 3)
    {
        ApplyNpc(e);
        return;
    }

    if (e->kind == 0)
    {
        if (!FlagSyncable(i) || sFlagMuted[i])
            return;
        if (((gSaveBlock1Ptr->flags[i >> 3] >> (i & 7)) & 1u) == (e->value ? 1u : 0u))
            return;                                   /* já está assim: nada a aplicar nem a registrar */
        sFlagsApplied = 1;
        if (e->value)
        {
            gSaveBlock1Ptr->flags[i >> 3] |= (u8)(1u << (i & 7));
            sShadowFlags[i >> 3] |= (u8)(1u << (i & 7));
        }
        else
        {
            gSaveBlock1Ptr->flags[i >> 3] &= (u8)~(1u << (i & 7));
            sShadowFlags[i >> 3] &= (u8)~(1u << (i & 7));
        }
    }
    else
    {
        if (!VarSyncable(i) || sVarMuted[i])
            return;
        if (e->kind == 1 && gSaveBlock1Ptr->vars[i] == e->value)
            return;
        if (e->kind == 2 && gSaveBlock1Ptr->vars[i] >= e->value)
            return;                                   /* mesclagem na conexão: fica o maior valor */
        gSaveBlock1Ptr->vars[i] = e->value;
        sShadowVars[i] = e->value;
    }
    if (sLogBudget > 0)
    {
        sLogBudget--;
        PcLog("[pc] coop: aplica %s %u = %u\n", e->kind == 0 ? "flag" : "var", i, e->value);
    }
}

/* Quando o outro jogador muda uma flag (treinador derrotado, item pego), os object events deste mapa que
   dependem dela precisam sumir agora; sozinho o jogo só reavalia isso ao recarregar o mapa. */
static void RefreshHiddenObjects(void)
{
    int n;

    for (n = 0; n < OBJECT_EVENT_TEMPLATES_COUNT; n++)
    {
        const struct ObjectEventTemplate *t = &gSaveBlock1Ptr->objectEventTemplates[n];

        if (t->localId == 0 || t->localId == PC_COOP_LOCAL_ID || t->flagId == 0)
            continue;
        if (FlagGet(t->flagId))
            RemoveObjectEventByLocalIdAndMap(t->localId, gSaveBlock1Ptr->location.mapNum, gSaveBlock1Ptr->location.mapGroup);
    }
}

static void SyncTick(void)
{
    static int sOff = -1;
    struct PcCoopSyncEvent e;
    unsigned i, b, applied = 0, nf = 0, nv = 0;

    if (sOff < 0)
        sOff = getenv("PC_COOP_NOSYNC") != NULL;
    if (sOff)
        return;
    if (!sShadowReady)
    {
        memcpy(sShadowFlags, gSaveBlock1Ptr->flags, sizeof sShadowFlags);
        memcpy(sShadowVars, gSaveBlock1Ptr->vars, sizeof sShadowVars);
        sShadowReady = 1;
        PcLog("[pc] coop: sync de flags %u..%u (de %u bits) e vars %u..%u\n",
              (unsigned)SYNC_FLAG_FIRST, (unsigned)SYNC_FLAG_LAST, (unsigned)(PC_FLAG_BYTES * 8),
              (unsigned)SYNC_VAR_FIRST, (unsigned)PC_VAR_COUNT - 1);
    }
    if (++sWindowTicks >= 600)                        /* janela de ~10 s para detectar contadores */
    {
        sWindowTicks = 0;
        memset(sVarChanges, 0, sizeof sVarChanges);
        memset(sFlagChanges, 0, sizeof sFlagChanges);
    }
    while (applied < 256 && PcCoopNetPollEvent(&e))
    {
        ApplyEv(&e);
        applied++;
    }
    if (sFlagsApplied)
    {
        sFlagsApplied = 0;
        RefreshHiddenObjects();
    }
    if (PcCoopNetJustConnected())
    {
        for (i = SYNC_FLAG_FIRST; i <= SYNC_FLAG_LAST && i < PC_FLAG_BYTES * 8; i++)
            if (gSaveBlock1Ptr->flags[i >> 3] & (1u << (i & 7)))
            {
                QueueEv(0, i, 1);
                nf++;
            }
        for (i = SYNC_VAR_FIRST; i < PC_VAR_COUNT; i++)
            if (gSaveBlock1Ptr->vars[i])
            {
                QueueEv(2, i, gSaveBlock1Ptr->vars[i]);
                nv++;
            }
        PcLog("[pc] coop: sincronizacao inicial enviada: %u flags, %u vars\n", nf, nv);
    }
    for (b = 0; b < PC_FLAG_BYTES; b++)
    {
        u8 cur = gSaveBlock1Ptr->flags[b];
        u8 diff = cur ^ sShadowFlags[b];

        if (!diff)
            continue;
        for (i = 0; i < 8; i++)
            if ((diff & (1u << i)) && FlagSyncable(b * 8 + i) && !sFlagMuted[b * 8 + i])
            {
                if (++sFlagChanges[b * 8 + i] >= 8)
                {
                    sFlagMuted[b * 8 + i] = 1;
                    PcLog("[pc] coop: flag %u muda toda hora: nao sincroniza mais\n", b * 8 + i);
                    continue;
                }
                SendEv(0, b * 8 + i, (cur >> i) & 1);
            }
        sShadowFlags[b] = cur;
    }
    for (i = 0; i < PC_VAR_COUNT; i++)
    {
        u16 cur = gSaveBlock1Ptr->vars[i];

        if (cur == sShadowVars[i])
            continue;
        sShadowVars[i] = cur;
        if (!VarSyncable(i) || sVarMuted[i])
            continue;
        if (++sVarChanges[i] >= 8)
        {
            sVarMuted[i] = 1;
            PcLog("[pc] coop: var %u muda toda hora (contador de passos?): nao sincroniza mais\n", i);
            continue;
        }
        SendEv(1, i, cur);
    }
}

static int NpcIsHost(void)
{
    static int v = -1;

    if (v < 0)
        v = getenv("PC_COOP_HOST") != NULL;
    return v;
}

static int NpcOff(void)
{
    static int v = -1;

    if (v < 0)
        v = getenv("PC_COOP_NONPC") != NULL;
    return v;
}

static u32 sNpcLast[OBJECT_EVENTS_COUNT];
static u32 sNpcFirst[OBJECT_EVENTS_COUNT];
static u8 sNpcMoved[OBJECT_EVENTS_COUNT];
static unsigned sNpcRefresh;

static void NpcHostTick(void)
{
    struct PcCoopPlayerState remote;
    int n, full;

    if (!PcCoopNetGetRemote(&remote)
     || remote.mapGroup != gSaveBlock1Ptr->location.mapGroup || remote.mapNum != gSaveBlock1Ptr->location.mapNum)
    {
        static int sOnce;

        if (!sOnce)
        {
            sOnce = 1;
            PcLog("[pc] coop: host nao envia npcs: o outro nao esta conectado/valido ou esta em outro mapa (local %d/%d)\n",
                  gSaveBlock1Ptr->location.mapGroup, gSaveBlock1Ptr->location.mapNum);
        }
        return;
    }
    full = (++sNpcRefresh % 180) == 0;                /* reenvia tudo a cada ~3 s */
    for (n = 0; n < OBJECT_EVENTS_COUNT; n++)
    {
        struct ObjectEvent *o = &gObjectEvents[n];
        u32 pack;
        struct PcCoopSyncEvent e;

        if (!o->active || n == gPlayerAvatar.objectEventId || o->localId == PC_COOP_LOCAL_ID)
            continue;
        pack = ((u32)o->currentCoords.x & 0x3FF) | (((u32)o->currentCoords.y & 0x3FF) << 10)
             | ((u32)(o->facingDirection & 7) << 20) | 0x80000000u;
        {
            u32 pos = ((u32)o->currentCoords.x & 0x3FF) | (((u32)o->currentCoords.y & 0x3FF) << 10);

            if (sNpcFirst[n] == 0)
            {
                sNpcFirst[n] = pos | 0x80000000u;      /* primeira vez que vemos este objeto */
                continue;
            }
            if (!sNpcMoved[n])
            {
                if (pos == (sNpcFirst[n] & 0x7FFFFFFFu))
                    continue;                           /* nunca saiu do lugar: não é um NPC que anda, não mexemos nele */
                sNpcMoved[n] = 1;
            }
        }
        if (pack == sNpcLast[n] && !full)
            continue;
        sNpcLast[n] = pack;
        e.kind = 3;
        e.index = (gSaveBlock1Ptr->location.mapGroup << 8) | gSaveBlock1Ptr->location.mapNum;
        e.value = (o->localId << 8) | (o->facingDirection & 7);
        e.a = o->currentCoords.x;
        e.b = o->currentCoords.y;
        {
            static int sSent;

            if (sSent < 12)
            {
                sSent++;
                PcLog("[pc] coop: host envia npc %u em (%d,%d)\n", o->localId, (int)e.a, (int)e.b);
            }
        }
        PcCoopNetSendEvent(&e);
    }
}

static void NpcGuestTick(void)
{
    int n;
    int busy = ScriptContext_IsEnabled() ? 1 : 0;   /* um script (cena, conversa) está rodando aqui: o NPC é dele */

    for (n = 0; n < OBJECT_EVENTS_COUNT; n++)
    {
        struct ObjectEvent *o = &gObjectEvents[n];
        int dx, dy;

        if (!sNpcHas[n])
            continue;
        if (o->inanimate)
            continue;
        if (busy)
        {
            o->frozen = FALSE;
            continue;
        }
        if (!o->active || o->localId != sNpcLocal[n] || n == gPlayerAvatar.objectEventId)
        {
            sNpcHas[n] = 0;
            continue;
        }
        o->frozen = TRUE;                             /* a IA deste NPC fica parada: só o host decide para onde ele vai */
        if (o->heldMovementActive)
        {
            if (!o->heldMovementFinished)
                continue;
            ObjectEventClearHeldMovement(o);
        }
        dx = sNpcX[n] - o->currentCoords.x;
        dy = sNpcY[n] - o->currentCoords.y;
        if (dx == 0 && dy == 0)
        {
            if (sNpcFacing[n] >= DIR_SOUTH && sNpcFacing[n] <= DIR_EAST && o->facingDirection != sNpcFacing[n])
                ObjectEventTurn(o, sNpcFacing[n]);
        }
        else if (dx * dx + dy * dy == 1)
        {
            u8 dir = dx > 0 ? DIR_EAST : dx < 0 ? DIR_WEST : dy > 0 ? DIR_SOUTH : DIR_NORTH;

            ObjectEventSetHeldMovement(o, GetWalkNormalMovementAction(dir));
        }
        else
        {
            MoveObjectEventToMapCoords(o, sNpcX[n], sNpcY[n]);   /* longe demais: teleporta */
        }
    }
}

static void NpcTick(void)
{
    static u8 sMapGroup = 0xFF, sMapNum = 0xFF;

    if (NpcOff())
        return;
    if (sMapGroup != gSaveBlock1Ptr->location.mapGroup || sMapNum != gSaveBlock1Ptr->location.mapNum)
    {
        sMapGroup = gSaveBlock1Ptr->location.mapGroup;
        sMapNum = gSaveBlock1Ptr->location.mapNum;
        memset(sNpcLast, 0, sizeof sNpcLast);
        memset(sNpcFirst, 0, sizeof sNpcFirst);
        memset(sNpcMoved, 0, sizeof sNpcMoved);
        memset(sNpcHas, 0, sizeof sNpcHas);
    }
    if (NpcIsHost())
        NpcHostTick();
    else
        NpcGuestTick();
}

/* ---------- treinadores só lutam se o outro jogador está perto ---------- */
extern u8 CreateNPCTrainerParty(struct Pokemon *party, u16 trainerNum);
extern int atoi(const char *nptr);

int PcCoopTrainerMayEngage(void)
{
    static int sNear = -1;
    static int sLogBudget2 = 10;
    struct PcCoopPlayerState remote;
    struct ObjectEvent *player;
    int dx, dy, d, ok;

    if (!PcCoopNetActive() || !PcCoopNetGetRemote(&remote))
        return 1;                                     /* solo (ou o outro desconectou): tudo normal */
    if (gPlayerAvatar.objectEventId >= OBJECT_EVENTS_COUNT || !gSaveBlock1Ptr)
        return 1;
    player = &gObjectEvents[gPlayerAvatar.objectEventId];
    if (sNear < 0)
    {
        const char *e = getenv("PC_COOP_NEAR");

        sNear = e ? atoi(e) : 6;
    }
    if (remote.mapGroup != gSaveBlock1Ptr->location.mapGroup || remote.mapNum != gSaveBlock1Ptr->location.mapNum)
        ok = 0;
    else
    {
        dx = remote.x - player->currentCoords.x;
        dy = remote.y - player->currentCoords.y;
        if (dx < 0)
            dx = -dx;
        if (dy < 0)
            dy = -dy;
        d = dx > dy ? dx : dy;
        ok = d <= sNear;
    }
    if (!ok && sLogBudget2 > 0)
    {
        sLogBudget2--;
        PcLog("[pc] coop: treinador nao enxergou o jogador (o outro esta longe)\n");
    }
    return ok;
}

/* ---------- cabo link virtual: PvP de teste e início da batalha ---------- */
extern int access(const char *path, int mode);
extern int unlink(const char *path);
extern void CB2_InitBattle(void);
extern void CB2_ReturnToField(void);
extern void CleanupOverworldWindowsAndTilemaps(void);
extern void LoadPlayerParty(void);
extern u16 gTrainerBattleOpponent_A;
#ifndef TRAINER_LINK_OPPONENT
#define TRAINER_LINK_OPPONENT 0x800
#endif

static int sOverworldSeen, sOverworldNow;

int PcCoopCanLink(void)
{
    return sOverworldNow && !gMain.inBattle;
}

static void CB2_ReturnFromVirtualBattle(void)
{
    gBattleTypeFlags &= ~BATTLE_TYPE_LINK_IN_BATTLE;
    LoadPlayerParty();                                 /* desfaz o dano da batalha, como o cabo original */
    SetMainCallback2(CB2_ReturnToField);
}

void PcCoopStartVirtualBattle(void)
{
    gBattleTypeFlags = BATTLE_TYPE_TRAINER | BATTLE_TYPE_LINK;
    CleanupOverworldWindowsAndTilemaps();
    gTrainerBattleOpponent_A = TRAINER_LINK_OPPONENT;
    SetMainCallback2(CB2_InitBattle);
    gMain.savedCallback = CB2_ReturnFromVirtualBattle;
    PcLog("[pc] coop: batalha PvP pelo cabo virtual iniciada\n");
}

/* ---------- treinador coop: dois jogadores contra um treinador NPC (batalha dupla multi) ---------- */
#ifndef TRAINERS_COUNT
#define TRAINERS_COUNT 200
#endif

static int sCoopTrainerMode;

int PcCoopTrainerMode(void)
{
    return sCoopTrainerMode;
}

void PcCoopTrainerName(unsigned trainerId, void *dst, unsigned len)
{
    u8 *d = dst;
    unsigned i;

    for (i = 0; i + 1 < len && gTrainers[trainerId].trainerName[i] != EOS; i++)
        d[i] = gTrainers[trainerId].trainerName[i];
    d[i] = EOS;
}

/* Monta os 6 "slots" do time do treinador como o modo multi os espera:
   id 3 recebe os slots 0-2 (mons 0,2,4 do treinador) e id 1 os slots 3-5 (mons 1,3,5). Os líderes são o slot 0 e o 3. */
int PcCoopBuildTrainerSlots(unsigned trainerId, void *out, unsigned outSize)
{
    static struct Pokemon tmp[PARTY_SIZE];
    static const u8 order[PARTY_SIZE] = { 0, 2, 4, 1, 3, 5 };
    unsigned mon = sizeof(struct Pokemon);
    unsigned i, n;

    if (outSize < mon * PARTY_SIZE)
        return 0;
    memset(tmp, 0, sizeof tmp);
    n = CreateNPCTrainerParty(tmp, trainerId);
    memset(out, 0, mon * PARTY_SIZE);
    for (i = 0; i < PARTY_SIZE; i++)
        if (order[i] < n)
            memcpy((u8 *)out + i * mon, &tmp[order[i]], mon);
    return (int)mon;
}

static unsigned PcCoopPickTrainer(void)
{
    const char *e = getenv("PC_COOP_TRAINER");
    unsigned id;

    if (e)
        return (unsigned)atoi(e);
    for (id = 1; id < TRAINERS_COUNT; id++)
        if (gTrainers[id].partySize >= 3 && gTrainers[id].doubleBattle != TRUE)
            return id;
    return 0;
}

static void CB2_ReturnFromCoopTrainerBattle(void)
{
    sCoopTrainerMode = 0;
    gBattleTypeFlags &= ~BATTLE_TYPE_LINK_IN_BATTLE;
    LoadPlayerParty();                                 /* nesta versão o time volta ao estado de antes */
    SetMainCallback2(CB2_ReturnToField);
}

void PcCoopStartCoopTrainerBattle(unsigned trainerId)
{
    sCoopTrainerMode = 1;
    gBattleTypeFlags = BATTLE_TYPE_TRAINER | BATTLE_TYPE_LINK | BATTLE_TYPE_DOUBLE | BATTLE_TYPE_MULTI;
    CleanupOverworldWindowsAndTilemaps();
    gTrainerBattleOpponent_A = trainerId;
    SetMainCallback2(CB2_InitBattle);
    gMain.savedCallback = CB2_ReturnFromCoopTrainerBattle;
    PcLog("[pc] coop: batalha coop contra o treinador %u iniciada\n", trainerId);
}

static void PcCoopTrainerTriggerCheck(void)
{
    static unsigned sT;
    static int sWarned;
    unsigned id;

    if (!getenv("PC_COOP_HOST") || (++sT & 15) != 0 || access("/tmp/pc_coop_trainer", 0) != 0)
        return;
    if (!sOverworldNow || gMain.inBattle)
    {
        if (!sWarned)
        {
            sWarned = 1;
            PcLog("[pc] coop: treinador coop pedido, mas este jogo nao esta livre no overworld; atendo quando estiver\n");
        }
        return;
    }
    sWarned = 0;
    unlink("/tmp/pc_coop_trainer");
    id = PcCoopPickTrainer();
    if (id == 0 || id >= TRAINERS_COUNT || gTrainers[id].partySize < 2)
    {
        PcLog("[pc] coop: nenhum treinador adequado (id %u); use PC_COOP_TRAINER=<id> com 2 ou mais pokemon\n", id);
        return;
    }
    PcLog("[pc] coop: treinador coop pedido: id %u (%d pokemon)\n", id, gTrainers[id].partySize);
    PcVLTriggerTrainer(id);
}

/* ---------- treinador coop: dois jogadores contra um treinador NPC (batalha dupla multi) ---------- */
extern u8 CreateNPCTrainerParty(struct Pokemon *party, u16 trainerNum);
extern const struct Trainer gTrainers[];
#ifndef TRAINERS_COUNT
#define TRAINERS_COUNT 200
#endif

static int sCoopTrainerMode;

int PcCoopTrainerMode(void)
{
    return sCoopTrainerMode;
}

void PcCoopTrainerName(unsigned trainerId, void *dst, unsigned len)
{
    u8 *d = dst;
    unsigned i;

    for (i = 0; i + 1 < len && gTrainers[trainerId].trainerName[i] != EOS; i++)
        d[i] = gTrainers[trainerId].trainerName[i];
    d[i] = EOS;
}

/* Monta os 6 "slots" do time do treinador como o modo multi os espera:
   id 3 recebe os slots 0-2 (mons 0,2,4 do treinador) e id 1 os slots 3-5 (mons 1,3,5). Os líderes são o slot 0 e o 3. */
int PcCoopBuildTrainerSlots(unsigned trainerId, void *out, unsigned outSize)
{
    static struct Pokemon tmp[PARTY_SIZE];
    static const u8 order[PARTY_SIZE] = { 0, 2, 4, 1, 3, 5 };
    unsigned mon = sizeof(struct Pokemon);
    unsigned i, n;

    if (outSize < mon * PARTY_SIZE)
        return 0;
    memset(tmp, 0, sizeof tmp);
    n = CreateNPCTrainerParty(tmp, trainerId);
    memset(out, 0, mon * PARTY_SIZE);
    for (i = 0; i < PARTY_SIZE; i++)
        if (order[i] < n)
            memcpy((u8 *)out + i * mon, &tmp[order[i]], mon);
    return (int)mon;
}

static unsigned PcCoopPickTrainer(void)
{
    const char *e = getenv("PC_COOP_TRAINER");
    unsigned id;

    if (e)
        return (unsigned)atoi(e);
    for (id = 1; id < TRAINERS_COUNT; id++)
        if (gTrainers[id].partySize >= 3 && gTrainers[id].doubleBattle != TRUE)
            return id;
    return 0;
}

static void CB2_ReturnFromCoopTrainerBattle(void)
{
    sCoopTrainerMode = 0;
    gBattleTypeFlags &= ~BATTLE_TYPE_LINK_IN_BATTLE;
    LoadPlayerParty();                                 /* nesta versão o time volta ao estado de antes */
    SetMainCallback2(CB2_ReturnToField);
}

void PcCoopStartCoopTrainerBattle(unsigned trainerId)
{
    sCoopTrainerMode = 1;
    gBattleTypeFlags = BATTLE_TYPE_TRAINER | BATTLE_TYPE_LINK | BATTLE_TYPE_DOUBLE | BATTLE_TYPE_MULTI;
    CleanupOverworldWindowsAndTilemaps();
    gTrainerBattleOpponent_A = trainerId;
    SetMainCallback2(CB2_InitBattle);
    gMain.savedCallback = CB2_ReturnFromCoopTrainerBattle;
    PcLog("[pc] coop: batalha coop contra o treinador %u iniciada\n", trainerId);
}

static void PcCoopTrainerTriggerCheck(void)
{
    static unsigned sT;
    static int sWarned;
    unsigned id;

    if (!getenv("PC_COOP_HOST") || (++sT & 15) != 0 || access("/tmp/pc_coop_trainer", 0) != 0)
        return;
    if (!sOverworldNow || gMain.inBattle)
    {
        if (!sWarned)
        {
            sWarned = 1;
            PcLog("[pc] coop: treinador coop pedido, mas este jogo nao esta livre no overworld; atendo quando estiver\n");
        }
        return;
    }
    sWarned = 0;
    unlink("/tmp/pc_coop_trainer");
    id = PcCoopPickTrainer();
    if (id == 0 || id >= TRAINERS_COUNT || gTrainers[id].partySize < 2)
    {
        PcLog("[pc] coop: nenhum treinador adequado (id %u); use PC_COOP_TRAINER=<id> com 2 ou mais pokemon\n", id);
        return;
    }
    PcLog("[pc] coop: treinador coop pedido: id %u (%d pokemon)\n", id, gTrainers[id].partySize);
    PcVLTriggerTrainer(id);
}

void PcCoopLinkTick(void)
{
    static unsigned sT;
    static int sWarned;

    if (!PcCoopNetActive())
        return;
    sOverworldNow = sOverworldSeen;                    /* PcCoopTick marca isto todo quadro de overworld */
    sOverworldSeen = 0;
    PcVLPoll();
    PcCoopTrainerTriggerCheck();
    if (getenv("PC_COOP_HOST") && (++sT & 15) == 0 && access("/tmp/pc_pvp", 0) == 0)
    {
        if (!sOverworldNow || gMain.inBattle)
        {
            if (!sWarned)
            {
                sWarned = 1;
                PcLog("[pc] coop: PvP pedido, mas este jogo nao esta livre no overworld (batalha, menu ou carregando); atendo quando estiver\n");
            }
        }
        else
        {
            sWarned = 0;
            unlink("/tmp/pc_pvp");
            PcLog("[pc] coop: gatilho de PvP (arquivo /tmp/pc_pvp)\n");
            PcVLTrigger();
        }
    }
}

void PcCoopTick(void)
{
    static int sStarted;
    struct PcCoopPlayerState local, remote;
    struct ObjectEvent *player;
    struct ObjectEvent *ghost;

    sOverworldSeen = 1;
    if (!sStarted)
    {
        sStarted = 1;
        PcCoopNetStart();
    }
    if (!PcCoopNetActive())
        return;
    /* PcCoopTick só é chamada de dentro de OverworldBasic (patch em overworld.c): só roda com o jogo no overworld. */
    if (!gMapHeader.mapLayout || !gMapHeader.events || !gSaveBlock1Ptr)
        return;
    if (gPlayerAvatar.objectEventId >= OBJECT_EVENTS_COUNT)
        return;
    player = &gObjectEvents[gPlayerAvatar.objectEventId];
    if (!player->active || player->spriteId >= MAX_SPRITES)
        return;

    memset(&local, 0, sizeof local);
    local.valid = 1;
    local.mapGroup = gSaveBlock1Ptr->location.mapGroup;
    local.mapNum = gSaveBlock1Ptr->location.mapNum;
    local.facing = player->facingDirection;
    local.gfx = player->graphicsId;
    local.elevation = player->currentElevation;
    local.x = player->currentCoords.x;
    local.y = player->currentCoords.y;
    PcCoopNetSetLocal(&local);
    SyncTick();
    NpcTick();

    if (!PcCoopNetGetRemote(&remote)
     || remote.mapGroup != local.mapGroup || remote.mapNum != local.mapNum)
    {
        RemoveGhost();
        return;
    }

    if (!GhostValid() || sGhostMapNum != local.mapNum || sGhostMapGroup != local.mapGroup)
    {
        RemoveGhost();
        /* limpa um fantasma antigo que tenha sido salvo junto com o mapa */
        RemoveObjectEventByLocalIdAndMap(PC_COOP_LOCAL_ID, local.mapNum, local.mapGroup);
        sGhostMapNum = local.mapNum;
        sGhostMapGroup = local.mapGroup;
        sGhostObjectId = SpawnSpecialObjectEventParameterized(
            OBJ_EVENT_GFX_GREEN_NORMAL, MOVEMENT_TYPE_NONE, PC_COOP_LOCAL_ID,
            remote.x, remote.y, remote.elevation);
        if (sGhostObjectId >= OBJECT_EVENTS_COUNT)
            return;
    }

    ghost = &gObjectEvents[sGhostObjectId];
    if (ghost->graphicsId != remote.gfx)
        ObjectEventSetGraphicsId(ghost, remote.gfx);
    if (ghost->heldMovementActive)
    {
        if (!ghost->heldMovementFinished)
            return;                                   /* ainda andando: espera o passo terminar */
        ObjectEventClearHeldMovement(ghost);
    }
    {
        int dx = remote.x - ghost->currentCoords.x;
        int dy = remote.y - ghost->currentCoords.y;

        if (dx == 0 && dy == 0)
        {
            if (remote.facing >= DIR_SOUTH && remote.facing <= DIR_EAST && ghost->facingDirection != remote.facing)
                ObjectEventTurn(ghost, remote.facing);
        }
        else if (dx * dx + dy * dy == 1)
        {
            u8 dir = dx > 0 ? DIR_EAST : dx < 0 ? DIR_WEST : dy > 0 ? DIR_SOUTH : DIR_NORTH;
            ObjectEventSetHeldMovement(ghost, GetWalkNormalMovementAction(dir));
        }
        else
        {
            MoveObjectEventToMapCoords(ghost, remote.x, remote.y);   /* longe demais: teleporta */
        }
    }
}
