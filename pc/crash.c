// Diagnóstico de falhas: se o jogo morrer (SIGSEGV etc.) imprime onde; se o laço principal travar, o watchdog
// (em ppu_timing.c) manda SIGUSR2 para a thread principal e esta imprime a pilha. Linkado com -rdynamic para
// aparecerem os nomes das funções. Sem nomes? use:  addr2line -f -e build_pc/pokefirered_pc ENDEREÇO
#define _GNU_SOURCE
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ucontext.h>

static void Resolve(void *const *bt, int n) {            // nomes das funções (inclusive as static) via addr2line, se existir
    char exe[300]; ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k <= 0) return;
    exe[k] = 0;
    char cmd[2200]; int off = snprintf(cmd, sizeof cmd, "addr2line -f -e '%s'", exe);
    for (int i = 0; i < n && off < (int)sizeof cmd - 40; i++) off += snprintf(cmd + off, sizeof cmd - (size_t)off, " %p", bt[i]);
    snprintf(cmd + off, sizeof cmd - (size_t)off, " 2>/dev/null | paste - - 1>&2");
    fprintf(stderr, "[pc] funções (nome  arquivo:linha):\n"); fflush(stderr);
    (void)system(cmd);
}

static void OnCrash(int sig, siginfo_t *si, void *ctx) {
    void *bt[64]; int n = backtrace(bt, 64);
    unsigned long eip = 0;
#ifdef REG_EIP
    eip = (unsigned long)((ucontext_t *)ctx)->uc_mcontext.gregs[REG_EIP];
#endif
    fprintf(stderr, "\n[pc] ERRO FATAL: sinal %d (%s), endereço acessado %p, EIP=0x%08lx\n[pc] pilha (mais recente primeiro):\n", sig, strsignal(sig), si->si_addr, eip);
    backtrace_symbols_fd(bt, n, 2);
    Resolve(bt, n);
    fprintf(stderr, "[pc] se os nomes não aparecerem: addr2line -f -e build_pc/pokefirered_pc 0x%08lx\n", eip);
    _exit(128 + sig);
}

static void OnDump(int sig) {
    (void)sig;
    void *bt[64]; int n = backtrace(bt, 64);
    fprintf(stderr, "[pc] pilha da thread principal (mais recente primeiro):\n");
    backtrace_symbols_fd(bt, n, 2);
    Resolve(bt, n);
}

void PcInstallCrashHandlers(void) {
    static char altstack[1 << 16];                     // pilha alternativa: o handler funciona mesmo com estouro de pilha
    stack_t ss; ss.ss_sp = altstack; ss.ss_size = sizeof altstack; ss.ss_flags = 0; sigaltstack(&ss, NULL);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = OnCrash; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
    struct sigaction sd; memset(&sd, 0, sizeof sd); sd.sa_handler = OnDump; sigemptyset(&sd.sa_mask);
    sigaction(SIGUSR2, &sd, NULL);
}
