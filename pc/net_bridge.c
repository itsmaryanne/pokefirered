// Ponte TCP do lado do jogo: permite um visualizador em OUTRO sistema (ex.: janela nativa do Windows, rodando fora
// do WSL) ver o jogo e mandar as teclas. Ativada por PC_NET_PORT=porta (padrão do run_windows.sh: 47474).
// Escuta só em 127.0.0.1 (PC_NET_BIND=ip para mudar). Protocolo: pacotes [tipo u8][tamanho u32 LE][dados].
//   jogo -> visualizador: 1 = quadro (u32 número + 240*160*4 bytes ARGB), 2 = áudio (s16 estéreo 32768 Hz)
//   visualizador -> jogo: 3 = teclas do jogador 1, 4 = encerrar, 6 = teclas do jogador 2
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "shared.h"

static struct PcShared *sSh;

static int SendAll(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) { ssize_t k = send(fd, c, n, MSG_NOSIGNAL); if (k <= 0) { if (k < 0 && errno == EINTR) continue; return 0; } c += k; n -= (size_t)k; }
    return 1;
}

static void *Bridge(void *arg) {
    int port = (int)(intptr_t)arg, one = 1;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("[pc] socket"); return NULL; }
    fcntl(ls, F_SETFD, FD_CLOEXEC);                    // o SoftReset (exec) não pode herdar o socket
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    const char *bindIp = getenv("PC_NET_BIND"); a.sin_addr.s_addr = bindIp ? inet_addr(bindIp) : htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 2) < 0) { perror("[pc] bind/listen da ponte TCP"); return NULL; }
    fcntl(ls, F_SETFL, fcntl(ls, F_GETFL, 0) | O_NONBLOCK);
    fprintf(stderr, "[pc] aguardando o visualizador em %s:%d\n", bindIp ? bindIp : "127.0.0.1", port);
    static uint32_t frameBuf[240 * 160];
    static int16_t abuf[PC_AUDIO_RING_FRAMES * 2];
    for (;;) {
        int fd[2] = {-1, -1};
        uint8_t rbuf[2][64]; int rn[2] = {0, 0};
        uint32_t lastFrame = 0xFFFFFFFFu;
        uint8_t hdr[5];
        int ok = 1;
        while (ok) {
            for (int i = 0; i < 2; i++) {
                if (fd[i] >= 0)
                    continue;
                int nfd = accept(ls, NULL, NULL);
                if (nfd >= 0) {
                    fcntl(nfd, F_SETFD, FD_CLOEXEC);
                    setsockopt(nfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    fd[i] = nfd;
                    rn[i] = 0;
                    fprintf(stderr, "[pc] visualizador %d conectado%s\n", i + 1, i ? " (coop)" : "");
                    if (i == 0) sSh->audioRead = sSh->audioWrite;
                    else { sSh->coopConnected = 0; sSh->coopKeyInput = 0x3FF; }
                }
            }
            if (fd[0] < 0) { usleep(2000); continue; }

            int did = 0;
            uint32_t fc = sSh->frameCounter;
            if (fc != lastFrame) {
                lastFrame = fc; memcpy(frameBuf, sSh->frame, sizeof frameBuf);
                uint32_t len = 4 + sizeof frameBuf; hdr[0] = 1; memcpy(hdr + 1, &len, 4);
                for (int i = 0; i < 2; i++)
                    if (fd[i] >= 0 && !(SendAll(fd[i], hdr, 5) && SendAll(fd[i], &fc, 4) && SendAll(fd[i], frameBuf, sizeof frameBuf)))
                    { close(fd[i]); fd[i] = -1; if (i == 1) sSh->coopConnected = 0; }
                did = 1;
            }
            uint32_t w = __atomic_load_n(&sSh->audioWrite, __ATOMIC_ACQUIRE), r = sSh->audioRead;
            if (w != r) {
                uint32_t n = w - r; if (n > PC_AUDIO_RING_FRAMES) { r = w - PC_AUDIO_RING_FRAMES; n = PC_AUDIO_RING_FRAMES; }
                for (uint32_t i = 0; i < n; i++) { uint32_t pos = (r + i) % PC_AUDIO_RING_FRAMES; abuf[i * 2] = sSh->audio[pos * 2]; abuf[i * 2 + 1] = sSh->audio[pos * 2 + 1]; }
                uint32_t len = n * 4; hdr[0] = 2; memcpy(hdr + 1, &len, 4);
                for (int i = 0; i < 2; i++)
                    if (fd[i] >= 0 && !(SendAll(fd[i], hdr, 5) && SendAll(fd[i], abuf, len)))
                    { close(fd[i]); fd[i] = -1; if (i == 1) sSh->coopConnected = 0; else ok = 0; }
                __atomic_store_n(&sSh->audioRead, w, __ATOMIC_RELEASE); did = 1;
            }
            for (int i = 0; i < 2; i++) {
                if (fd[i] < 0) continue;
                ssize_t k = recv(fd[i], rbuf[i] + rn[i], sizeof rbuf[i] - (size_t)rn[i], MSG_DONTWAIT);
                if (k == 0) { close(fd[i]); fd[i] = -1; if (i == 1) sSh->coopConnected = 0; continue; }
                if (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { close(fd[i]); fd[i] = -1; if (i == 1) sSh->coopConnected = 0; continue; }
                if (k > 0) rn[i] += (int)k;
                while (rn[i] >= 5) {
                    uint32_t len; memcpy(&len, rbuf[i] + 1, 4);
                    if (len > sizeof rbuf[i] - 5 || rn[i] < (int)(5 + len)) break;
                    if (rbuf[i][0] == 3 && len == 4 && i == 0) { uint32_t keys; memcpy(&keys, rbuf[i] + 5, 4); sSh->keyInput = keys & 0x3FF; }
                    else if ((rbuf[i][0] == 6 || rbuf[i][0] == 3) && len == 4 && i == 1) { uint32_t keys; memcpy(&keys, rbuf[i] + 5, 4); sSh->coopKeyInput = keys & 0x3FF; sSh->coopConnected = 1; }
                    else if (rbuf[i][0] == 5 && len == 4 && i == 0) { uint32_t sp; memcpy(&sp, rbuf[i] + 5, 4); sSh->speed = sp < 1 ? 1 : (sp > 32 ? 32 : sp); }
                    else if (rbuf[i][0] == 4) sSh->quit = 1;
                    memmove(rbuf[i], rbuf[i] + 5 + len, (size_t)(rn[i] - 5 - (int)len)); rn[i] -= 5 + (int)len;
                }
            }
            if (!did) usleep(2000);
        }
        for (int i = 0; i < 2; i++) if (fd[i] >= 0) close(fd[i]);
        sSh->coopConnected = 0;
        sSh->coopKeyInput = 0x3FF;
        sSh->keyInput = 0x3FF; sSh->speed = 1;
        fprintf(stderr, "[pc] visualizadores desconectados\n");
        continue;
        /* código antigo mantido abaixo temporariamente durante a transição */
        /*
        sSh->audioRead = sSh->audioWrite;              // descarta áudio acumulado enquanto ninguém ouvia
        uint32_t lastFrame = 0xFFFFFFFFu; uint8_t rbuf[64]; int rn = 0, ok = 1; uint8_t hdr[5];
        while (ok) {
            int did = 0;
            uint32_t fc = sSh->frameCounter;
            if (fc != lastFrame) {
                lastFrame = fc; memcpy(frameBuf, sSh->frame, sizeof frameBuf);
                uint32_t len = 4 + sizeof frameBuf; hdr[0] = 1; memcpy(hdr + 1, &len, 4);
                ok = SendAll(fd, hdr, 5) && SendAll(fd, &fc, 4) && SendAll(fd, frameBuf, sizeof frameBuf); did = 1;
            }
            uint32_t w = __atomic_load_n(&sSh->audioWrite, __ATOMIC_ACQUIRE), r = sSh->audioRead;
            if (ok && w != r) {
                uint32_t n = w - r; if (n > PC_AUDIO_RING_FRAMES) { r = w - PC_AUDIO_RING_FRAMES; n = PC_AUDIO_RING_FRAMES; }
                for (uint32_t i = 0; i < n; i++) { uint32_t pos = (r + i) % PC_AUDIO_RING_FRAMES; abuf[i * 2] = sSh->audio[pos * 2]; abuf[i * 2 + 1] = sSh->audio[pos * 2 + 1]; }
                uint32_t len = n * 4; hdr[0] = 2; memcpy(hdr + 1, &len, 4);
                ok = SendAll(fd, hdr, 5) && SendAll(fd, abuf, len);
                __atomic_store_n(&sSh->audioRead, w, __ATOMIC_RELEASE); did = 1;
            }
            ssize_t k = recv(fd, rbuf + rn, sizeof rbuf - (size_t)rn, MSG_DONTWAIT);
            if (k == 0) ok = 0;
            else if (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) ok = 0;
            else if (k > 0) rn += (int)k;
            while (rn >= 5) {
                uint32_t len; memcpy(&len, rbuf + 1, 4);
                if (len > sizeof rbuf - 5) { rn = 0; break; }                      // pacote inesperado: descarta o buffer
                if (rn < (int)(5 + len)) break;
                if (rbuf[0] == 3 && len == 4) { uint32_t keys; memcpy(&keys, rbuf + 5, 4); sSh->keyInput = keys & 0x3FF; }
                else if (rbuf[0] == 5 && len == 4) { uint32_t sp; memcpy(&sp, rbuf + 5, 4); sSh->speed = sp < 1 ? 1 : (sp > 32 ? 32 : sp); }
                else if (rbuf[0] == 4) sSh->quit = 1;
                memmove(rbuf, rbuf + 5 + len, (size_t)(rn - 5 - (int)len)); rn -= 5 + (int)len;
            }
            if (!did) usleep(2000);
        }
        close(fd);
        sSh->keyInput = 0x3FF; sSh->speed = 1;
        fprintf(stderr, "[pc] visualizador desconectado\n");
        */
    }
}

void PcNetStart(struct PcShared *sh) {
    const char *p = getenv("PC_NET_PORT");
    if (!p || !sh) return;
    sSh = sh;
    pthread_t th;
    if (pthread_create(&th, NULL, Bridge, (void *)(intptr_t)atoi(p)) == 0) pthread_detach(th);
}
