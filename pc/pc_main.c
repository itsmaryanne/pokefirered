// Ponto de entrada do port PC. Reserva a memória do GBA nos endereços originais e chama AgbMain.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include "pc_compat.h"
void AgbMain(void);
void PcStartTiming(void);
void PcMemInit(void) {
    static const struct { uintptr_t a; size_t s; const char *n; } r[] = {
        {0x02000000,0x40000,"EWRAM"},{0x03000000,0x8000,"IWRAM"},{0x04000000,0x1000,"IO"},
        {0x05000000,0x400,"PLTT"},{0x06000000,0x18000,"VRAM"},{0x07000000,0x400,"OAM"} };
    for (unsigned i = 0; i < sizeof r / sizeof r[0]; i++) {
        void *p = mmap((void *)r[i].a, r[i].s, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
        if (p != (void *)r[i].a) { fprintf(stderr, "falha ao mapear %s\n", r[i].n); exit(1); }
    }
    // O GBA lê a BIOS nos endereços baixos; o jogo original tem leituras de "NULL + deslocamento" que lá são inofensivas.
    // Se o sistema permitir (sudo sysctl -w vm.mmap_min_addr=0), mapeamos essa região (zeros) e esse tipo de bug não derruba o jogo.
    { void *z = mmap((void *)0, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0); if (z != (void *)0 && getenv("PC_DEBUG")) fprintf(stderr, "[pc] regiao baixa (BIOS) nao mapeada (vm.mmap_min_addr > 0)\n"); }
}
char **gPcArgv;                                       // usado pelo SoftReset (reinicia o processo)
void PcInstallCrashHandlers(void);
int main(int argc, char **argv) { (void)argc; gPcArgv = argv; fprintf(stderr, "[pc] build de %s %s\n", __DATE__, __TIME__); PcInstallCrashHandlers(); PcMemInit(); PcStartTiming(); AgbMain(); return 0; }

// ---- Símbolos que no GBA vinham do linker script / cabeçalho da ROM / libgcnmultiboot ----
unsigned char gHeap[0x1C000];                         // HEAP_SIZE do jogo
const char RomHeaderGameCode[4] = { 'B', 'P', 'R', 'E' };
const char RomHeaderSoftwareVersion = 0;
int GameCubeMultiBoot_Main(void *p) { (void)p; return 0; }
void GameCubeMultiBoot_ExecuteProgram(void *p) { (void)p; }
void GameCubeMultiBoot_Init(void *p) { (void)p; }
void GameCubeMultiBoot_HandleSerialInterrupt(void *p) { (void)p; }
void GameCubeMultiBoot_Quit(void) {}
