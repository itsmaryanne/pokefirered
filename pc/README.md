# Port do pokefirered para PC — experimento (x86 32 bits, Linux/WSL)

Tudo fica em `pc/` e `build_pc/`; nada do código original é alterado (arquivos com asm ARM são
patchados em CÓPIA, em `build_pc/patched/`).

## Como usar (na raiz do repositório, depois de compilar a ROM normalmente com `make`)
    sudo apt install gcc-multilib perl gdb libsdl2-dev pkg-config
    chmod +x pc/*.sh pc/*.pl
    ./pc/build.sh         # compila o jogo (32 bits) e o visualizador (64 bits)
    ./pc/selftest.sh      # autoteste sem janela (~40 s): confere vídeo, input e áudio e diz PASSOU/FALHOU
    ./pc/run.sh           # abre a janela e roda o jogo (com som)
Teclas: Z = A, X = B, Enter = Start, Backspace ou Shift direito = Select, setas, A = L, S = R,
M = mudo, F11 = tela cheia, Esc = sair. Controle (SDL GameController): botão leste = A, sul = B, Back = Select, Start, gatilhos L/R.
A janela e o som usam o WSLg (WSL2 no Windows 11, ou Windows 10 atualizado com `wsl --update`).
Se a janela abrir mas ficar branca/parada: olhe o número no título ("quadro N"). Se N não sobe, o visualizador não está
recebendo o jogo (rode `rm -f /dev/shm/pokefirered_pc`); se N sobe e a tela é branca, rode o autoteste e me mande a saída.
Variáveis: PC_TRACE, PC_DEBUG, PC_DUMP=n,n (grava frame_N.ppm/.mem), PC_HASH=arq (hash por quadro), PC_AUDIO_DUMP=arq (PCM s16 32768 Hz),
PC_AUDIO_MASK, PC_SAVE (padrão pokefirered.sav), PC_NORENDER, PC_NOSHM, PC_NOLOCKSTEP.

## O que cada arquivo faz
- `build.sh`        compila C, monta dados e músicas, compila a camada PC e linka
- `cc_one.sh`       cpp -> preproc (textos GBA) -> gcc -m32 (patcha os 5 arquivos com asm ARM antes)
- `patch_src.pl`    trocas de asm ARM por chamadas C, aplicadas em cópia
- `as_data.sh`, `as_song.sh`, `inline_inc.pl`  montam data/*.s e as músicas com o `as` x86
                    (normalizam .word/.align/@/espaços em macros, que diferem do assembler ARM)
- `bios.c`          BIOS do GBA em C (LZ77/RL, CpuSet, Div, Sqrt, ArcTan2, afins...). `test_bios.c` testa
- `pc_main.c`       mapeia a memória do GBA nos endereços originais e chama AgbMain
- `ppu_timing.c`    thread que simula VCOUNT/DISPSTAT e levanta interrupções (entregues por sinal)
- `intr.c`          despacho de interrupções (porta do IntrMain)
- `dma.c`           DMA simulado (o macro DmaSet é redirecionado numa cópia gerada do macro.h)
- `flash_file.c`    save em arquivo (substitui src/agb_flash*.c)
- `ppu.c`          renderizador de vídeo por software (fundos de texto/afins, sprites, janelas, alpha/brilho)
- `viewer.c`       visualizador SDL2 em 64 bits; conversa com o jogo por memória compartilhada (`shared.h`)
- `run.sh`         roda o jogo + o visualizador
- `m4a_engine.c`   motor de som m4a em C (sequenciador, ply_*, alocação de canais, mixer PCM incl. vozes comprimidas/reversas)
- `audio.c`        PSG do GBA (2 quadrados, onda, ruído), mistura com o PCM, reamostragem e anel de saída
- `frame_sync.c`   sincronismo de quadros em passo travado (determinismo)
- `selftest.sh`    autoteste; `tools/` tem as ferramentas de comparação com o mGBA
- `m4a_real.c`, `sound_stub.c`  parte do motor de som em C; o resto está MUDO (stubs)

## Estado
Roda a 60 quadros/s com janela, teclado/controle e SOM. Validado contra a ROM original no mGBA:
- vídeo: pixel a pixel idêntico nas cenas conferidas (copyright, título, guia de controles, Oak, Nidoran, tela de nome);
  só diferem elementos animados com fase diferente (cursor piscando, quadro de animação de sprites);
- áudio: PCM correlaciona 0,98-0,99 com a referência (incl. gritos de Pokémon); PSG tem as mesmas frequências e energia
  (sem travamento de fase). O nível do PCM fica ~10% abaixo (reamostragem linear 13379->32768 Hz).
Não verificado: janela/teclado/controle/dispositivo de áudio reais (testei com eventos sintéticos e drivers "dummy"),
mosaico e modos bitmap (3/4/5) do renderizador, efeitos por linha (H-blank), BgAffineSet/ObjAffineSet contra hardware,
PSG canal 3 em modo de 64 amostras (2 bancos), multiplayer/wireless (desligados de propósito).
Detalhes técnicos: o jogo é 32 bits (ponteiros de 32 bits e a memória do GBA nos endereços originais); o V-blank "para o
tempo" até o handler terminar e só dispara depois do laço principal terminar o quadro anterior (passo travado);
SetGpuReg grava sempre na hora (a fila do GBA perdia o DISPSTAT conforme o VCOUNT no boot); RegisterRamReset segue a BIOS
(matriz afim identidade); a checagem "tilemap fora da WRAM" do bg.c é patchada em cópia.

## Novidades (janela do Windows, FPS, diagnóstico)
    sudo apt install mingw-w64 curl     # uma vez, para a janela do Windows
    ./pc/run_windows.sh                 # roda o jogo no WSL e abre a janela NATIVA do Windows (pc_viewer.exe), ligada por TCP
F3 mostra/esconde o FPS (também no título da janela). Se o jogo morrer ou travar, o terminal mostra a causa (pilha com nomes);
o título da janela passa a dizer "O JOGO PAROU" ou "O JOGO NAO RESPONDE". Resumo técnico completo em pc/PROJETO.md.
