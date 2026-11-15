# PLANO Coop FireRed — CoopInFire

> **Aguarda aprovação antes de implementar qualquer passo além do 1.**

---

## Estado Atual (confirmado em build — 2026-11-15)

| Item | Status |
|------|--------|
| \src/coop.c\ + \include/coop.h\ | ✅ compila e linka |
| Símbolos na ROM (9 funções/dados) | ✅ confirmado por m\ |
| Ganchos em \link.h\, \link.c\, \overworld.c\ | ✅ |
| EWRAM | ⚠️ 99,60% (261 092 / 262 144 B) — ~1 KB livre |
| IWRAM | ⚠️ 91% (29 824 / 32 768 B) |
| ROM | ✅ 45,9% |
| Commit | ✅ \ee6ed28c2\ branch \CoopInFire\ |
| Testado no jogo | ❌ não testado |

---

## Restrição de Memória (crítica)

EWRAM está em 99,6%. **Nenhum buffer novo pode ser declarado com \EWRAM_DATA\**
sem antes medir e confirmar que cabe no ~1 KB restante, OU reusar espaço existente
(heap dinâmica via \AllocZeroed\, ou campos já alocados como \gObjectEvents\).

**Regra:** cada passo mostra a tabela de memória antes do próximo começar.

---

## Passos

### ✅ Passo 1 — Transporte de Posição (FEITO, commit ee6ed28c2)
- Protocolo: \LINKCMD_COOP_POS\ (0x6A01) em \link.c- \Coop_BuildPosCmd\ / \Coop_OnPosCmd\ / \Coop_OverworldTick\ / \Task_CoopConnect- \gCoopRemote[2]\ em EWRAM: posição + mapa + facing do parceiro remoto
- **Compila** ✅ | **Linka** ✅ | **Testado no jogo** ❌

---

### 🔲 Passo 2 — HUD de Debug (zero bytes novos em EWRAM)
**Objetivo:** Mostrar na tela o estado da conexão e posição recebida, para confirmar
que o Passo 1 funciona sem precisar adivinhar.

**Implementação:**
- \Coop_DrawDebugHud()\ chamada por \Coop_OverworldTick\ a cada frame.
- Usar \PrintUserText\ / \AddTextPrinterToWindow\ já existentes no jogo.
- Texto no canto: \COOP: OFF\ / \COOP: CONN\ / \COOP: OK x=NNN y=NNN pckt=NNN- **Sem \EWRAM_DATA\ novo.** Buffer de texto vai para pilha (local).
- O display some quando coop está OFF.

**Critério de sucesso:** texto visível na tela mudando quando L+R pressionados.

**Verificações automáticas:** compila, linka, m | grep Coop\, tabela de memória.

---

### 🔲 Passo 3 — Avatar Remoto no Overworld
**Objetivo:** Renderizar o jogador 2 como sprite no mapa do jogador 1.

**Pré-pesquisa necessária (antes de escrever código):**
- Verificar \gLinkPlayerObjectEvents\ no FireRed: qual struct é, tamanho real, quando é usada.
- Verificar \AddLinkPlayerObjectEvents\ / \UpdateLinkPlayerObjectEvents\ no decomp.
- Verificar se \OBJECT_EVENTS_COUNT=16\ tem slots livres no overworld normal.
- Decidir estratégia: slot em \gObjectEvents\ OU struct de link player OU heap.

**Critério de sucesso:** sprite do parceiro aparece e se move no mapa.

---

### 🔲 Passo 4 — Batalha Cooperativa (Trainer)
**Objetivo:** Ambos perto de um treinador → batalha dupla de link.

**Pré-pesquisa necessária:**
- \DoTrainerBattle()\ no FireRed: hook de entrada.
- \SetControllerToPlayerPartner\ existe no FireRed? (pode ser só Esmeralda).
- \gLinkPlayersCount\ / \IsLinkActive()\ no FireRed.
- Lembrete: relatórios A-G são de Esmeralda; verificar tudo no decomp do FireRed.

---

### 🔲 Passo 5 — Batalha Cooperativa (Wild)
**Objetivo:** Encontro selvagem vira batalha dupla quando parceiro está perto (≤ 3 tiles, mesmo mapa).

---

### 🔲 Passo 6 — Sincronização de Flags de História
**Objetivo:** Treinador derrotado → flag setado nos dois saves via novo \LINKCMD_COOP_FLAG\.

---

### 🔲 Passo 7 (futuro) — Pokémon Aleatório para Treinadores de 1 Mon
**Objetivo:** Treinador com 1 Pokémon → parceiro recebe um aleatório do mesmo tipo.

---

## Fora do Escopo (deste projeto)
- Port para PC
- NPC sync / bag sync (risco de overflow de EWRAM)
- Shared party / level cap (design não definido)
- Tudo do design doc de Esmeralda que usa símbolos não verificados no FireRed

---

## Aviso sobre Material do Quetzal

| Fonte | Confiabilidade |
|-------|----------------|
| \Battle_Coop_Analysis_A-C.txt\ | ⚠️ INFERIDO — baseado em Esmeralda |
| \Battle_Coop_Analysis_D-G.txt\ | ⚠️ INFERIDO / NÃO ENCONTRADO |
| \Coop_FireRed_Hack_Design.md\ | ❌ Pseudo-código de Esmeralda, NÃO verificado no FireRed |

Verificar cada símbolo/endereço no decomp do FireRed antes de usar.
