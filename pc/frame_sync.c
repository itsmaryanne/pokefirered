// Sincronismo de quadros em "passo travado": o V-blank só é disparado depois que o laço principal do jogo
// terminou o quadro anterior (voltou a esperar em WaitForVBlank, que limpa o flag de V-blank em gMain.intrCheck).
// No hardware o jogo precisa terminar a tempo ou o quadro é perdido; aqui, em vez de pular lógica, a emulação
// espera — assim o comportamento é determinístico mesmo em máquinas lentas ou ocupadas.
#include "global.h"
#include "main.h"

int PcMainWaitingForVBlank(void) { return !(gMain.intrCheck & INTR_FLAG_VBLANK); }
