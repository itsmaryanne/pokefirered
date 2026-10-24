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
