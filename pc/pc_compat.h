#ifndef PC_COMPAT_H
#define PC_COMPAT_H
// Declarações das funções da camada de compatibilidade PC (substituem asm ARM / BIOS do GBA)
void PcHalt(void);
void PcClearIwram(void);
void PcMusicPlayerJumpTableCopy(void);
void PcMemInit(void);
void PcLog(const char *fmt, ...);
void PcCoopTick(void);
#endif
