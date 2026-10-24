// Harness de referência: roda a ROM ORIGINAL no mGBA e grava áudio (s16 estéreo) e quadros (PPM) para comparação.
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/gba/core.h>
#include <mgba/gba/interface.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/vfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "uso: ref rom frames prefixo dump_frames [script frame:teclas,...]\n"); return 1; }
    const char *rom = argv[1]; int nframes = atoi(argv[2]); const char *prefix = argv[3];
    int dumps[64], nd = 0; for (char *t = strtok(strdup(argv[4]), ","); t && nd < 64; t = strtok(NULL, ",")) dumps[nd++] = atoi(t);
    int sf[256], sk[256], ns = 0;
    if (argc > 5) for (char *t = strtok(strdup(argv[5]), ","); t && ns < 256; t = strtok(NULL, ",")) { sf[ns] = atoi(t); char *c = strchr(t, ':'); sk[ns] = c ? strtol(c + 1, NULL, 0) : 0; ns++; }
    struct mCore *core = mCoreFind(rom);
    if (!core) { fprintf(stderr, "nao reconheci a ROM\n"); return 1; }
    core->init(core);
    mCoreInitConfig(core, NULL);
    unsigned w, h; core->baseVideoSize(core, &w, &h);
    mColor *vbuf = malloc(w * h * sizeof(mColor));
    core->setVideoBuffer(core, vbuf, w);
    core->setAudioBufferSize(core, 4096);
    if (!mCoreLoadFile(core, rom)) { fprintf(stderr, "falha ao carregar a ROM\n"); return 1; }
    core->reset(core);
    { const char *m = getenv("REF_MASK"); if (m) { unsigned mask = strtoul(m, NULL, 0); for (int c = 0; c < 6; c++) core->enableAudioChannel(core, c, (mask >> c) & 1); } }
    unsigned rate = core->audioSampleRate(core);
    fprintf(stderr, "taxa de áudio do núcleo: %u Hz, vídeo %ux%u\n", rate, w, h);
    char name[256]; snprintf(name, sizeof name, "%s.pcm", prefix);
    FILE *af = fopen(name, "wb");
    snprintf(name, sizeof name, "%s.cnt", prefix); FILE *cf = fopen(name, "wb");
    struct mAudioBuffer *ab = core->getAudioBuffer(core);
    FILE *hf = getenv("REF_HASH") ? fopen(getenv("REF_HASH"), "w") : NULL;
    uint32_t keys = 0;
    for (int f = 1; f <= nframes; f++) {
        for (int i = 0; i < ns; i++) if (sf[i] == f) keys = (uint32_t)sk[i];
        core->setKeys(core, keys);
        core->runFrame(core);
        int16_t buf[4096 * 2];
        size_t avail = mAudioBufferAvailable(ab);
        if (avail > 4096) avail = 4096;
        size_t got = mAudioBufferRead(ab, buf, avail);
        fwrite(buf, 4, got, af); { int g = (int)got; fwrite(&g, 4, 1, cf); }
        { const char *io = getenv("REF_IO"); if (io && f == atoi(io)) { fprintf(stderr, "REF IO @%d: DISPCNT=%04x BG2CNT=%04x PA=%04x PB=%04x PC=%04x PD=%04x X=%08x Y=%08x\n", f, core->busRead16(core, 0x04000000), core->busRead16(core, 0x0400000C), core->busRead16(core, 0x04000020), core->busRead16(core, 0x04000022), core->busRead16(core, 0x04000024), core->busRead16(core, 0x04000026), core->busRead32(core, 0x04000028), core->busRead32(core, 0x0400002C)); } }
        if (hf) { uint32_t hh = 2166136261u; for (unsigned p = 0; p < w * h; p++) { uint32_t c = vbuf[p]; hh = (hh ^ (c & 255)) * 16777619u; hh = (hh ^ ((c >> 8) & 255)) * 16777619u; hh = (hh ^ ((c >> 16) & 255)) * 16777619u; } fprintf(hf, "%d %08x\n", f, hh); }
        for (int i = 0; i < nd; i++) if (dumps[i] == f) {
            snprintf(name, sizeof name, "%s_f%d.ppm", prefix, f);
            FILE *pf = fopen(name, "wb"); fprintf(pf, "P6\n%u %u\n255\n", w, h);
            for (unsigned p = 0; p < w * h; p++) { uint32_t c = vbuf[p]; fputc(c & 255, pf); fputc((c >> 8) & 255, pf); fputc((c >> 16) & 255, pf); }
            fclose(pf);
        }
    }
    fclose(af); fclose(cf);
    core->deinit(core);
    return 0;
}
