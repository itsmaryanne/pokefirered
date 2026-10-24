// Temporização de vídeo simulada: uma thread avança VCOUNT/DISPSTAT no ritmo do GBA
// (228 linhas por quadro, ~59,7275 quadros/s), levanta os flags de interrupção de V-blank / H-blank /
// V-count em REG_IF e avisa a thread principal (SIGUSR1) para tratá-los de forma assíncrona.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "shared.h"
#define REG_DISPSTAT_PTR ((uint16_t *)0x04000004)
#define REG_VCOUNT (*(volatile uint16_t *)0x04000006)
#define REG_KEYINPUT (*(volatile uint16_t *)0x04000130)
#define REG_IE (*(volatile uint16_t *)0x04000200)
#define REG_IF_PTR ((uint16_t *)0x04000202)
#define REG_IME (*(volatile uint16_t *)0x04000208)
#define LINES 228
#define NS_PER_LINE 73433L // 1e9 / (59.7275 * 228)

void PcDeliverIntrs(void);
int PcMainWaitingForVBlank(void);
void PcRenderFrame(uint32_t *out);
// Quadro mais recente (240x160, ARGB8888), renderizado no início de cada V-blank. A camada SDL vai exibir isto.
uint32_t gPcFrame[240 * 160];
static struct PcShared *sShared;
struct PcShared *PcGetShared(void) { return sShared; }
void PcNetStart(struct PcShared *sh);
static int sStall;
volatile int gPcInVBlankWait; // 1 enquanto o jogo espera o V-blank pela BIOS (VBlankIntrWait)
void PcVBlankDma(void);       // quadros seguidos em que o laço principal não respondeu                     // memória compartilhada com o visualizador (pc_viewer)
static void ShmOpen(void)
{
    if (getenv("PC_NOSHM"))
        return;
    // Não fazemos unlink/recriação: um visualizador que já esteja aberto continua ligado ao mesmo objeto.
    // Invalidamos o magic, reinicializamos no lugar e só então publicamos (magic + pid por último).
    char shmName[96]; const char *np = getenv("PC_NET_PORT");
    if (np) snprintf(shmName, sizeof shmName, "%s_%s", PC_SHM_NAME, np); else snprintf(shmName, sizeof shmName, "%s", PC_SHM_NAME);
    int fd = shm_open(shmName, O_RDWR | O_CREAT, 0600);
    if (fd < 0)
    {
        perror("[pc] shm_open (visualizador desativado)");
        return;
    }
    if (ftruncate(fd, sizeof(struct PcShared)) != 0)
    {
        perror("[pc] ftruncate");
        close(fd);
        return;
    }
    void *p = mmap(NULL, sizeof(struct PcShared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED)
    {
        perror("[pc] mmap shm");
        return;
    }
    sShared = p;
    sShared->magic = 0;
    sShared->keyInput = 0x3FF;
    sShared->quit = 0;
    sShared->frameCounter = 0;
    sShared->audioWrite = 0;
    sShared->audioRead = 0;
    memset((void *)sShared->frame, 0, sizeof sShared->frame);
    sShared->speed = 1;
    sShared->coopKeyInput = 0x3FF;
    sShared->coopConnected = 0;
    sShared->gamePid = (uint32_t)getpid();
    sShared->magic = PC_SHM_MAGIC;
}
static FILE *sHashFile;
static int sHashInit;
static void MaybeHash(unsigned long frame)
{ // PC_HASH=arquivo: uma linha "quadro hash" por quadro (comparação com o mGBA)
    if (!sHashInit)
    {
        sHashInit = 1;
        const char *e = getenv("PC_HASH");
        if (e)
            sHashFile = fopen(e, "w");
    }
    if (!sHashFile)
        return;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 240 * 160; i++)
    {
        uint32_t c = gPcFrame[i] & 0xFFFFFF;
        h = (h ^ (c & 255)) * 16777619u;
        h = (h ^ ((c >> 8) & 255)) * 16777619u;
        h = (h ^ (c >> 16)) * 16777619u;
    }
    fprintf(sHashFile, "%lu %08x\n", frame, h);
    fflush(sHashFile);
}
static int sDump[64], sDumpN = -1;
static void MaybeDump(unsigned long frame)
{ // PC_DUMP=120,300,600 grava frame_N.ppm nesses quadros
    if (sDumpN < 0)
    {
        sDumpN = 0;
        const char *e = getenv("PC_DUMP");
        while (e && *e && sDumpN < 64)
        {
            sDump[sDumpN++] = atoi(e);
            while (*e && *e != ',')
                e++;
            if (*e == ',')
                e++;
        }
    }

    if (access("/tmp/pc_dump", F_OK) == 0)
    {
        unlink("/tmp/pc_dump");
        if (sDumpN < 64)
            sDump[sDumpN++] = (int)frame;
        volatile uint16_t *io = (volatile uint16_t *)0x04000000;
        fprintf(stderr, "[pc] extra: HOFS/VOFS=%04x/%04x %04x/%04x %04x/%04x %04x/%04x WIN0H=%04x WIN1H=%04x WIN0V=%04x WIN1V=%04x DMA0-3 CNT_H=%04x %04x %04x %04x\n",
                io[0x10 / 2], io[0x12 / 2], io[0x14 / 2], io[0x16 / 2], io[0x18 / 2], io[0x1A / 2], io[0x1C / 2], io[0x1E / 2],
                io[0x40 / 2], io[0x42 / 2], io[0x44 / 2], io[0x46 / 2], io[0xBA / 2], io[0xC6 / 2], io[0xD2 / 2], io[0xDE / 2]);
    }
    for (int i = 0; i < sDumpN; i++)
        if ((unsigned long)sDump[i] == frame)
        {
            fprintf(stderr, "[pc] quadro %lu: DISPCNT=%04x BG0-3CNT=%04x %04x %04x %04x BLDCNT=%04x BLDY=%04x WININ=%04x WINOUT=%04x PLTT[0..3]=%04x %04x %04x %04x PLTT[16..19]=%04x %04x %04x %04x\n", frame,
                    *(volatile uint16_t *)0x04000000, *(volatile uint16_t *)0x04000008, *(volatile uint16_t *)0x0400000A, *(volatile uint16_t *)0x0400000C, *(volatile uint16_t *)0x0400000E,
                    *(volatile uint16_t *)0x04000050, *(volatile uint16_t *)0x04000054, *(volatile uint16_t *)0x04000048, *(volatile uint16_t *)0x0400004A,
                    ((uint16_t *)0x05000000)[0], ((uint16_t *)0x05000000)[1], ((uint16_t *)0x05000000)[2], ((uint16_t *)0x05000000)[3],
                    ((uint16_t *)0x05000000)[16], ((uint16_t *)0x05000000)[17], ((uint16_t *)0x05000000)[18], ((uint16_t *)0x05000000)[19]);
            char name[64];
            snprintf(name, sizeof name, "frame_%lu.ppm", frame);
            FILE *f = fopen(name, "wb");
            if (!f)
                return;
            fprintf(f, "P6\n240 160\n255\n");
            for (int p = 0; p < 240 * 160; p++)
            {
                uint32_t c = gPcFrame[p];
                fputc((c >> 16) & 255, f);
                fputc((c >> 8) & 255, f);
                fputc(c & 255, f);
            }
            fclose(f);
            snprintf(name, sizeof name, "frame_%lu.mem", frame);
            f = fopen(name, "wb"); // IO(1 KB) + PLTT(1 KB) + VRAM(96 KB) + OAM(1 KB), para depuração offline
            if (f)
            {
                fwrite((void *)0x04000000, 1, 0x400, f);
                fwrite((void *)0x05000000, 1, 0x400, f);
                fwrite((void *)0x06000000, 1, 0x18000, f);
                fwrite((void *)0x07000000, 1, 0x400, f);
                fclose(f);
            }
        }
}
extern volatile unsigned long gPcVBlankServiced;
// Estado das teclas (ativo em zero, como o REG_KEYINPUT do GBA). A camada SDL de input vai escrever aqui.
volatile uint16_t gPcKeyInput = 0x03FF;
static volatile unsigned long sFrame; // quadros completos desde o início
static volatile int sRunning;
static pthread_t sMainThread;
static unsigned long sKills;

static void OnIrqSignal(int sig)
{
    (void)sig;
    PcDeliverIntrs();
}

static void Raise(uint16_t flag) { __atomic_fetch_or(REG_IF_PTR, flag, __ATOMIC_SEQ_CST); }

static long NowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
static long sLastBt;
static int sBtCount;
static void MaybeBacktrace(const char *why)
{ // pede a pilha da thread principal quando algo está lento (no máx. 8 vezes, 1 a cada 1,5 s)
    long n = NowMs();
    if (sBtCount < 8 && n - sLastBt > 1500)
    {
        sLastBt = n;
        sBtCount++;
        fprintf(stderr, "\n[pc] LENTO: %s. Pilha da thread principal agora:\n", why);
        pthread_kill(sMainThread, SIGUSR2);
    }
}
static inline void SpinOrSleep(int i)
{ // primeiro gira (rápido), depois dorme em passos de 100 us
    if (i < 300)
    {
        for (int k = 0; k < 40; k++)
            __builtin_ia32_pause();
    }
    else
    {
        struct timespec w = {0, 100000};
        nanosleep(&w, NULL);
    }
}
static void *TimingThread(void *arg)
{
    (void)arg;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    for (;;)
    {
        { uint32_t sp = sShared ? sShared->speed : 1; if (sp < 1) sp = 1; if (sp > 32) sp = 32; t.tv_nsec += NS_PER_LINE / (long)sp; }
        while (t.tv_nsec >= 1000000000L)
        {
            t.tv_nsec -= 1000000000L;
            t.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL); // absoluto: sem deriva acumulada
        {                                                          // Se ficamos muito atrasados (ex.: renderização lenta), descartamos o atraso em vez de avançar
            // várias linhas de uma vez: rajadas fariam o VCOUNT passar por cada valor em microssegundos e
            // laços do jogo que esperam um valor exato (SampleFreqSet espera VCOUNT==0) poderiam nunca vê-lo.
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long late = (now.tv_sec - t.tv_sec) * 1000000000L + (now.tv_nsec - t.tv_nsec);
            if (late > 2 * NS_PER_LINE && (REG_VCOUNT < 160 || late > 8000000L))
                t = now; // no V-blank (linhas >= 160) o atraso do handler/render é absorvido, não descartado: senão cada quadro perderia esse tempo e o FPS cairia
        }
        uint16_t v = (REG_VCOUNT + 1) % LINES;
        REG_VCOUNT = v;
        if (sShared)
        {
            gPcKeyInput = (uint16_t)(sShared->keyInput & 0x3FF);
            if (sShared->quit)
                _exit(0);
        }
        REG_KEYINPUT = gPcKeyInput; // o jogo (ou RegisterRamReset) pode zerar o registrador; reafirmamos
        uint16_t flags = 0;
        if (v >= 160 && v <= 226)
            flags |= 0x0001; // flag de V-blank
        uint16_t d = *(volatile uint16_t *)REG_DISPSTAT_PTR;
        if (v == (d >> 8))
            flags |= 0x0004; // flag de V-count (LYC)
        __atomic_fetch_and(REG_DISPSTAT_PTR, (uint16_t)~0x0007, __ATOMIC_SEQ_CST);
        __atomic_fetch_or(REG_DISPSTAT_PTR, flags, __ATOMIC_SEQ_CST);
        if (v == 160)
        {
            sFrame++;
            PcVBlankDma(); // DMAs com gatilho de V-blank
            // V-blank: o "tempo para" (VCOUNT fica em 160) até o jogo terminar de tratar a interrupção, como se a CPU
            // fosse infinitamente rápida dentro do handler. Sem isso, o handler assíncrono começa tarde e coisas como
            // ProcessDma3Requests (que desiste se VCOUNT > 224) nunca rodam. Espera no máximo ~100 ms (IME desligado etc.).
            unsigned long before = gPcVBlankServiced;
            if ((d & 0x0008) && (REG_IE & 1))
            {
                if (!getenv("PC_NOLOCKSTEP"))
                { // passo travado: espera o laço principal terminar o quadro anterior (máx. ~250 ms)
                    int ok = 0;
                    long tw0 = NowMs();
                    for (int i = 0; i < 2500; i++)
                    {
                        if (PcMainWaitingForVBlank() || gPcInVBlankWait)
                        {
                            ok = 1;
                            break;
                        }
                        SpinOrSleep(i);
                        if (i > 300 && NowMs() - tw0 > (sStall ? 2 : 40))
                            break; // laço fechado do jogo: não espera mais de 40 ms
                        MaybeBacktrace("o laço principal do jogo demora para terminar o quadro");
                    }
                    if (ok)
                        sStall = 0;
                    else
                    {
                        static int once;
                        if (!once)
                        {
                            once = 1;
                            fprintf(stderr, "\n[pc] passo travado estourou: IME=%04x IE=%04x IF=%04x intrCheck? waitingFlag=%d\n",
                                    REG_IME, REG_IE, *(volatile uint16_t *)REG_IF_PTR, gPcInVBlankWait);
                            pthread_kill(sMainThread, SIGUSR2);
                        }
                        if (++sStall == 120)
                        { /* mantém o aviso de 3 s que já existe */
                        }
                    }
                }
                Raise(1 << 0);
                long ts0 = NowMs();
                for (int i = 0; i < 1000 && gPcVBlankServiced == before; i++)
                {
                    if ((i == 0 || (i >= 300 && (i % 20) == 0)) && (REG_IME & 1))
                    {
                        sKills++;
                        pthread_kill(sMainThread, SIGUSR1);
                    }
                    SpinOrSleep(i);
                    if (i > 300 && NowMs() - ts0 > (sStall ? 2 : 40))
                        break; // laço fechado do jogo: não espera mais de 40 ms
                    if (i > 300 && (i & 63) == 0 && NowMs() - ts0 > 80)
                        MaybeBacktrace("o handler de V-blank demora");
                }
            }
            // Só agora desenhamos: o quadro mostra o estado já atualizado pelo handler de V-blank (como no hardware).
            if (!getenv("PC_NORENDER"))
            {
                struct timespec r0, r1;
                clock_gettime(CLOCK_MONOTONIC, &r0);
                PcRenderFrame(gPcFrame);
                MaybeDump(sFrame);
                MaybeHash(sFrame);
                if (sShared)
                {
                    memcpy(sShared->frame, gPcFrame, sizeof gPcFrame);
                    sShared->frameCounter = (uint32_t)sFrame;
                }
                clock_gettime(CLOCK_MONOTONIC, &r1);
                if (getenv("PC_DEBUG") && sFrame % 60 == 0)
                    fprintf(stderr, "[pc/thr] render levou %ld us\n", (r1.tv_sec - r0.tv_sec) * 1000000L + (r1.tv_nsec - r0.tv_nsec) / 1000);
            }
        }
        if (v < 160 && (d & 0x0010))
            Raise(1 << 1); // IRQ de H-blank (linhas visíveis)
        if ((flags & 0x0004) && (d & 0x0020))
            Raise(1 << 2); // IRQ de V-count
        int pend = (REG_IME & 1) && (REG_IE & *(volatile uint16_t *)REG_IF_PTR);
        if (pend)
        {
            sKills++;
            pthread_kill(sMainThread, SIGUSR1);
        }
        {
            static int d2 = -1;
            if (d2 < 0)
                d2 = getenv("PC_DEBUG2") != NULL;
            if (d2 && sFrame >= 100 && sFrame <= 101 && (v % 20 == 0 || v == 160 || v == 161))
                fprintf(stderr, "[pc/thr] f=%lu v=%u DISPCNT=%04x served=%lu\n", sFrame, v, *(volatile uint16_t *)0x04000000, gPcVBlankServiced);
        }
        static int dbg = -1;
        if (dbg < 0)
            dbg = getenv("PC_DEBUG") != NULL;
        if (dbg && v == 100 && sFrame % 60 == 0)
            fprintf(stderr, "[pc/thr] frame=%lu served=%lu kills=%lu IE=%04x IF=%04x IME=%04x DISPSTAT=%04x\n", sFrame, gPcVBlankServiced, sKills,
                    REG_IE, *(volatile uint16_t *)REG_IF_PTR, REG_IME, *(volatile uint16_t *)REG_DISPSTAT_PTR);
    }
    return NULL;
}

void PcStartTiming(void)
{
    pthread_t th;
    if (sRunning)
        return;
    sRunning = 1;
    sMainThread = pthread_self();
    ShmOpen();
    PcNetStart(sShared);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = OnIrqSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1); // a thread de temporização nunca recebe o sinal
    pthread_sigmask(SIG_BLOCK, &block, NULL);
    if (pthread_create(&th, NULL, TimingThread, NULL) != 0)
    {
        fprintf(stderr, "[pc] falha ao criar a thread de temporização\n");
        exit(1);
    }
    pthread_detach(th);
    pthread_sigmask(SIG_UNBLOCK, &block, NULL);
}

static unsigned CountNonZero(const uint8_t *p, unsigned n)
{
    unsigned c = 0;
    for (unsigned i = 0; i < n; i++)
        c += p[i] != 0;
    return c;
}

void PcVBlankWait(void)
{ // chamado pelo VBlankIntrWait da BIOS
    unsigned long snap = gPcVBlankServiced, f0 = sFrame;
    REG_IME = 1;
    gPcInVBlankWait = 1;
    for (;;)
    { // as interrupções chegam por sinal enquanto dormimos
        if (gPcVBlankServiced != snap)
            break;
        if (!(REG_IE & 1) && sFrame != f0)
            break; // V-blank desabilitado em IE: só espera o quadro
        struct timespec ts = {0, 100000};
        nanosleep(&ts, NULL);
    }
    gPcInVBlankWait = 0;
    if (getenv("PC_TRACE") && sFrame % 60 == 0)
        fprintf(stderr, "[pc] quadro %lu  DISPCNT=0x%04x  VRAM!=0: %u  PLTT!=0: %u  OAM!=0: %u bytes\n", sFrame,
                *(volatile uint16_t *)0x04000000, CountNonZero((uint8_t *)0x06000000, 0x18000),
                CountNonZero((uint8_t *)0x05000000, 0x400), CountNonZero((uint8_t *)0x07000000, 0x400));
}
