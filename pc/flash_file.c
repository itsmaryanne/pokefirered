// Memória flash do cartucho (save de 128 KB) simulada com um arquivo mapeado em memória.
// Substitui src/agb_flash*.c. Mantém a semântica do chip: apagar = 0xFF, gravar só limpa bits (AND).
// O arquivo é "pokefirered.sav" no diretório atual (ou o caminho em $PC_SAVE).
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define FLASH_SIZE   131072
#define SECTOR_SIZE  4096
#define NUM_SECTORS  32

static uint8_t *sFlash;

static void FlashOpen(void) {
    if (sFlash) return;
    const char *path = getenv("PC_SAVE"); if (!path) path = "pokefirered.sav";
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("[pc] nao consegui abrir o arquivo de save"); exit(1); }
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < FLASH_SIZE) {                              // arquivo novo (ou curto): completa com 0xFF, como um chip apagado
        static uint8_t ff[SECTOR_SIZE]; memset(ff, 0xFF, sizeof ff);
        lseek(fd, size, SEEK_SET);
        for (off_t n = size; n < FLASH_SIZE; n += SECTOR_SIZE) if (write(fd, ff, SECTOR_SIZE) != SECTOR_SIZE) { perror("[pc] save"); exit(1); }
    }
    sFlash = mmap(NULL, FLASH_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (sFlash == MAP_FAILED) { perror("[pc] mmap do save"); exit(1); }
    close(fd);
}

static uint8_t *Sector(uint16_t n) { return sFlash + (size_t)(n % NUM_SECTORS) * SECTOR_SIZE; }

uint16_t IdentifyFlash(void) { FlashOpen(); return 0; }                 // 0 = chip reconhecido

void ReadFlash(uint16_t sectorNum, uint32_t offset, void *dest, uint32_t size) {
    FlashOpen();
    if (offset + size > SECTOR_SIZE) size = offset < SECTOR_SIZE ? SECTOR_SIZE - offset : 0;
    memcpy(dest, Sector(sectorNum) + offset, size);
}

static uint16_t EraseFlashSectorImpl(uint16_t);
uint16_t (*EraseFlashSector)(uint16_t) = EraseFlashSectorImpl;
static uint16_t EraseFlashSectorImpl(uint16_t sectorNum) { FlashOpen(); memset(Sector(sectorNum), 0xFF, SECTOR_SIZE); return 0; }

static uint16_t ProgramFlashByteImpl(uint16_t, uint32_t, uint8_t);
uint16_t (*ProgramFlashByte)(uint16_t, uint32_t, uint8_t) = ProgramFlashByteImpl;
static uint16_t ProgramFlashByteImpl(uint16_t sectorNum, uint32_t offset, uint8_t data) {
    FlashOpen();
    if (offset >= SECTOR_SIZE) return 1;
    Sector(sectorNum)[offset] &= data;                                    // flash real só consegue levar bits de 1 para 0
    return 0;
}

uint32_t ProgramFlashSectorAndVerify(uint16_t sectorNum, uint8_t *src) {   // retorna 0 se gravou e conferiu
    FlashOpen();
    EraseFlashSector(sectorNum);
    memcpy(Sector(sectorNum), src, SECTOR_SIZE);
    return memcmp(Sector(sectorNum), src, SECTOR_SIZE) != 0;
}

uint16_t SetFlashTimerIntr(uint8_t timerNum, void (**intrFunc)(void)) { (void)intrFunc; return timerNum >= 4; }
