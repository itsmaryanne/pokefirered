// Testes da BIOS em C: roda sem o jogo, comparando com dados reais gerados pelo build do projeto.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int16_t s16; typedef int32_t s32;
void LZ77UnCompWram(const void *, void *); void RLUnCompWram(const void *, void *);
u16 ArcTan2(s16, s16); u16 Sqrt(u32); s32 Div(s32, s32);
void CpuSet(const void *, void *, u32); void CpuFastSet(const void *, void *, u32);
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALHOU: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static u8 *rd(const char *p, long *n) { FILE *f = fopen(p, "rb"); if (!f) return 0; fseek(f, 0, SEEK_END); *n = ftell(f); rewind(f); u8 *b = malloc(*n + 16); fread(b, 1, *n, f); fclose(f); return b; }
int main(void) {
    CHECK(ArcTan2(1, 1) == 0x2000, "ArcTan2(1,1)=%x", ArcTan2(1, 1));
    CHECK(ArcTan2(0, 1) == 0x4000, "ArcTan2(0,1)");  CHECK(ArcTan2(-1, 0) == 0x8000, "ArcTan2(-1,0)");
    CHECK(ArcTan2(0, -1) == 0xC000, "ArcTan2(0,-1)"); CHECK(ArcTan2(1, -1) == 0xE000, "ArcTan2(1,-1)=%x", ArcTan2(1, -1));
    CHECK(ArcTan2(-1, -1) == 0xA000, "ArcTan2(-1,-1)=%x", ArcTan2(-1, -1)); CHECK(ArcTan2(-1, 1) == 0x6000, "ArcTan2(-1,1)=%x", ArcTan2(-1, 1));
    CHECK(Sqrt(0) == 0 && Sqrt(15) == 3 && Sqrt(16) == 4 && Sqrt(0xFFFFFFFFu) == 65535, "Sqrt");
    CHECK(Div(-7, 2) == -3 && Div(100, 7) == 14, "Div");
    u16 a[4] = {1, 2, 3, 4}, b[4] = {0}; CpuSet(a, b, 4); CHECK(!memcmp(a, b, 8), "CpuSet 16");
    u32 w[16], z[16]; for (int i = 0; i < 16; i++) w[i] = i; CpuFastSet(w, z, 16); CHECK(!memcmp(w, z, 64), "CpuFastSet");
    // LZ77: compara cada graphics/**/X.ext.lz com o X.ext irmão (gerado pelo build)
    int lz = 0, lzbad = 0, rl = 0, rlbad = 0;
    char cmd[] = "find graphics -name '*.lz' 2>/dev/null"; FILE *p = popen(cmd, "r"); char path[1024];
    while (p && fgets(path, sizeof path, p)) {
        path[strcspn(path, "\n")] = 0; char raw[1024]; strcpy(raw, path); raw[strlen(raw) - 3] = 0;
        long n1, n2; u8 *c = rd(path, &n1), *r = rd(raw, &n2); if (!c || !r) { free(c); free(r); continue; }
        u8 *o = calloc(1, n2 + 64); LZ77UnCompWram(c, o); lz++; if (n2 < 0 || memcmp(o, r, n2)) lzbad++;
        free(o); free(c); free(r);
    }
    if (p) pclose(p);
    p = popen("find graphics -name '*.rl' 2>/dev/null", "r");
    while (p && fgets(path, sizeof path, p)) {
        path[strcspn(path, "\n")] = 0; char raw[1024]; strcpy(raw, path); raw[strlen(raw) - 3] = 0;
        long n1, n2; u8 *c = rd(path, &n1), *r = rd(raw, &n2); if (!c || !r) { free(c); free(r); continue; }
        u8 *o = calloc(1, n2 + 64); RLUnCompWram(c, o); rl++; if (memcmp(o, r, n2)) rlbad++;
        free(o); free(c); free(r);
    }
    if (p) pclose(p);
    printf("LZ77: %d arquivos comparados, %d diferentes\nRL:   %d arquivos comparados, %d diferentes\n", lz, lzbad, rl, rlbad);
    CHECK(lzbad == 0 && rlbad == 0, "descompressao difere do original");
    printf(fails ? "%d testes falharam\n" : "todos os testes passaram\n", fails);
    return fails != 0;
}
