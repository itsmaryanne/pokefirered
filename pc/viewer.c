// Visualizador do port PC (SDL2): mostra os quadros do jogo, toca o som e envia as teclas.
// Dois jeitos de falar com o jogo:
//   - memória compartilhada (padrão no Linux/WSLg);
//   - TCP (padrão no Windows):  pc_viewer 127.0.0.1:47474   (ou PC_VIEWER_CONNECT=host:porta)
// Teclas: Z=A X=B Enter=Start Backspace/ShiftDir=Select setas A=L S=R | F3 = FPS | M = mudo | F11 = tela cheia | Esc = sair
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "shared.h"
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define BAD_SOCK INVALID_SOCKET
  #define CLOSESOCK(s) closesocket(s)
  #define SEND_FLAGS 0
#else
  #include <fcntl.h>
  #include <unistd.h>
  #include <signal.h>
  #include <errno.h>
  #include <sys/mman.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <netdb.h>
  typedef int sock_t;
  #define BAD_SOCK (-1)
  #define CLOSESOCK(s) close(s)
  #define SEND_FLAGS MSG_NOSIGNAL
#endif

enum { K_A, K_B, K_SELECT, K_START, K_RIGHT, K_LEFT, K_UP, K_DOWN, K_R, K_L };
static uint32_t kbKeys, padKeys;

static struct PcShared gLocal;                       // modo TCP: cópia local do estado do jogo
static struct PcShared *gShared;
static int gTcp;
static volatile int gMuted, gShowFps, gConnected, gQuitNet;
static volatile int gFast;                           /* Tab segurado = avanço rápido */
static volatile uint32_t gAudioIn, gAudioOut;        // diagnóstico: quadros de áudio recebidos / consumidos
static int gCoopViewer;
static sock_t gSock = BAD_SOCK;
static char gHost[128] = "127.0.0.1", gPort[16] = "47474";

static int KeyBit(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_Z: return K_A;            case SDL_SCANCODE_X: return K_B;
    case SDL_SCANCODE_BACKSPACE: case SDL_SCANCODE_RSHIFT: return K_SELECT;
    case SDL_SCANCODE_RETURN: return K_START;
    case SDL_SCANCODE_RIGHT: return K_RIGHT;    case SDL_SCANCODE_LEFT: return K_LEFT;
    case SDL_SCANCODE_UP: return K_UP;          case SDL_SCANCODE_DOWN: return K_DOWN;
    case SDL_SCANCODE_S: return K_R;            case SDL_SCANCODE_A: return K_L;
    default: return -1; }
}

/* ------------------------------ texto do FPS (fonte 3x5) ------------------------------ */
static const uint8_t *Glyph(char c) {
    static const uint8_t g[][5] = {
        {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}, /* 0-9 */
        {0,0,0,0,2},                                   /* . */
        {7,4,5,5,7},{2,5,7,5,5},{5,7,7,5,5},{7,4,7,4,7},{5,5,5,5,2},{7,2,2,2,7},{5,5,7,7,5},{5,5,5,5,7},{6,5,5,5,6}, /* G A M E V I W U D */
    };
    if (c >= '0' && c <= '9') return g[c - '0'];
    switch (c) { case '.': return g[10]; case 'G': return g[11]; case 'A': return g[12]; case 'M': return g[13]; case 'E': return g[14];
                 case 'V': return g[15]; case 'I': return g[16]; case 'W': return g[17]; case 'U': return g[18]; case 'D': return g[19]; }
    return NULL;
}
static void PutPx(uint32_t *img, int x, int y, uint32_t c) { if (x >= 0 && x < 240 && y >= 0 && y < 160) img[y * 240 + x] = c; }
static void OverlayText(uint32_t *img, int x, int y, const char *s) {
    for (int pass = 0; pass < 2; pass++)              // sombra preta e depois branco
        for (int cx = x, i = 0; s[i]; i++, cx += 4) {
            const uint8_t *g = Glyph(s[i]); if (!g) continue;
            for (int r = 0; r < 5; r++) for (int c = 0; c < 3; c++) if (g[r] & (4 >> c))
                PutPx(img, cx + c + (pass == 0), y + r + (pass == 0), pass == 0 ? 0xFF000000u : 0xFFFFFFFFu);
        }
}

/* ------------------------------------ áudio ------------------------------------ */
#define AUD_TARGET 2048                               /* ~62 ms de buffer-alvo */
// Consome o anel (estéreo s16, 32768 Hz) com interpolação linear e taxa adaptativa (±6%) que mantém o nível do buffer
// perto do alvo; em falta de dados segura a última amostra decaindo (sem estalo) e só re-carrega se faltar muito.
static void AudioCb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *out = (int16_t *)stream; int frames = len / 4;
    static int primed; static double frac; static int16_t lastL, lastR;
    struct PcShared *sh = gShared;
    if (gFast) { memset(stream, 0, (size_t)len); __atomic_store_n(&sh->audioRead, __atomic_load_n(&sh->audioWrite, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE); primed = 0; frac = 0; return; }
    uint32_t w = __atomic_load_n(&sh->audioWrite, __ATOMIC_ACQUIRE), r = sh->audioRead, avail = w - r;
    if (avail > 9000) { r = w - AUD_TARGET; avail = AUD_TARGET; frac = 0; }
    if (!primed) { if (avail < AUD_TARGET) { memset(stream, 0, (size_t)len); return; } primed = 1; }
    double ratio = 1.0 + 0.06 * ((double)avail - AUD_TARGET) / AUD_TARGET;
    if (ratio > 1.06) ratio = 1.06; if (ratio < 0.94) ratio = 0.94;
    int i = 0;
    for (; i < frames; i++) {
        if ((uint32_t)(w - r) < 2) break;
        uint32_t p0 = r % PC_AUDIO_RING_FRAMES, p1 = (r + 1) % PC_AUDIO_RING_FRAMES;
        double l = sh->audio[p0 * 2] + (sh->audio[p1 * 2] - sh->audio[p0 * 2]) * frac;
        double rr = sh->audio[p0 * 2 + 1] + (sh->audio[p1 * 2 + 1] - sh->audio[p0 * 2 + 1]) * frac;
        lastL = (int16_t)l; lastR = (int16_t)rr;
        out[i * 2] = gMuted ? 0 : lastL; out[i * 2 + 1] = gMuted ? 0 : lastR;
        frac += ratio; uint32_t adv = (uint32_t)frac; frac -= adv; r += adv;
    }
    if (i < frames) {
        int missing = frames - i;
        for (int k = 0; k < missing; k++) { float g = 1.0f - (float)k / (float)missing; out[(i + k) * 2] = gMuted ? 0 : (int16_t)(lastL * g); out[(i + k) * 2 + 1] = gMuted ? 0 : (int16_t)(lastR * g); }
        if (missing > frames / 2) { primed = 0; frac = 0; }
    }
    gAudioOut += r - sh->audioRead;
    __atomic_store_n(&sh->audioRead, r, __ATOMIC_RELEASE);
}

/* ------------------------------------- rede ------------------------------------- */
static int RecvAll(sock_t s, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) { int k = (int)recv(s, (char *)p, (int)n, 0); if (k <= 0) return 0; p += k; n -= (size_t)k; }
    return 1;
}
static int Discard(sock_t s, uint32_t n) {
    uint8_t tmp[4096];
    while (n) { uint32_t c = n < sizeof tmp ? n : (uint32_t)sizeof tmp; if (!RecvAll(s, tmp, c)) return 0; n -= c; }
    return 1;
}
static void SendMsg(uint8_t type, uint32_t value) {
    if (!gTcp || !gConnected) return;
    uint8_t m[9]; uint32_t len = 4; m[0] = type; memcpy(m + 1, &len, 4); memcpy(m + 5, &value, 4);
    send(gSock, (const char *)m, 9, SEND_FLAGS);
}
static int NetThread(void *unused) {
    (void)unused;
    static uint8_t abuf[65536];
    while (!gQuitNet) {
        struct addrinfo hints, *res = NULL; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(gHost, gPort, &hints, &res) != 0 || !res) { SDL_Delay(500); continue; }
        sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s == BAD_SOCK || connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) { if (s != BAD_SOCK) CLOSESOCK(s); freeaddrinfo(res); SDL_Delay(500); continue; }
        freeaddrinfo(res);
        int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        gLocal.audioWrite = 0; gLocal.audioRead = 0;
        gSock = s; gConnected = 1;
        uint8_t hdr[5];
        while (RecvAll(s, hdr, 5)) {
            uint32_t len; memcpy(&len, hdr + 1, 4);
            if (hdr[0] == 1 && len == 4 + sizeof gLocal.frame) {
                uint32_t fn;
                if (!RecvAll(s, &fn, 4) || !RecvAll(s, gLocal.frame, sizeof gLocal.frame)) break;
                gLocal.frameCounter = fn;
            } else if (hdr[0] == 2 && len <= sizeof abuf) {
                if (!RecvAll(s, abuf, len)) break;
                uint32_t n = len / 4, w = gLocal.audioWrite, r = gLocal.audioRead;
                if (w - r + n <= PC_AUDIO_RING_FRAMES) {
                    const int16_t *src = (const int16_t *)abuf;
                    for (uint32_t i = 0; i < n; i++) { uint32_t pos = (w + i) % PC_AUDIO_RING_FRAMES; gLocal.audio[pos * 2] = src[i * 2]; gLocal.audio[pos * 2 + 1] = src[i * 2 + 1]; }
                    __atomic_store_n(&gLocal.audioWrite, w + n, __ATOMIC_RELEASE); gAudioIn += n;
                }
            } else if (!Discard(s, len)) break;
        }
        gConnected = 0; gSock = BAD_SOCK; CLOSESOCK(s);
    }
    return 0;
}

/* ---------------------------------- controle/script ---------------------------------- */
static SDL_GameController *pad;
static void PadUpdate(void) {
    padKeys = 0; if (!pad) return;
#define B(btn, bit) if (SDL_GameControllerGetButton(pad, btn)) padKeys |= 1u << bit
    B(SDL_CONTROLLER_BUTTON_B, K_A); B(SDL_CONTROLLER_BUTTON_A, K_B);                 // posição física: leste = A, sul = B
    B(SDL_CONTROLLER_BUTTON_BACK, K_SELECT); B(SDL_CONTROLLER_BUTTON_START, K_START);
    B(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, K_RIGHT); B(SDL_CONTROLLER_BUTTON_DPAD_LEFT, K_LEFT);
    B(SDL_CONTROLLER_BUTTON_DPAD_UP, K_UP); B(SDL_CONTROLLER_BUTTON_DPAD_DOWN, K_DOWN);
    B(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, K_R); B(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, K_L);
#undef B
    int lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    if (lx > 16000) padKeys |= 1u << K_RIGHT;
    if (lx < -16000) padKeys |= 1u << K_LEFT;
    if (ly > 16000) padKeys |= 1u << K_DOWN;
    if (ly < -16000) padKeys |= 1u << K_UP;
}

// Roteiro de teste sem teclado/tela: PC_VIEWER_SCRIPT="900:down:enter,906:up:enter,1000:snap:/tmp/a.bmp,1200:quit"
// (o número é o contador de quadros do jogo; as teclas entram como eventos SDL, o mesmo caminho do teclado real)
struct Step { unsigned frame; char act[8]; char arg[200]; int done; } steps[2048]; int nsteps;
static SDL_Scancode ScFromName(const char *n) {
    if (!strcmp(n, "enter")) return SDL_SCANCODE_RETURN;
    if (!strcmp(n, "z")) return SDL_SCANCODE_Z;
    if (!strcmp(n, "x")) return SDL_SCANCODE_X;
    if (!strcmp(n, "up")) return SDL_SCANCODE_UP;
    if (!strcmp(n, "down")) return SDL_SCANCODE_DOWN;
    if (!strcmp(n, "left")) return SDL_SCANCODE_LEFT;
    if (!strcmp(n, "right")) return SDL_SCANCODE_RIGHT;
    if (!strcmp(n, "f3")) return SDL_SCANCODE_F3;
    if (!strcmp(n, "m")) return SDL_SCANCODE_M;
    return SDL_SCANCODE_UNKNOWN;
}
static void ParseScript(const char *s) {
    char *c = strdup(s), *t = c;
    while (t && *t && nsteps < 2048) {
        char *comma = strchr(t, ','); if (comma) *comma = 0;
        struct Step *st = &steps[nsteps]; memset(st, 0, sizeof *st);
        char *a = strchr(t, ':');
        if (a) {
            *a++ = 0; st->frame = (unsigned)atoi(t);
            char *b = strchr(a, ':'); if (b) { *b++ = 0; strncpy(st->arg, b, sizeof st->arg - 1); }
            strncpy(st->act, a, sizeof st->act - 1); nsteps++;
        }
        t = comma ? comma + 1 : NULL;
    }
    free(c);
}

/* ------------------------------------- principal ------------------------------------- */
int main(int argc, char **argv) {
    const char *hp = argc > 1 ? argv[1] : getenv("PC_VIEWER_CONNECT");
#ifdef _WIN32
    if (!hp) hp = "127.0.0.1:47474";
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    if (hp) {
        gTcp = 1;
        const char *colon = strrchr(hp, ':');
        if (colon) { size_t n = (size_t)(colon - hp); if (n >= sizeof gHost) n = sizeof gHost - 1; memcpy(gHost, hp, n); gHost[n] = 0; snprintf(gPort, sizeof gPort, "%s", colon + 1); }
        else snprintf(gHost, sizeof gHost, "%s", hp);
        gShared = &gLocal;
        gCoopViewer = getenv("PC_COOP_PLAYER") && atoi(getenv("PC_COOP_PLAYER")) == 2;
    }
#ifndef _WIN32
    if (!gTcp) {                                    // memória compartilhada: o objeto precisa existir, ter o magic certo e o pid dele estar vivo
        struct PcShared *sh = NULL;
        for (int i = 0; i < 300; i++) {
            if (!sh) {
                int fd = shm_open(PC_SHM_NAME, O_RDWR, 0600);
                if (fd >= 0) { void *p = mmap(NULL, sizeof(struct PcShared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); close(fd); if (p != MAP_FAILED) sh = p; }
            }
            if (sh && sh->magic == PC_SHM_MAGIC && sh->gamePid && (kill((pid_t)sh->gamePid, 0) == 0 || errno == EPERM)) break;
            usleep(100000);
            if (i == 299) { fprintf(stderr, "viewer: o jogo não respondeu em 30 s (sem %s)\n", PC_SHM_NAME); return 1; }
        }
        gShared = sh;
    }
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "viewer: SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_Window *win = SDL_CreateWindow("Pokémon FireRed (PC)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 720, 480, SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "viewer: janela: %s\n", SDL_GetError()); return 1; }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "viewer: renderer: %s\n", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(ren, 240, 160); SDL_RenderSetIntegerScale(ren, SDL_TRUE);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 240, 160);
    if (gTcp) { fprintf(stderr, "viewer: conectando a %s:%s ...\n", gHost, gPort); SDL_CreateThread(NetThread, "net", NULL); }

    SDL_AudioDeviceID adev = 0;
    { SDL_AudioSpec want, have; memset(&want, 0, sizeof want);
      want.freq = PC_AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.callback = AudioCb;
      const char *ab = getenv("PC_AUDIO_BUF"); want.samples = (Uint16)(ab ? atoi(ab) : 1024);
      adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);                  /* allowed_changes = 0: o SDL converte para o dispositivo */
      if (adev) SDL_PauseAudioDevice(adev, 0); else fprintf(stderr, "viewer: sem áudio (%s)\n", SDL_GetError()); }
    const char *scr = getenv("PC_VIEWER_SCRIPT"); if (scr) ParseScript(scr);

    static uint32_t disp[240 * 160];
    uint32_t last = 0xFFFFFFFFu; int running = 1, fullscreen = 0, dirty = 1;
    Uint32 tStat = SDL_GetTicks(); uint32_t fcStat = 0, presents = 0, stalledMs = 0; double gameFps = 0, viewFps = 0; int gameDead = 0, gameStalled = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                int down = e.type == SDL_KEYDOWN;
                if (e.key.keysym.scancode == SDL_SCANCODE_TAB) gFast = down;
                if (down && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) running = 0;
                if (down && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_M) gMuted = !gMuted;
                if (down && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_F3) { gShowFps = !gShowFps; dirty = 1; }
                if (down && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_F11) { fullscreen = !fullscreen; SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); }
                int b = KeyBit(e.key.keysym.scancode); if (b >= 0) { if (down) kbKeys |= 1u << b; else kbKeys &= ~(1u << b); }
            } else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
            else if (e.type == SDL_CONTROLLERDEVICEREMOVED && pad) { SDL_GameControllerClose(pad); pad = NULL; }
        }
        PadUpdate();
        uint32_t keys = ~(kbKeys | padKeys) & 0x3FF;
        if (gTcp) {
            static uint32_t lastKeys = 0xFFFFFFFFu; static Uint32 lastSend; Uint32 now = SDL_GetTicks();
            if (!gConnected) lastKeys = 0xFFFFFFFFu;
            else if (keys != lastKeys || now - lastSend > 500) { SendMsg(gCoopViewer ? 6 : 3, keys); lastKeys = keys; lastSend = now; }
        } else gShared->keyInput = keys;
        { static uint32_t spSent = 1; uint32_t sp = gFast ? 8 : 1;
          if (gTcp && !gConnected) spSent = 0xFFFFFFFFu;
          else if (sp != spSent) { if (gTcp) SendMsg(5, sp); else gShared->speed = sp; spSent = sp; } }

        uint32_t fc = gShared->frameCounter;
        for (int i = 0; i < nsteps; i++) if (!steps[i].done && fc >= steps[i].frame) {
            steps[i].done = 1;
            if (!strcmp(steps[i].act, "down") || !strcmp(steps[i].act, "up")) {
                SDL_Event ke; memset(&ke, 0, sizeof ke); ke.type = steps[i].act[0] == 'd' ? SDL_KEYDOWN : SDL_KEYUP;
                ke.key.keysym.scancode = ScFromName(steps[i].arg); SDL_PushEvent(&ke);
            } else if (!strcmp(steps[i].act, "snap")) {
                SDL_Surface *sf = SDL_CreateRGBSurfaceFrom(gShowFps ? (void *)disp : (void *)gShared->frame, 240, 160, 32, 240 * 4, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
                if (sf) { SDL_SaveBMP(sf, steps[i].arg); SDL_FreeSurface(sf); fprintf(stderr, "viewer: snapshot %s no quadro %u\n", steps[i].arg, fc); }
            } else if (!strcmp(steps[i].act, "quit")) running = 0;
        }

        // estatísticas a cada 0,5 s (FPS, estado do jogo) e título da janela
        Uint32 now = SDL_GetTicks();
        if (now - tStat >= 500) {
            double dt = (now - tStat) / 1000.0;
            gameFps = (uint32_t)(fc - fcStat) / dt; viewFps = presents / dt; fcStat = fc; presents = 0; tStat = now;
            if (fc == last && gameFps == 0) stalledMs += 500; else stalledMs = 0;
            gameStalled = stalledMs >= 3000;
#ifndef _WIN32
            if (!gTcp) gameDead = !(kill((pid_t)gShared->gamePid, 0) == 0 || errno == EPERM);
#else
            gameDead = 0;
#endif
            char title[160], fps[48] = "", st[96] = "";
            if (gShowFps) snprintf(fps, sizeof fps, " - %.1f fps", gameFps);
            if (gTcp && !gConnected) snprintf(st, sizeof st, " - aguardando o jogo em %s:%s", gHost, gPort);
            else if (gameDead) snprintf(st, sizeof st, " - O JOGO PAROU (veja o terminal)");
            else if (gameStalled) snprintf(st, sizeof st, " - O JOGO NAO RESPONDE (veja o terminal)");
            snprintf(title, sizeof title, "Pokémon FireRed (PC) - quadro %u%s%s", fc, fps, st);
            SDL_SetWindowTitle(win, title);
            if (gShowFps) dirty = 1;
        }
        if (fc != last || dirty) {
            last = fc; dirty = 0;
            if (gShowFps) {
                memcpy(disp, (const void *)gShared->frame, sizeof disp);
                char l1[32], l2[32]; snprintf(l1, sizeof l1, "GAME %.1f", gameFps); snprintf(l2, sizeof l2, "VIEW %.1f", viewFps);
                OverlayText(disp, 2, 2, l1); OverlayText(disp, 2, 9, l2);
                SDL_UpdateTexture(tex, NULL, disp, 240 * 4);
            } else SDL_UpdateTexture(tex, NULL, gShared->frame, 240 * 4);
        }
        SDL_RenderClear(ren); SDL_RenderCopy(ren, tex, NULL, NULL); SDL_RenderPresent(ren); presents++;
        SDL_Delay(2);
    }
    if (gTcp) { SendMsg(4, 0); fprintf(stderr, "viewer: áudio recebido=%u consumido=%u quadros de áudio\n", gAudioIn, gAudioOut); gQuitNet = 1; }
    else gShared->quit = 1;
    if (adev) SDL_CloseAudioDevice(adev);
    SDL_DestroyTexture(tex); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
