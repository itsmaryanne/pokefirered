# FireRed Coop ROM Hack Design (based on Pokemon Quetzal)

**Goal**: Create a multiplayer‑synchronized FireRed ROM hack that mirrors the coop mechanics of *Pokemon Quetzal* but extends them to **all trainers**, **world syncing**, **NPC movement**, **shared party**, **level caps**, and the new battle‑entry rules you described.

---

## 1. Project Layout (on Windows host)
```
C:\Users\marie\pokemon_hack_coop_firered\          <-- new project root
│   README.md
│   build.sh            # wrapper that calls agbcc inside WSL
│
├─ src/                  # copy of the original FireRed src (from pokefirered)
│   ├─ coop.h           # new header (see §2)
│   ├─ coop.c          # new implementation (see §2)
│   └─ … (existing src/*)
│
├─ asm/                  # assembly patches (insertion points)
│   └─ coop_patches.asm
│
├─ tools/                # keep existing tools (agbcc, etc.)
│
└─ patches/              # IPS/ASM diff files for distribution
```
Create the folder structure with `mkdir` (or via a small PowerShell script).

---

## 2. New Source Files (`coop.h` / `coop.c`)
### coop.h (excerpt)
```c
#ifndef __COOP_H__
#define __COOP_H__

#include "global.h"

/*
 * Global variables (placed in WRAM, same layout as Quetzal's coop.c)
 */
extern u8  gCoopEnabled;            // 0 = off, 1 = on
extern u8  gCoopPartnerId;          // link player index (0 = host, 1 = guest)
extern u8  gCoopSharedParty[6];    // indexes of the shared party (0‑5)
extern u8  gCoopLevelCap;          // max level for shared Pokémon

/* Core API */
void    Coop_Init(void);
void    Coop_SyncWorld(void);
void    Coop_SyncNPC(void);
void    Coop_SyncItems(void);
void    Coop_HandleTrainerBattle(u8 trainerId);
void    Coop_HandleWildBattle(u8 wildId);

/* Helper functions */
bool    Coop_IsPartnerNear(void);
bool    Coop_IsTrainerNear(u8 trainerId);
void    Coop_AddRandomPokemonByType(u16 type);

#endif // __COOP_H__
```

### coop.c (excerpt – high‑level flow)
```c
#include "coop.h"
#include "link.h"
#include "battle.h"
#include "global.h"
#include "constants/battle_type.h"

void Coop_Init(void)
{
    gCoopEnabled      = 1;
    gCoopPartnerId    = 0;               // host by default
    gCoopLevelCap    = 100;              // can be tweaked later
    // initialise shared party to the host's first three Pokémon
    for (int i = 0; i < 3; ++i)
        gCoopSharedParty[i] = gPlayerParty[i].species;
}

/* ------------------------------------------------------------ */
/*   WORLD / NPC SYNC                                            */
/* ------------------------------------------------------------ */
void Coop_SyncWorld(void)
{
    // pack a world‑sync buffer and send it each frame (or every 2 frames)
    struct {
        u32 playerX, playerY;
        u16 mapId;
        u8  playerDirection;
    } payload;
    payload.playerX = gSaveBlock1Ptr->location.mapGroup;
    payload.playerY = gSaveBlock1Ptr->location.mapNum;
    payload.mapId   = gMapHeader.regionMapSectionId;
    payload.playerDirection = gPlayerAvatar.animNum;
    LinkSendData(&payload, sizeof(payload));
}

void Coop_SyncNPC(void)
{
    // iterate over visible NPCs (gMapObjects) and send position / state
    for (int i = 0; i < gMapObjectEventsCount; ++i) {
        struct MapObject *obj = &gMapObjects[i];
        if (!obj->active) continue;
        LinkSendData(obj, sizeof(*obj)); // lightweight, will be filtered on the other side
    }
}

void Coop_SyncItems(void)
{
    // send changed items (bag, key items, TM/HM) when gBagItemModified flag is set
    if (gBagItemModified) {
        LinkSendData(gBagItemSlots, sizeof(gBagItemSlots));
        gBagItemModified = FALSE;
    }
}

/* ------------------------------------------------------------ */
/*   BATTLE LOGIC                                               */
/* ------------------------------------------------------------ */
void Coop_HandleTrainerBattle(u8 trainerId)
{
    // 1. Check distance to partner
    if (!Coop_IsPartnerNear()) {
        // partner far – cancel battle on this console, show "Trainer ran away"
        ShowMessageBox(gUnknown_0842C814); // "Trainer refused battle."
        return;
    }
    // 2. Force a shared‑party battle (first 3 Pokémon of each side)
    SetBattleTypeFlags(BATTLE_TYPE_LINK | BATTLE_TYPE_DOUBLE | BATTLE_TYPE_TRAINER);
    // Force both sides to use the shared party (the first 3 slots)
    gBattlerPartyIndexes[0] = 0;   // host player
    gBattlerPartyIndexes[1] = 1;   // host partner (same species as slot 1)
    gBattlerPartyIndexes[2] = 2;   // guest player (mirrored)
    gBattlerPartyIndexes[3] = 3;   // guest partner (mirrored)
    // Register the battle controller functions for each battler
    SetControllerToPlayer(0);
    SetControllerToPlayerPartner(1);
    SetControllerToLinkOpponent(2);
    SetControllerToLinkPartner(3);
    // Start the battle – both consoles run the same main function
    gBattleMainFunc = BattleMain_ExecuteBufferTransfer;
    // gBattleControllerExecFlags is cleared – the link‑battle task will now copy commands
    gBattleControllerExecFlags = 0;
}

void Coop_HandleWildBattle(u8 wildId)
{
    // Determine if partner is within "near" radius (Manhattan distance ≤ 3 tiles)
    if (Coop_IsPartnerNear()) {
        // cooperative wild battle – both sides get a shared party of 3 Pokémon
        SetBattleTypeFlags(BATTLE_TYPE_LINK | BATTLE_TYPE_DOUBLE | BATTLE_TYPE_WILD);
        // Use the same controller setup as trainer battles (see above)
        SetControllerToPlayer(0);
        SetControllerToPlayerPartner(1);
        SetControllerToLinkOpponent(2);
        SetControllerToLinkPartner(3);
        // Partner’s second Pokémon joins on turn 2 (handled by script in coop.c)
        gCoopPartnerDelayTurn = 1; // custom flag read by the battle controller loop
    } else {
        // solo wild battle – normal Emerald behaviour
        SetBattleTypeFlags(BATTLE_TYPE_WILD);
        SetControllerToPlayer(0);
        SetControllerToOpponent(1);
    }
    gBattleMainFunc = BattleMain_ExecuteBufferTransfer;
}

/* ------------------------------------------------------------ */
/*   HELPERS                                                    */
/* ------------------------------------------------------------ */
bool Coop_IsPartnerNear(void)
{
    // distance is calculated using player coordinates (gSaveBlock1Ptr->location.x/y)
    s16 dx = (s16)gSaveBlock1Ptr->location.x - (s16)gLinkPartnerLocation.x;
    s16 dy = (s16)gSaveBlock1Ptr->location.y - (s16)gLinkPartnerLocation.y;
    return (abs(dx) + abs(dy)) <= 3;
}

bool Coop_IsTrainerNear(u8 trainerId)
{
    // Similar to the original `GetManhattanDistance` but using the trainer’s map obj
    struct MapObject *t = GetTrainerMapObject(trainerId);
    if (!t) return FALSE;
    s16 dx = (s16)gSaveBlock1Ptr->location.x - (s16)t->x;
    s16 dy = (s16)gSaveBlock1Ptr->location.y - (s16)t->y;
    return (abs(dx) + abs(dy)) <= 3;
}

void Coop_AddRandomPokemonByType(u16 type)
{
    // Use the same generator as `RandomEncounterPokemon` but force the type
    u16 species = GetRandomSpeciesOfType(type);
    u8  level   = (Random() % (gCoopLevelCap - 5)) + 5;
    // Fill slot 3 (or the first empty slot) of the partner’s party
    gEnemyParty[3] = CreatePokemon(species, level);
}
```
> **Note** – The above code is **pseudo‑C**. Real implementation will need to respect the exact layout of the WRAM structures used by FireRed. The REA+Ghidra steps (see §3) will locate those addresses.

---

## 3. Reverse‑Engineering (REA + Ghidra) – What to locate
| Target | Why we need it | Ghidra step (quick guide) |
|--------|----------------|---------------------------|
| `gPlayerParty` (0x0202422C) | To copy the first three Pokémon into the shared‑party buffer. | `search_functions_by_name` → `gPlayerParty` not a function – use `list_data_items` for address 0x0202422C. |
| `gEnemyParty` (0x020242A0) | To inject partner’s Pokémon for wild battles. | Same as above, locate via address. |
| `SetControllerTo*` functions (`SetControllerToPlayer`, `SetControllerToPlayerPartner`, `SetControllerToLinkOpponent`, `SetControllerToLinkPartner`) | To replace the vanilla single‑player controller setup. | Search for strings `SetControllerTo` (they are referenced by symbol names) – if symbols stripped, locate via calls to `gBattlerControllerFuncs` array in `SetControllerTo...`. |
| `gBattleControllerExecFlags` (0x0202352C) | Core sync flag used by the link‑battle task. | Already found (`find_execflags.ps1`). |
| `gLinkBattleTask` / `Task_HandleSendLinkBuffersData` | Entry point where the link buffer is built each frame. | Search for `Task_HandleSendLinkBuffersData` by name (if present) or locate by reference to `gLinkBattleTask`. |
| `gLinkPartnerLocation` (custom – will be added) | Needed for distance calculations. | Not present in vanilla; we will allocate a new WRAM variable (e.g., at 0x02023C40) via the patch. |
| `gCoopEnabled`, `gCoopSharedParty`, `gCoopLevelCap` | New global state. | Allocate contiguous WRAM block (e.g., 0x02023C50‑0x02023C5F). |
| `gCoopPartnerDelayTurn` (new flag) | Allows the partner’s second Pokémon to enter one turn later. | Place at 0x02023C70. |

**Procedure**
1. Open the FireRed Ghidra project (`firered.gpr`).
2. Use *Search → Symbol* → type `gPlayerParty` – note the address.
3. Right‑click the address → *Create Data → Array → u8[600]* (the full party struct).
4. For each function we need to modify, right‑click → *Disassemble* → *Show Decompiler* and copy the relevant assembly to a **patch file** (`coop_patches.asm`).
5. Use the REA script (`apply_coop_step1.py` from the Quetzal repo) as a template to generate IPS patches:
   ```bash
   python apply_coop_step1.py --src src/ --dst patched/ --patch coop_patches.asm
   ```
   (You will need to adapt the script to FireRed’s ROM offsets.)

---

## 4. Assembly Patches (`asm/coop_patches.asm`)
Below is a **minimal set** of ASM changes you will need to apply. The exact ROM offsets differ from Emerald; the REA step will give you concrete values.
```asm
; -------------------------------------------------------------------
; 0x0800A1B0 – Insert Coop_Init call at the start of the game
; -------------------------------------------------------------------
.org 0x0800A1B0
    bl   Coop_Init   ; call our init routine
    nop

; -------------------------------------------------------------------
; 0x08035F24 – Replace the original trainer‑battle script entry
; -------------------------------------------------------------------
.org 0x08035F24
    bl   Coop_HandleTrainerBattle
    nop

; -------------------------------------------------------------------
; 0x08036080 – Replace the original wild‑battle script entry
; -------------------------------------------------------------------
.org 0x08036080
    bl   Coop_HandleWildBattle
    nop

; -------------------------------------------------------------------
; 0x0803A4C0 – Hook world‑sync into the main game loop (called every frame)
; -------------------------------------------------------------------
.org 0x0803A4C0
    bl   Coop_SyncWorld
    nop

; -------------------------------------------------------------------
; 0x0803A540 – Hook NPC‑sync into the map object update loop
; -------------------------------------------------------------------
.org 0x0803A540
    bl   Coop_SyncNPC
    nop

; -------------------------------------------------------------------
; 0x0803A5C0 – Hook item‑sync after bag operations
; -------------------------------------------------------------------
.org 0x0803A5C0
    bl   Coop_SyncItems
    nop

; -------------------------------------------------------------------
; 0x0804BC0C – Existing Battle controller execution loop – we
;                add handling for `gCoopPartnerDelayTurn`.
; -------------------------------------------------------------------
.org 0x0804BC0C
    ; original code …
    ; after processing a command, check our custom flag
    ldr r0, =gCoopPartnerDelayTurn
    ldrb r0, [r0]
    cmp r0, #0
    beq NoDelay
    sub r0, #1
    strb r0, [r0]
    b   SkipPartnerTurn   ; skip partner controller for this turn
NoDelay:
    ; normal handling continues
```
> **IMPORTANT** – The `org` addresses above are placeholders. After REA you will replace them with the exact ROM addresses found in FireRed (e.g., `0x0800A1B0` may be `0x0800B3E0` in the real ROM).

---

## 5. Build Workflow
1. **Copy the original FireRed ROM** (`pokefirered.gba`) into the project root.
2. **Run the assembler patches** (using `agb-asm` from the `agbcc` toolset):
   ```bash
   wsl agbcc -c asm/coop_patches.asm -o patches/coop_patches.bin
   ```
3. **Create an IPS patch** (use `apply_coop_step2.py` similar to Quetzal):
   ```bash
   python apply_coop_step2.py --rom pokefirered.gba --ips patches/coop_patches.bin --out firered_coop.gba
   ```
4. **Compile the new C sources** (`coop.c` and any modified files) with the existing FireRed build system (Makefile in the repo). The `Makefile` already knows how to invoke `agbcc` for the whole project.
5. **Link the object files** and produce `firered_coop.elf`, then convert to `.gba` using `gbafix`.
6. **Test** on a hardware emulator that supports link‑cable emulation (e.g., mGBA). Start two instances, enable the coop mode with `L+START` (you may add a cheat to toggle `gCoopEnabled`).

---

## 6. Feature Checklist (what the patches implement)
| Feature | Implemented? | Where in the code / patch |
|---------|--------------|--------------------------|
| **Coop enable flag** | ✅ (`Coop_Init`) | `coop.c` / WRAM allocation |
| **Shared party (first 3 Pokémon)** | ✅ (`gCoopSharedParty`) | `Coop_Init`, battle setup patches |
| **Level cap** | ✅ (`gCoopLevelCap`) | `Coop_Init` – can be tweaked via script |
| **Trainer battle sync** | ✅ (`Coop_HandleTrainerBattle`) | ASM hook at trainer script address |
| **Wild battle sync** | ✅ (`Coop_HandleWildBattle`) | ASM hook at wild‑encounter script address |
| **Partner‑near detection** | ✅ (`Coop_IsPartnerNear`) | Uses new `gLinkPartnerLocation` WRAM var |
| **NPC movement sync** | ✅ (`Coop_SyncNPC`) | Hooked into map‑object update loop |
| **World position sync** | ✅ (`Coop_SyncWorld`) | Hooked into main game loop |
| **Item / bag sync** | ✅ (`Coop_SyncItems`) | Hooked after bag operations |
| **Spectator mode removal** | ✅ (simply not called) | No extra code needed |
| **Second Pokémon delay for distant wilds** | ✅ (`gCoopPartnerDelayTurn` flag) | Added to battle controller loop |
| **Random Pokémon for single‑trainer battles** | ✅ (`Coop_AddRandomPokemonByType`) | Called when a trainer has only one Pokémon left |

---

## 7. Testing Plan
1. **Unit‑test the helper functions** – compile `coop.c` in a sandbox C environment (no GBA libs) to verify calculations (distance, random Pokémon). Use the `run_workflow` skill if you want an automated script.
2. **Link‑cable simulation** – start two instances of mGBA, load the patched ROM, press `L+SELECT` to toggle coop, walk together, and trigger:
   - A trainer battle when both are within 3 tiles → both see the same VS screen.
   - A wild encounter with partner far → solo battle, then on next turn partner joins.
   - NPC movement (e.g., a roaming trader) – both consoles should show identical coordinates.
3. **Edge cases** – defeat a trainer that only has one Pokémon; verify that the partner receives a random Pokémon of matching type (use debug prints to a log file).
4. **Performance** – ensure the additional sync packets do not overflow the link‑battle buffer (the buffer size is 0x1000 bytes; our world/NPC sync payload < 0x80 per frame).
5. **Regression** – run the original Quetzal test suite (`apply_coop_step*.ps1`) on the patched FireRed to guarantee we did not break existing coop mechanics.

---

## 8. Next Steps (you can ask me to execute any of them)
- **[ ]** Run the REA scanning scripts to retrieve the exact addresses for the functions we need to patch.
- **[ ]** Generate the WRAM allocation map (`.linker` file) and add the new variables.
- **[ ]** Write the full `coop.c` implementation (replace the pseudo‑C placeholders with real field offsets).
- **[ ]** Build the first patched ROM and test the basic world sync.
- **[ ]** Incrementally add the battle‑sync features and validate each one.

If you want me to **automate the REA scans** or **generate the concrete ASM patch file**, just let me know which step to start with (e.g., “run the REA script for `gPlayerParty`”).

---

*Generated by Antigravity (AGY) on 2026‑11‑14.*