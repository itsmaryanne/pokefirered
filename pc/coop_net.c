// Rede do coop: liga dois jogos (host <-> convidado) por TCP e troca o estado do jogador.
// Pacote: [tipo u8][tamanho u16 LE][dados]. 1 = HELLO ("FRC1"), 2 = STATE (10 bytes).
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "coop_net.h"

#define PKT_HELLO 1
#define PKT_STATE 2

static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static struct PcCoopPlayerState sLocal, sRemote;
static uint32_t sLocalVer;
static volatile int sConnected;
static int sStarted, sHostPort;
static char sJoinHost[128], sJoinPort[16];

static long NowMs(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L; }

static int SendAll(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) { ssize_t k = send(fd, c, n, MSG_NOSIGNAL); if (k <= 0) { if (k < 0 && errno == EINTR) continue; return 0; } c += k; n -= (size_t)k; }
    return 1;
}
static int SendHello(int fd) { static const uint8_t m[7] = { PKT_HELLO, 4, 0, 'F', 'R', 'C', '1' }; return SendAll(fd, m, sizeof m); }
static int SendState(int fd, const struct PcCoopPlayerState *s) {
    uint8_t m[13];
    m[0] = PKT_STATE; m[1] = 10; m[2] = 0;
    m[3] = s->mapGroup; m[4] = s->mapNum; m[5] = s->facing; m[6] = s->gfx; m[7] = s->elevation; m[8] = 0;
    m[9] = (uint8_t)(s->x & 0xFF); m[10] = (uint8_t)(((uint16_t)s->x) >> 8);
    m[11] = (uint8_t)(s->y & 0xFF); m[12] = (uint8_t)(((uint16_t)s->y) >> 8);
    return SendAll(fd, m, sizeof m);
}

static int HandlePacket(uint8_t type, const uint8_t *p, int len) {
    if (type == PKT_HELLO) { if (len != 4 || memcmp(p, "FRC1", 4) != 0) return 0; sConnected = 1; return 1; }
    if (type == PKT_STATE && len == 10) {
        struct PcCoopPlayerState s; memset(&s, 0, sizeof s);
        s.mapGroup = p[0]; s.mapNum = p[1]; s.facing = p[2]; s.gfx = p[3]; s.elevation = p[4];
        s.x = (int16_t)(p[6] | (p[7] << 8)); s.y = (int16_t)(p[8] | (p[9] << 8)); s.valid = 1;
        pthread_mutex_lock(&sLock); sRemote = s; pthread_mutex_unlock(&sLock);
    }
    return 1;
}

static void Session(int fd) {
    uint8_t rbuf[512]; int rn = 0, one = 1; long lastSend = 0; uint32_t lastVer = 0xFFFFFFFFu;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (!SendHello(fd)) return;
    for (;;) {
        struct pollfd pf; pf.fd = fd; pf.events = POLLIN; pf.revents = 0;
        int r = poll(&pf, 1, 5);
        if (r < 0 && errno != EINTR) return;
        if (r > 0) {
            if (pf.revents & POLLIN) {
                ssize_t k = recv(fd, rbuf + rn, sizeof rbuf - (size_t)rn, 0);
                if (k <= 0) return;
                rn += (int)k;
                while (rn >= 3) {
                    int len = rbuf[1] | (rbuf[2] << 8);
                    if (len > 200) return;
                    if (rn < 3 + len) break;
                    if (!HandlePacket(rbuf[0], rbuf + 3, len)) return;
                    memmove(rbuf, rbuf + 3 + len, (size_t)(rn - 3 - len)); rn -= 3 + len;
                }
            } else if (pf.revents & (POLLERR | POLLHUP | POLLNVAL)) return;
        }
        if (sConnected) {
            struct PcCoopPlayerState l; uint32_t ver; long now = NowMs();
            pthread_mutex_lock(&sLock); l = sLocal; ver = sLocalVer; pthread_mutex_unlock(&sLock);
            if (l.valid && (ver != lastVer || now - lastSend > 1000)) { if (!SendState(fd, &l)) return; lastVer = ver; lastSend = now; }
        }
    }
}

static int ListenOn(int port) {
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (ls < 0) return -1;
    fcntl(ls, F_SETFD, FD_CLOEXEC);                    /* o SoftReset (exec) nao pode herdar o socket */
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    const char *b = getenv("PC_COOP_BIND"); a.sin_addr.s_addr = b ? inet_addr(b) : htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 1) < 0) { perror("[pc] coop: bind/listen"); close(ls); return -1; }
    fprintf(stderr, "[pc] coop: host aguardando o convidado em %s:%d\n", b ? b : "127.0.0.1", port);
    return ls;
}
static int ConnectTo(void) {
    struct addrinfo hints, *res = NULL; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(sJoinHost, sJoinPort, &hints, &res) != 0 || !res) return -1;
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { freeaddrinfo(res); return -1; }
    fcntl(s, F_SETFD, FD_CLOEXEC);
    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) { close(s); freeaddrinfo(res); return -1; }
    freeaddrinfo(res);
    return s;
}

static void *NetThread(void *arg) {
    (void)arg;
    sigset_t m; sigemptyset(&m); sigaddset(&m, SIGUSR1); sigaddset(&m, SIGUSR2); pthread_sigmask(SIG_BLOCK, &m, NULL);
    int ls = -1;
    if (sHostPort) { ls = ListenOn(sHostPort); if (ls < 0) return NULL; }
    for (;;) {
        int fd;
        if (ls >= 0) { fd = accept(ls, NULL, NULL); if (fd < 0) { usleep(100000); continue; } }
        else { fd = ConnectTo(); if (fd < 0) { sleep(1); continue; } }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        fprintf(stderr, "[pc] coop: conectado\n");
        Session(fd);
        close(fd);
        sConnected = 0;
        pthread_mutex_lock(&sLock); memset(&sRemote, 0, sizeof sRemote); pthread_mutex_unlock(&sLock);
        fprintf(stderr, "[pc] coop: desconectado\n");
        if (ls < 0) sleep(1);
    }
    return NULL;
}

int PcCoopNetActive(void) { return getenv("PC_COOP_HOST") != NULL || getenv("PC_COOP_JOIN") != NULL; }

void PcCoopNetStart(void) {
    const char *h = getenv("PC_COOP_HOST"), *j = getenv("PC_COOP_JOIN");
    pthread_t th;
    if (sStarted || (!h && !j)) return;
    if (h) sHostPort = atoi(h);
    else {
        const char *colon = strrchr(j, ':');
        size_t n = colon ? (size_t)(colon - j) : strlen(j);
        if (n >= sizeof sJoinHost) n = sizeof sJoinHost - 1;
        memcpy(sJoinHost, j, n); sJoinHost[n] = 0;
        snprintf(sJoinPort, sizeof sJoinPort, "%s", colon ? colon + 1 : "47500");
    }
    sStarted = 1;
    if (pthread_create(&th, NULL, NetThread, NULL) == 0) pthread_detach(th);
}

void PcCoopNetSetLocal(const struct PcCoopPlayerState *s) {
    pthread_mutex_lock(&sLock);
    if (memcmp(&sLocal, s, sizeof sLocal) != 0) { sLocal = *s; sLocalVer++; }
    pthread_mutex_unlock(&sLock);
}
int PcCoopNetGetRemote(struct PcCoopPlayerState *out) {
    int ok;
    pthread_mutex_lock(&sLock); *out = sRemote; ok = sConnected && sRemote.valid; pthread_mutex_unlock(&sLock);
    return ok;
}
