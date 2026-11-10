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
#define PKT_EVENT 3

static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static struct PcCoopPlayerState sLocal, sRemote;
static uint32_t sLocalVer;
static volatile int sConnected;
static int sStarted, sHostPort;
static char sJoinHost[128], sJoinPort[16];

#define PC_QSIZE 4096
static struct PcCoopSyncEvent sOut[PC_QSIZE], sIn[PC_QSIZE];
static unsigned sOutHead, sOutTail, sInHead, sInTail;
static volatile int sNewConn;
static int PopOut(struct PcCoopSyncEvent *e) {
    int ok = 0;
    pthread_mutex_lock(&sLock);
    if (sOutHead != sOutTail) { *e = sOut[sOutTail % PC_QSIZE]; sOutTail++; ok = 1; }
    pthread_mutex_unlock(&sLock);
    return ok;
}

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

static int SendEvent(int fd, const struct PcCoopSyncEvent *e) {
    uint8_t m[12];
    m[0] = PKT_EVENT; m[1] = 9; m[2] = 0;
    m[3] = e->kind; m[4] = (uint8_t)(e->index & 0xFF); m[5] = (uint8_t)(e->index >> 8); m[6] = (uint8_t)(e->value & 0xFF); m[7] = (uint8_t)(e->value >> 8);
    m[8] = (uint8_t)(e->a & 0xFF); m[9] = (uint8_t)(e->a >> 8); m[10] = (uint8_t)(e->b & 0xFF); m[11] = (uint8_t)(e->b >> 8);
    return SendAll(fd, m, sizeof m);
}

/* ---------- cabo link virtual ---------- */
#define PKT_LINK 7
#define PC_LINK_SLOTS 64
#define PC_LINK_MAX 4096
struct PcLinkSlot { uint8_t kind, sender; uint16_t size; uint8_t data[PC_LINK_MAX]; };
static struct PcLinkSlot sLinkOut[PC_LINK_SLOTS], sLinkIn[PC_LINK_SLOTS];
static unsigned sLinkOutHead, sLinkOutTail, sLinkInHead, sLinkInTail;
static int sLinkPending;
static int sMyLinkId = 1;

static int LinkIsHost(void) { return getenv("PC_COOP_HOST") != NULL; }

/* chamar com sLock */
static int LinkPush(struct PcLinkSlot *q, unsigned *head, unsigned *tail, uint8_t kind, uint8_t sender, const void *data, unsigned size) {
    struct PcLinkSlot *s;
    if (size > PC_LINK_MAX || *head - *tail >= PC_LINK_SLOTS) { fprintf(stderr, "[pc] link: fila cheia ou bloco grande demais (%u bytes)\n", size); return 0; }
    s = &q[*head % PC_LINK_SLOTS];
    s->kind = kind; s->sender = sender; s->size = (uint16_t)size; memcpy(s->data, data, size);
    (*head)++;
    return 1;
}

static int LinkPopOut(uint8_t *kind, uint8_t *sender, uint8_t *buf, unsigned *size) {
    int ok = 0;
    pthread_mutex_lock(&sLock);
    if (sLinkOutHead != sLinkOutTail) {
        struct PcLinkSlot *s = &sLinkOut[sLinkOutTail % PC_LINK_SLOTS];
        *kind = s->kind; *sender = s->sender; *size = s->size; memcpy(buf, s->data, s->size);
        sLinkOutTail++; ok = 1;
    }
    pthread_mutex_unlock(&sLock);
    return ok;
}

static int HandlePacket(uint8_t type, const uint8_t *p, int len) {
    if (type == PKT_HELLO) { if (len != 4 || memcmp(p, "FRC1", 4) != 0) return 0; sConnected = 1; sNewConn = 1; return 1; }
    if (type == PKT_STATE && len == 10) {
        struct PcCoopPlayerState s; memset(&s, 0, sizeof s);
        s.mapGroup = p[0]; s.mapNum = p[1]; s.facing = p[2]; s.gfx = p[3]; s.elevation = p[4];
        s.x = (int16_t)(p[6] | (p[7] << 8)); s.y = (int16_t)(p[8] | (p[9] << 8)); s.valid = 1;
        pthread_mutex_lock(&sLock); sRemote = s; pthread_mutex_unlock(&sLock);
    }
    if (type == PKT_LINK && len >= 2 && len - 2 <= PC_LINK_MAX) {
        uint8_t kind = p[0], sender = p[1];
        unsigned size = (unsigned)(len - 2);
        pthread_mutex_lock(&sLock);
        if (kind == 0) {                      /* SUBMIT (convidado -> host): o host numera, entrega a si e redistribui */
            if (LinkIsHost()) {
                LinkPush(sLinkIn, &sLinkInHead, &sLinkInTail, 1, sender, p + 2, size);
                LinkPush(sLinkOut, &sLinkOutHead, &sLinkOutTail, 1, sender, p + 2, size);
            }
        } else if (kind == 1) {               /* DELIVER (host -> convidado): mesma ordem do host */
            LinkPush(sLinkIn, &sLinkInHead, &sLinkInTail, 1, sender, p + 2, size);
            if (sender == sMyLinkId && sLinkPending > 0) sLinkPending--;   /* o eco do nosso próprio bloco */
        }
        pthread_mutex_unlock(&sLock);
        return 1;
    }
    if (type == PKT_EVENT && len == 9) {
        struct PcCoopSyncEvent ev;
        ev.kind = p[0]; ev.index = (uint16_t)(p[1] | (p[2] << 8)); ev.value = (uint16_t)(p[3] | (p[4] << 8)); ev.a = (uint16_t)(p[5] | (p[6] << 8)); ev.b = (uint16_t)(p[7] | (p[8] << 8));
        pthread_mutex_lock(&sLock);
        if (sInHead - sInTail < PC_QSIZE) sIn[sInHead++ % PC_QSIZE] = ev;
        pthread_mutex_unlock(&sLock);
    }
    return 1;
}

static void Session(int fd) {
    static uint8_t rbuf[70000]; int rn = 0, one = 1; long lastSend = 0; uint32_t lastVer = 0xFFFFFFFFu;
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
                    if (len > 60000) return;
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
            { struct PcCoopSyncEvent ev; while (PopOut(&ev)) if (!SendEvent(fd, &ev)) return; }
            { uint8_t lk, ls; unsigned lsz; static uint8_t lb[PC_LINK_MAX], lpk[5 + PC_LINK_MAX];
              while (LinkPopOut(&lk, &ls, lb, &lsz)) {
                  lpk[0] = PKT_LINK; lpk[1] = (uint8_t)((lsz + 2) & 0xFF); lpk[2] = (uint8_t)((lsz + 2) >> 8); lpk[3] = lk; lpk[4] = ls;
                  memcpy(lpk + 5, lb, lsz);
                  if (!SendAll(fd, lpk, 5 + lsz)) return;
              } }
            { static int test = -1; static long t0; static unsigned n, seq; uint8_t snd; static uint8_t tb[PC_LINK_MAX]; unsigned tsz;
              if (test < 0) test = getenv("PC_LINK_TEST") != NULL;
              if (test) {
                  long nw = NowMs();
                  if (nw - t0 > 2000) { char msg[40]; t0 = nw; n++; snprintf(msg, sizeof msg, "PING %u de %s", n, LinkIsHost() ? "host" : "convidado"); PcLinkSubmit(msg, (unsigned)strlen(msg) + 1); }
                  while (PcLinkPopDelivered(&snd, tb, &tsz)) fprintf(stderr, "[pc] link-test #%u: do jogador %d: %s\n", ++seq, snd, (char *)tb);
              } }
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
        pthread_mutex_lock(&sLock); memset(&sRemote, 0, sizeof sRemote); sOutHead = sOutTail = 0; sLinkOutHead = sLinkOutTail = sLinkInHead = sLinkInTail = 0; sLinkPending = 0; pthread_mutex_unlock(&sLock);
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
    fprintf(stderr, "[pc] coop: ativo como %s\n", h ? "HOST" : "CONVIDADO");
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

void PcCoopNetSendEvent(const struct PcCoopSyncEvent *e) {
    pthread_mutex_lock(&sLock);
    if (sConnected && sOutHead - sOutTail < PC_QSIZE) sOut[sOutHead++ % PC_QSIZE] = *e;
    pthread_mutex_unlock(&sLock);
}
int PcCoopNetPollEvent(struct PcCoopSyncEvent *e) {
    int ok = 0;
    pthread_mutex_lock(&sLock);
    if (sInHead != sInTail) { *e = sIn[sInTail % PC_QSIZE]; sInTail++; ok = 1; }
    pthread_mutex_unlock(&sLock);
    return ok;
}
int PcCoopNetJustConnected(void) { int r = sNewConn; if (r) sNewConn = 0; return r; }

int PcLinkAvailable(void) { return PcCoopNetActive(); }
int PcLinkIsHost(void) { return LinkIsHost(); }
int PcLinkConnected(void) { return sConnected; }
int PcLinkSubmit(const void *data, unsigned size) {
    int ok;
    pthread_mutex_lock(&sLock);
    if (!sConnected) ok = 0;
    else if (LinkIsHost()) {                  /* host: numera na hora; entrega a si mesmo e manda ao convidado */
        ok = LinkPush(sLinkIn, &sLinkInHead, &sLinkInTail, 1, 0, data, size)
          && LinkPush(sLinkOut, &sLinkOutHead, &sLinkOutTail, 1, 0, data, size);
    } else {                                  /* convidado: manda ao host e espera o eco */
        ok = LinkPush(sLinkOut, &sLinkOutHead, &sLinkOutTail, 0, (uint8_t)sMyLinkId, data, size);
        if (ok) sLinkPending++;
    }
    pthread_mutex_unlock(&sLock);
    return ok;
}
int PcLinkPopDelivered(uint8_t *sender, void *buf, unsigned *size) {
    int ok = 0;
    pthread_mutex_lock(&sLock);
    if (sLinkInHead != sLinkInTail) {
        struct PcLinkSlot *s = &sLinkIn[sLinkInTail % PC_LINK_SLOTS];
        *sender = s->sender; *size = s->size; memcpy(buf, s->data, s->size);
        sLinkInTail++; ok = 1;
    }
    pthread_mutex_unlock(&sLock);
    return ok;
}
int PcLinkSubmitPending(void) { int n; pthread_mutex_lock(&sLock); n = sLinkPending; pthread_mutex_unlock(&sLock); return n; }

void PcLinkSetMyId(int id) { sMyLinkId = id; }
int PcLinkSubmitAs(int sender, const void *data, unsigned size) {
    int ok = 0;
    pthread_mutex_lock(&sLock);
    if (sConnected && LinkIsHost())
        ok = LinkPush(sLinkIn, &sLinkInHead, &sLinkInTail, 1, (uint8_t)sender, data, size)
          && LinkPush(sLinkOut, &sLinkOutHead, &sLinkOutTail, 1, (uint8_t)sender, data, size);
    pthread_mutex_unlock(&sLock);
    return ok;
}
