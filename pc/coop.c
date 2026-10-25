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
#include "constants/vars.h"
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

static void ApplyEv(const struct PcCoopSyncEvent *e)
{
    unsigned i = e->index;

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

void PcCoopTick(void)
{
    static int sStarted;
    struct PcCoopPlayerState local, remote;
    struct ObjectEvent *player;
    struct ObjectEvent *ghost;

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
