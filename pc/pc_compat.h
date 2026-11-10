#ifndef PC_COMPAT_H
#define PC_COMPAT_H
// Declarações das funções da camada de compatibilidade PC (substituem asm ARM / BIOS do GBA)
void PcHalt(void);
void PcClearIwram(void);
void PcMusicPlayerJumpTableCopy(void);
void PcMemInit(void);
void PcLog(const char *fmt, ...);
void PcCoopTick(void);
int PcVLActive(void);
int PcVLMyId(void);
int PcVLSend(const void *src, unsigned size);
int PcVLTaskFinished(void);
int PcVLRecvStatus(void);
void PcVLResetFlags(void);
void PcVLResetFlag(int who);
void PcVLStandby(void);
void PcVLClose(void);
void PcVLPoll(void);
void PcVLTrigger(void);
int PcLinkIsHost(void);
int PcCoopCanLink(void);
void PcCoopStartVirtualBattle(void);
void PcCoopLinkTick(void);
int PcVLPlayers(void);
void PcVLTriggerTrainer(unsigned trainerId);
int PcCoopTrainerMode(void);
int PcCoopBuildTrainerSlots(unsigned trainerId, void *out, unsigned outSize);
void PcCoopTrainerName(unsigned trainerId, void *dst, unsigned len);
void PcCoopStartCoopTrainerBattle(unsigned trainerId);
int PcCoopTrainerMayEngage(void);
#endif
