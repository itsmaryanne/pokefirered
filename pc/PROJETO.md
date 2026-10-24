# Port do Pokémon FireRed para PC — resumo do projeto (para retomar numa conversa nova)

## Objetivo
Rodar o FireRed (decompilação pret/pokefirered) no PC, sem emulador, para depois adicionar uma campanha cooperativa.
Ordem combinada: primeiro o port completo e estável; coop e extras só depois.

## Como o usuário quer trabalhar
- Usa Windows + WSL2 (Ubuntu). Compila e testa SÓ no ambiente dele; o ambiente do Claude não deve ser usado para rodar testes.
- Quer que o limite de uso dure: poucas chamadas de ferramenta, nada de execuções longas, editar com cuidado e pedir que ele rode.
- Ele cola a saída do terminal; por isso o jogo imprime diagnósticos (veja "Diagnóstico").

## Arquitetura (tudo em pc/, nada do código original é alterado)
- O jogo roda como processo de 32 bits (-m32): ponteiros de 32 bits e a memória do GBA (EWRAM, IWRAM, I/O, paleta, VRAM, OAM)
  mapeada nos endereços originais com mmap fixo (pc_main.c). O código C do jogo roda inalterado.
- Compilação: cpp -> preproc (textos) -> gcc -m32 (cc_one.sh). Arquivos com asm ARM inline são patchados em CÓPIA (patch_src.pl):
  script, m4a, main, librfu_intr, multiboot, bg (checagem de WRAM), gpu_regs (SetGpuReg grava sempre na hora).
- Dados (data/*.s) e músicas (sound/songs/midi/*.s) são montados com o `as` x86 (as_data.sh, as_song.sh): normalizam .word/.align/@/espaços.
- BIOS do GBA em C (bios.c): LZ77/RL, CpuSet, Div, Sqrt, ArcTan2, BgAffineSet/ObjAffineSet, RegisterRamReset (segue a BIOS), SoftReset (reinicia).
- Tempo/vídeo (ppu_timing.c): thread simula VCOUNT/DISPSTAT a ~59,7 fps, levanta IRQs e entrega por sinal (SIGUSR1) à thread principal;
  V-blank "para o tempo" até o handler terminar e só dispara depois do laço principal terminar o quadro anterior (passo travado, frame_sync.c).
- Interrupções (intr.c), DMA simulado (dma.c; macro DmaSet redirecionado numa cópia gerada do macro.h), save em arquivo mmap (flash_file.c, pokefirered.sav).
- Vídeo: renderizador por software (ppu.c): fundos de texto/afins, sprites normais/afins, janelas, alpha/brilho. Sem mosaico e sem modos bitmap.
- Som: m4a_engine.c (sequenciador, ply_*, ply_note, mixer PCM incl. vozes comprimidas/reversas), audio.c (PSG + mistura + saída 32768 Hz).
- Janela/entrada/som: viewer.c (SDL2). Modos: memória compartilhada (WSLg, ./pc/run.sh) ou TCP (janela nativa do Windows,
  ./pc/run_windows.sh; ponte no jogo em net_bridge.c, porta 47474). Teclas: Z=A X=B Enter=Start Backspace/ShiftDir=Select, setas, A=L S=R,
  F3=FPS, M=mudo, F11=tela cheia, Esc=sair. A ponte TCP aceita um segundo visualizador
  com `PC_COOP_PLAYER=2`; ele envia a entrada do segundo jogador.
- Scripts: build.sh, run.sh, run_windows.sh, build_windows.sh, selftest.sh (autoteste sem janela). tools/ = comparação com o mGBA.

## Validado (contra a ROM original no mGBA, no ambiente do Claude)
- Vídeo pixel a pixel idêntico até a tela de nome do jogador; áudio PCM correlação 0,98-0,99, PSG com mesmas frequências/energia.
## Não validado / conhecido
- Janela e áudio reais no PC do usuário (cortes de áudio relatados no WSLg), janela do Windows (nova), mosaico, bitmap 3/4/5, H-blank por linha.
- CORRIGIDO (aguarda confirmação do usuário): ao confirmar o nome o jogo dava SIGSEGV em SpriteCB_Cursor/Underscore, porque
  MainState_Exit zera sNamingScreen e o AnimateSprites do mesmo quadro ainda lê dele. No GBA ler NULL+offset cai na BIOS (inofensivo).
  Patch em cópia (naming_screen) + tentativa de mapear a região baixa (precisa `sudo sysctl -w vm.mmap_min_addr=0`).
  ATENÇÃO: este tipo de bug ("leitura de NULL inofensiva no GBA") pode aparecer de novo em outras telas; o handler de falhas mostra onde.

- Família de bugs "NULL+offset inofensivo no GBA": 1) tela de nome (patch em cópia); 2) callback de V-blank da transição de batalha
  (EIP no V-blank, endereço nil). Solução geral: `sudo sysctl -w vm.mmap_min_addr=0` (os run*.sh fazem isso) + pc_main.c mapeia 0..0x10000.
- FPS ~54: o reajuste do relógio da thread de tempo descartava o atraso do V-blank; agora só reajusta na região visível; esperas curtas giram antes de dormir.

## Diagnóstico
- Crash: handler imprime sinal, EIP e pilha (com nomes; senão `addr2line -f -e build_pc/pokefirered_pc 0xEIP`).
- Travamento: watchdog (3 s sem o laço principal responder) imprime a pilha da thread principal.
- Variáveis: PC_TRACE, PC_DEBUG, PC_DUMP, PC_HASH, PC_AUDIO_DUMP, PC_AUDIO_MASK, PC_SAVE, PC_NORENDER, PC_NOSHM, PC_NOLOCKSTEP, PC_NET_PORT.

## Armadilhas já resolvidas (não repetir)
- Roteiro do visualizador estava limitado a 64 passos; BMP do SDL é gravado de baixo para cima; `DrawText` colide com a API do Windows.
- DISPSTAT perdia os bits de V-count conforme o VCOUNT no boot (corrigido no gpu_regs); BG2PA/PD precisam ser 0x100 após RegisterRamReset.
- O ambiente do Claude tem 1 núcleo: comparações de tempo lá não representam o PC do usuário.

## Coop: estado atual e plano
1. Estabilidade: passar do nome, andar em Pallet Town, entrar em batalha, salvar/carregar, abrir menus. Corrigir o que cair.
2. Rede: ponte TCP com host autoritativo; o primeiro visualizador controla o jogador 1 e o segundo envia a entrada do jogador 2.
3. MVP em andamento: segundo jogador visível no overworld ("avatar fantasma": posição, direção, mapa), sem colisões, scripts ou batalha cooperativa.
4. Depois: batalhas em dupla cooperativas contra NPCs (o jogo já tem batalha dupla/multi), flags de história e itens compartilhados.
