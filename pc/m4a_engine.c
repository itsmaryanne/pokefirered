// Motor de som m4a (MusicPlayer2000) em C, portado função a função de src/m4a_1.s (assembly ARM).
// Usa as structs reais do jogo (processo de 32 bits => mesmo layout do GBA).
// Cobre: sequenciador (MPlayMain, TrackStop, ply_*), alocação de canais (ply_note), mixer PCM (SoundMain)
// e m4aSoundVSync (que no hardware reinicia o DMA do FIFO; aqui entrega um quadro de áudio à camada de saída).
// Vozes comprimidas (gritos) e reversas: ver MixSpecial.
#include "gba/m4a_internal.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern const u8 gClockTable[];
extern void FadeOutBody(struct MusicPlayerInfo *mplayInfo);
extern void TrkVolPitSet(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track);
extern u32 MidiKeyToFreq(struct WaveData *wav, u8 key, u8 fineAdjust);
extern void ClearChain(void *x);
extern void Clear64byte(void *x);
extern void RealClearChain(void *x);
void PcAudioEmit(const s8 *right, const s8 *left, int n, int pcmFreq);

// A asm usa os mesmos offsets para canais PCM e PSG nos campos comuns; conferimos em tempo de compilação.
_Static_assert(sizeof(struct SoundChannel) == 0x40 && sizeof(struct CgbChannel) == 0x40, "tamanho dos canais");
_Static_assert(offsetof(struct SoundChannel, track) == offsetof(struct CgbChannel, track), "offset track");
_Static_assert(offsetof(struct SoundChannel, nextChannelPointer) == offsetof(struct CgbChannel, nextChannelPointer), "offset next");
_Static_assert(offsetof(struct SoundChannel, frequency) == offsetof(struct CgbChannel, frequency), "offset frequency");
_Static_assert(sizeof(struct MusicPlayerTrack) == 0x50, "tamanho da trilha");
_Static_assert(sizeof(struct ToneData) == 12, "tamanho do ToneData");

static inline u8 *ReadCmd(struct MusicPlayerTrack *t) { return t->cmdPtr; }
static inline u8 NextByte(struct MusicPlayerTrack *t) { return *t->cmdPtr++; }

/* ======================= comandos do sequenciador (ply_*) ======================= */

void ply_fine(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    for (struct SoundChannel *chan = track->chan; chan != NULL; chan = chan->nextChannelPointer) {
        if (chan->statusFlags & SOUND_CHANNEL_SF_ON) chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
        RealClearChain(chan);
    }
    track->flags = 0;
}

void ply_goto(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    u8 *p = track->cmdPtr;
    u32 addr = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
    track->cmdPtr = (u8 *)(uintptr_t)addr;
}

void ply_patt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    if (track->patternLevel < 3) {
        track->patternStack[track->patternLevel] = track->cmdPtr + 4;
        track->patternLevel++;
        ply_goto(mplayInfo, track);
    } else {
        ply_fine(mplayInfo, track);
    }
}

void ply_pend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    if (track->patternLevel) {
        track->patternLevel--;
        track->cmdPtr = track->patternStack[track->patternLevel];
    }
}

void ply_rept(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    u8 *p = track->cmdPtr;
    if (*p == 0) {                          // repetições infinitas: pula o byte de contagem e vai
        track->cmdPtr = p + 1;
        ply_goto(mplayInfo, track);
        return;
    }
    track->repN++;
    u8 count = *track->cmdPtr++;
    if (track->repN < count) {
        ply_goto(mplayInfo, track);
    } else {
        track->repN = 0;
        track->cmdPtr = p + 5;              // contagem (1) + endereço (4)
    }
}

void ply_prio(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) { (void)mplayInfo; track->priority = NextByte(track); }

void ply_tempo(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    u32 v = NextByte(track) * 2;
    mplayInfo->tempoD = (u16)v;
    mplayInfo->tempoI = (u16)((v * mplayInfo->tempoU) >> 8);
}

void ply_keysh(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo; track->keyShift = (s8)NextByte(track); track->flags |= MPT_FLG_PITCHG;
}

void ply_voice(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    u8 idx = NextByte(track);
    struct ToneData *src = (struct ToneData *)((u8 *)mplayInfo->tone + idx * sizeof(struct ToneData));
    memcpy(&track->tone, src, sizeof(struct ToneData));
}

void ply_vol(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo; track->vol = NextByte(track); track->flags |= MPT_FLG_VOLCHG;
}

void ply_pan(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo; track->pan = (s8)(NextByte(track) - C_V); track->flags |= MPT_FLG_VOLCHG;
}

void ply_bend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo; track->bend = (s8)(NextByte(track) - C_V); track->flags |= MPT_FLG_PITCHG;
}

void ply_bendr(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo; track->bendRange = NextByte(track); track->flags |= MPT_FLG_PITCHG;
}

void ply_lfodl(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) { (void)mplayInfo; track->lfoDelay = NextByte(track); }

static void clear_modM(struct MusicPlayerTrack *track) {
    track->modM = 0;
    track->lfoSpeedC = 0;
    track->flags |= (track->modT == 0) ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG;
}

void ply_modt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    u8 v = NextByte(track);
    if (track->modT != v) { track->modT = v; track->flags |= MPT_FLG_VOLCHG | MPT_FLG_PITCHG; }
}

void ply_tune(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo; track->tune = (s8)(NextByte(track) - C_V); track->flags |= MPT_FLG_PITCHG;
}

void ply_port(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    u8 reg = NextByte(track), val = NextByte(track);
    *(volatile u8 *)(0x04000060 + reg) = val;       // REG_SOUND1CNT_L + reg
}

void ply_lfos(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    track->lfoSpeed = NextByte(track);
    if (track->lfoSpeed == 0) clear_modM(track);
}

void ply_mod(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    track->mod = NextByte(track);
    if (track->mod == 0) clear_modM(track);
}

void ply_endtie(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    u8 key = *track->cmdPtr;
    if (key < 0x80) { track->key = key; track->cmdPtr++; } else key = track->key;
    for (struct SoundChannel *chan = track->chan; chan != NULL; chan = chan->nextChannelPointer) {
        u8 st = chan->statusFlags;
        if ((st & (SOUND_CHANNEL_SF_START | SOUND_CHANNEL_SF_ENV)) && !(st & SOUND_CHANNEL_SF_STOP) && chan->midiKey == key) {
            chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
            break;
        }
    }
}

/* ============================= volume do canal ============================== */

static void ChnVolSetAsm(struct SoundChannel *chan, struct MusicPlayerTrack *track) {
    s32 velocity = chan->velocity;
    s32 rhythmPan = (s8)chan->rhythmPan;
    u32 r = ((u32)track->volMR * (u32)((0x80 + rhythmPan) * velocity)) >> 14;
    chan->rightVolume = r > 0xFF ? 0xFF : (u8)r;
    u32 l = ((u32)track->volML * (u32)((0x7F - rhythmPan) * velocity)) >> 14;
    chan->leftVolume = l > 0xFF ? 0xFF : (u8)l;
}

/* ================================== ply_note ================================= */

void ply_note(u32 note_cmd, struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;
    struct SoundChannel *chan;
    struct ToneData *tone;
    s32 rhythmPan = 0;
    u8 key;

    track->gateTime = gClockTable[note_cmd];
    u8 *cmd = track->cmdPtr;
    if (*cmd < 0x80) {
        track->key = *cmd++;
        if (*cmd < 0x80) {
            track->velocity = *cmd++;
            if (*cmd < 0x80) { track->gateTime += *cmd++; }
        }
        track->cmdPtr = cmd;
    }

    u8 type = track->tone.type;
    if (type & (TONEDATA_TYPE_RHY | TONEDATA_TYPE_SPL)) {
        u8 idx;
        if (type & TONEDATA_TYPE_SPL) {
            u8 *splitTable = *(u8 **)&track->tone.attack;       // a tabela de divisão ocupa o lugar de attack/decay/sustain/release
            idx = splitTable[track->key];
        } else {
            idx = track->key;
        }
        tone = (struct ToneData *)((u8 *)track->tone.wav + idx * sizeof(struct ToneData));   // wav aponta p/ um voicegroup
        if (tone->type & (TONEDATA_TYPE_SPL | TONEDATA_TYPE_RHY)) return;                    // divisão dentro de divisão não vale
        key = track->key;
        if (type & TONEDATA_TYPE_RHY) {
            if (tone->pan_sweep & 0x80) rhythmPan = ((s32)tone->pan_sweep - TONEDATA_P_S_PAN) * 2;
            key = tone->key;
        }
    } else {
        tone = &track->tone;
        key = track->key;
    }

    u32 priority = mplayInfo->priority + track->priority;
    if (priority > 0xFF) priority = 0xFF;

    u8 cgb = tone->type & TONEDATA_TYPE_CGB;
    if (cgb) {
        struct CgbChannel *cc = soundInfo->cgbChans;
        if (cc == NULL) return;
        cc += cgb - 1;
        u8 sf = cc->statusFlags;
        if ((sf & SOUND_CHANNEL_SF_ON) && !(sf & SOUND_CHANNEL_SF_STOP)) {
            if (cc->priority > priority) return;
            if (cc->priority == priority && (uintptr_t)cc->track < (uintptr_t)track) return;
        }
        chan = (struct SoundChannel *)cc;
    } else {
        u32 bestPrio = priority;
        struct MusicPlayerTrack *bestTrack = track;
        int foundStop = 0;
        struct SoundChannel *candidate = NULL;
        struct SoundChannel *c = soundInfo->chans;
        chan = NULL;
        for (s32 n = soundInfo->maxChans; n > 0; n--, c++) {
            u8 sf = c->statusFlags;
            if (!(sf & SOUND_CHANNEL_SF_ON)) { chan = c; break; }           // canal livre: usa já
            if (sf & SOUND_CHANNEL_SF_STOP) {
                if (!foundStop) { foundStop = 1; bestPrio = c->priority; bestTrack = c->track; candidate = c; continue; }
            } else if (foundStop) {
                continue;                                                    // já temos um canal em release; ignora os ativos
            }
            if (c->priority < bestPrio) { bestPrio = c->priority; bestTrack = c->track; candidate = c; }
            else if (c->priority > bestPrio) continue;
            else if ((uintptr_t)c->track > (uintptr_t)bestTrack) { bestTrack = c->track; candidate = c; }
            else if ((uintptr_t)c->track < (uintptr_t)bestTrack) continue;
            else candidate = c;
        }
        if (chan == NULL) {
            if (candidate == NULL) return;
            chan = candidate;
        }
    }

    ClearChain(chan);
    chan->prevChannelPointer = NULL;
    chan->nextChannelPointer = track->chan;
    if (track->chan != NULL) ((struct SoundChannel *)track->chan)->prevChannelPointer = chan;
    track->chan = chan;
    chan->track = track;

    track->lfoDelayC = track->lfoDelay;
    if (track->lfoDelay != 0) clear_modM(track);
    TrkVolPitSet(mplayInfo, track);

    chan->gateTime = track->gateTime;
    chan->midiKey = track->key;
    chan->velocity = track->velocity;
    chan->priority = (u8)priority;
    chan->key = key;
    chan->rhythmPan = (u8)rhythmPan;
    chan->type = tone->type;
    chan->wav = tone->wav;
    chan->attack = tone->attack;
    chan->decay = tone->decay;
    chan->sustain = tone->sustain;
    chan->release = tone->release;
    chan->pseudoEchoVolume = track->pseudoEchoVolume;
    chan->pseudoEchoLength = track->pseudoEchoLength;
    ChnVolSetAsm(chan, track);

    s32 k = (s32)chan->key + (s8)track->keyM;
    if (k < 0) k = 0;
    u32 freq;
    if (cgb) {
        struct CgbChannel *cc = (struct CgbChannel *)chan;
        cc->length = tone->length;
        u8 sw = tone->pan_sweep;
        if ((sw & 0x80) || !(sw & 0x70)) sw = 8;                // sem sweep válido: valor "desligado"
        cc->sweep = sw;
        freq = soundInfo->MidiKeyToCgbFreq(cgb, (u8)k, track->pitM);
    } else {
        chan->count = track->unk_3C;
        freq = MidiKeyToFreq(chan->wav, (u8)k, track->pitM);
    }
    chan->frequency = freq;
    chan->statusFlags = SOUND_CHANNEL_SF_START;
    track->flags &= 0xF0;
}

/* ============================== trilhas / player ============================ */

void TrackStop(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    if (!(track->flags & MPT_FLG_EXIST)) return;
    struct SoundChannel *chan = track->chan;
    while (chan != NULL) {
        if (chan->statusFlags != 0) {
            u8 cgb = chan->type & TONEDATA_TYPE_CGB;
            if (cgb) SOUND_INFO_PTR->CgbOscOff(cgb);
            chan->statusFlags = 0;
        }
        chan->track = NULL;
        chan = chan->nextChannelPointer;
    }
    track->chan = NULL;
}

void MPlayMain(struct MusicPlayerInfo *mplayInfo) {
    struct SoundInfo *soundInfo;
    if (mplayInfo->ident != ID_NUMBER) return;
    mplayInfo->ident++;

    if (mplayInfo->MPlayMainNext != NULL) mplayInfo->MPlayMainNext(mplayInfo->musicPlayerNext);

    if ((s32)mplayInfo->status < 0) goto done;                          // pausado
    soundInfo = SOUND_INFO_PTR;
    FadeOutBody(mplayInfo);
    if ((s32)mplayInfo->status < 0) goto done;

    mplayInfo->tempoC += mplayInfo->tempoI;
    while (mplayInfo->tempoC >= 150) {
        u32 existing = 0, bit = 1;
        struct MusicPlayerTrack *track = mplayInfo->tracks;
        for (s32 i = mplayInfo->trackCount; i > 0; i--, track++, bit <<= 1) {
            if (!(track->flags & MPT_FLG_EXIST)) continue;
            existing |= bit;

            // gate time dos canais desta trilha
            for (struct SoundChannel *chan = track->chan; chan != NULL; chan = chan->nextChannelPointer) {
                u8 sf = chan->statusFlags;
                if (sf & SOUND_CHANNEL_SF_ON) {
                    if (chan->gateTime != 0) {
                        chan->gateTime--;
                        if (chan->gateTime == 0) chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
                    }
                } else {
                    ClearChain(chan);
                }
            }

            if (track->flags & MPT_FLG_START) {
                Clear64byte(track);
                track->flags = MPT_FLG_EXIST;
                track->bendRange = 2;
                track->volX = 0x40;
                track->lfoSpeed = 0x16;
                track->tone.type = 1;
            }

            // lê comandos até a trilha ter que esperar
            int ended = 0;
            while (track->wait == 0) {
                u8 *p = track->cmdPtr;
                u32 cmd = *p;
                if (cmd < 0x80) {
                    cmd = track->runningStatus;                         // status "corrente": o byte lido é dado, não comando
                } else {
                    track->cmdPtr = p + 1;
                    if (cmd >= 0xBD) track->runningStatus = (u8)cmd;
                }
                if (cmd >= 0xCF) {
                    soundInfo->plynote(cmd - 0xCF, mplayInfo, track);
                } else if (cmd > 0xB0) {
                    mplayInfo->cmd = (u8)(cmd - 0xB1);
                    soundInfo->MPlayJumpTable[mplayInfo->cmd](mplayInfo, track);
                    if (track->flags == 0) { ended = 1; break; }        // ply_fine encerrou a trilha
                } else {
                    track->wait = gClockTable[cmd - 0x80];
                }
            }
            if (ended) continue;

            track->wait--;
            // LFO
            if (track->lfoSpeed != 0 && track->mod != 0) {
                if (track->lfoDelayC != 0) {
                    track->lfoDelayC--;
                } else {
                    track->lfoSpeedC += track->lfoSpeed;
                    u8 t = track->lfoSpeedC;
                    s32 tri = (((u8)(t - 0x40)) & 0x80) == 0 ? (0x80 - t) : (s8)t;     // onda triangular
                    tri = (s32)(((s32)track->mod * tri) >> 6);
                    if (((u8)track->modM ^ (u8)tri) != 0) {
                        track->modM = (s8)tri;
                        track->flags |= (track->modT == 0) ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG;
                    }
                }
            }
        }
        mplayInfo->clock++;
        if (existing == 0) { mplayInfo->status = MUSICPLAYER_STATUS_PAUSE; goto done; }
        mplayInfo->status = existing;
        mplayInfo->tempoC -= 150;
    }

    // aplica volume/altura alterados às trilhas e seus canais
    {
        struct MusicPlayerTrack *track = mplayInfo->tracks;
        for (s32 i = mplayInfo->trackCount; i > 0; i--, track++) {
            if (!(track->flags & MPT_FLG_EXIST)) continue;
            if (!(track->flags & (MPT_FLG_VOLCHG | MPT_FLG_PITCHG))) continue;
            TrkVolPitSet(mplayInfo, track);
            for (struct SoundChannel *chan = track->chan; chan != NULL; chan = chan->nextChannelPointer) {
                if (!(chan->statusFlags & SOUND_CHANNEL_SF_ON)) { ClearChain(chan); continue; }
                u8 cgb = chan->type & TONEDATA_TYPE_CGB;
                if (track->flags & MPT_FLG_VOLCHG) {
                    ChnVolSetAsm(chan, track);
                    if (cgb) ((struct CgbChannel *)chan)->modify |= CGB_CHANNEL_MO_VOL;
                }
                if (track->flags & MPT_FLG_PITCHG) {
                    s32 k = (s32)chan->key + (s8)track->keyM;
                    if (k < 0) k = 0;
                    if (cgb) {
                        ((struct CgbChannel *)chan)->frequency = soundInfo->MidiKeyToCgbFreq(cgb, (u8)k, track->pitM);
                        ((struct CgbChannel *)chan)->modify |= CGB_CHANNEL_MO_PIT;
                    } else {
                        chan->frequency = MidiKeyToFreq(chan->wav, (u8)k, track->pitM);
                    }
                }
            }
            track->flags &= 0xF0;
        }
    }
done:
    mplayInfo->ident = ID_NUMBER;
}

/* ================================== mixer PCM ================================= */

static inline s8 Wrap8(s32 v) { return (s8)(u8)v; }

/* ---- vozes "comprimidas" (gritos de Pokémon) e "reversas": portadas de SoundMainRAM_Unk1/Unk2 ---- */
#define SF_SPECIAL 0x20
#define TYPE_REV   0x10
extern const s8 gDeltaEncodingTable[];
static s8 sDecBuf[64];

// Amostra de índice idx (relativo ao início dos dados). Amostras comprimidas vêm em blocos de 33 bytes -> 64 amostras:
// o 1o byte é a amostra inicial; do 2o só vale o nibble baixo; dos demais, nibble alto e depois baixo; cada nibble
// indexa a tabela de deltas e o resultado é acumulado.
static s32 FetchSample(struct WaveData *wav, s32 idx, s32 *cachedBlock) {
    if (wav->type == 0) return wav->data[idx];
    s32 b = idx >> 6;
    if (b != *cachedBlock) {
        *cachedBlock = b;
        const u8 *p = (const u8 *)wav->data + b * 33;
        u8 acc = p[0];
        sDecBuf[0] = (s8)acc;
        acc = (u8)(acc + gDeltaEncodingTable[p[1] & 0xF]);
        sDecBuf[1] = (s8)acc;
        const u8 *q = p + 2;
        for (int i = 2; i < 64; q++) {
            acc = (u8)(acc + gDeltaEncodingTable[*q >> 4]);  sDecBuf[i++] = (s8)acc;
            acc = (u8)(acc + gDeltaEncodingTable[*q & 0xF]); sDecBuf[i++] = (s8)acc;
        }
    }
    return sDecBuf[idx & 63];
}

// Mistura uma voz comprimida e/ou reversa. A posição fica em currentPointer como ÍNDICE (não ponteiro).
static void MixSpecial(struct SoundInfo *si, struct SoundChannel *chan, struct WaveData *wav, s8 *bufR, s32 spv,
                       u32 vr, u32 vl, s32 loopLen, s32 count) {
    s8 *bufL = bufR + PCM_DMA_BUF_SIZE;
    int rev = (chan->type & TYPE_REV) != 0;
    if (!(chan->statusFlags & SF_SPECIAL)) {                    // primeira vez: converte o ponteiro inicial em índice
        chan->statusFlags |= SF_SPECIAL;
        s32 idx = (s32)(chan->currentPointer - wav->data);
        if (rev) idx = (s32)wav->size - idx;
        chan->currentPointer = (s8 *)(intptr_t)idx;
    }
    s32 idx = (s32)(intptr_t)chan->currentPointer;
    u32 step = (chan->type & 0x08) ? 0x800000u : (u32)si->divFreq * chan->frequency;
    u32 fw = chan->fw;
    s32 blk = -1;
    if (!rev && wav->type == 0) return;                        // como na asm: nada a fazer
    s32 c = rev ? idx - 1 : idx;                               // índice da amostra atual
    s32 dir = rev ? -1 : 1;
    s32 cur = FetchSample(wav, c, &blk), nxt = FetchSample(wav, c + dir, &blk), diff = nxt - cur;
    for (s32 n = 0; n < spv; n++) {
        s32 v = cur + (s32)(((s32)fw * diff) >> 23);
        bufR[n] = Wrap8(bufR[n] + ((s32)(vr * v) >> 8));
        bufL[n] = Wrap8(bufL[n] + ((s32)(vl * v) >> 8));
        fw += step;
        u32 adv = fw >> 23;
        if (adv == 0) continue;
        fw &= ~0x3F800000u;
        count -= (s32)adv;
        if (count <= 0) {
            if (rev || loopLen == 0) { chan->statusFlags = 0; return; }
            s32 over = -count, c2 = loopLen + count;
            while (c2 <= 0) { over -= loopLen; c2 += loopLen; }
            count = c2;
            c = (s32)wav->loopStart + over;
        } else {
            c += dir * (s32)adv;
        }
        cur = FetchSample(wav, c, &blk); nxt = FetchSample(wav, c + dir, &blk); diff = nxt - cur;
    }
    chan->fw = fw; chan->count = (u32)count;
    chan->currentPointer = (s8 *)(intptr_t)(rev ? c + 1 : c);
}

// Mistura um canal PCM nos buffers R/L (um "quadro" de spv amostras), na ordem exata da asm.
static void MixChannel(struct SoundInfo *si, struct SoundChannel *chan, s8 *bufR, s32 spv) {
    u8 st = chan->statusFlags;
    if (!(st & SOUND_CHANNEL_SF_ON)) return;
    struct WaveData *wav = chan->wav;
    u32 env;
    s8 *bufL = bufR + PCM_DMA_BUF_SIZE;

    if (st & SOUND_CHANNEL_SF_START) {
        if (st & SOUND_CHANNEL_SF_STOP) { chan->statusFlags = 0; return; }
        st = SOUND_CHANNEL_SF_ENV_ATTACK;
        chan->statusFlags = st;
        s32 cnt = (s32)chan->count;
        chan->currentPointer = wav->data + cnt;
        chan->count = wav->size - cnt;
        env = 0; chan->envelopeVolume = 0; chan->fw = 0;
        if ((wav->status >> 8) & 0xC0) { st |= SOUND_CHANNEL_SF_LOOP; chan->statusFlags = st; }
        goto attack;
    }
    env = chan->envelopeVolume;
    if (st & SOUND_CHANNEL_SF_IEC) {
        u8 len = chan->pseudoEchoLength;
        chan->pseudoEchoLength = len - 1;
        if (len > 1) goto have_env;
        chan->statusFlags = 0; return;
    } else if (st & SOUND_CHANNEL_SF_STOP) {
        env = (env * chan->release) >> 8;
        if (env > chan->pseudoEchoVolume) goto have_env;
        goto pseudo_echo;
    } else {
        u8 e = st & SOUND_CHANNEL_SF_ENV;
        if (e == SOUND_CHANNEL_SF_ENV_DECAY) {
            env = (env * chan->decay) >> 8;
            if (env > chan->sustain) goto have_env;
            env = chan->sustain;
            if (env == 0) goto pseudo_echo;
            st -= 1; chan->statusFlags = st;
            goto have_env;
        } else if (e == SOUND_CHANNEL_SF_ENV_ATTACK) {
            goto attack;
        } else {
            goto have_env;
        }
    }
pseudo_echo:
    env = chan->pseudoEchoVolume;
    if (env == 0) { chan->statusFlags = 0; return; }
    st |= SOUND_CHANNEL_SF_IEC; chan->statusFlags = st;
    goto have_env;
attack:
    env += chan->attack;
    if (env >= 0xFF) { env = 0xFF; st -= 1; chan->statusFlags = st; }
have_env:
    chan->envelopeVolume = (u8)env;
    u32 e2 = (((u32)si->masterVolume + 1) * env) >> 4;
    u32 vr = (chan->rightVolume * e2) >> 8, vl = (chan->leftVolume * e2) >> 8;
    chan->envelopeVolumeRight = (u8)vr; chan->envelopeVolumeLeft = (u8)vl;

    s32 loopLen = 0; s8 *loopStart = NULL;
    if (st & SOUND_CHANNEL_SF_LOOP) { loopStart = wav->data + wav->loopStart; loopLen = (s32)(wav->size - wav->loopStart); }

    s32 count = (s32)chan->count;
    if (chan->type & (0x10 | 0x20)) { MixSpecial(si, chan, wav, bufR, spv, vr, vl, loopLen, count); return; }
    s8 *p = chan->currentPointer;

    if (chan->type & 0x08) {                                    // voz de taxa fixa (sem reamostragem)
        for (s32 n = 0; n < spv; n++) {
            s32 s = *p++;
            bufR[n] = Wrap8(bufR[n] + ((s32)(vr * s) >> 8));
            bufL[n] = Wrap8(bufL[n] + ((s32)(vl * s) >> 8));
            if (--count == 0) {
                if (loopLen != 0) { p = loopStart; count = loopLen; }
                else { chan->statusFlags = 0; return; }
            }
        }
        chan->count = (u32)count; chan->currentPointer = p;
        return;
    }

    // reamostragem com interpolação linear; fw tem 23 bits de fração
    u32 fw = chan->fw;
    u32 step = (u32)si->divFreq * chan->frequency;
    s32 cur = p[0], diff = p[1] - cur;
    for (s32 n = 0; n < spv; n++) {
        s32 v = cur + (s32)(((s32)fw * diff) >> 23);
        bufR[n] = Wrap8(bufR[n] + ((s32)(vr * v) >> 8));
        bufL[n] = Wrap8(bufL[n] + ((s32)(vl * v) >> 8));
        fw += step;
        u32 adv = fw >> 23;
        if (adv == 0) continue;
        fw &= ~0x3F800000u;
        count -= (s32)adv;
        if (count <= 0) {
            if (loopLen == 0) { chan->statusFlags = 0; return; }
            s32 over = -count;
            s32 c = loopLen + count;
            while (c <= 0) { over -= loopLen; c += loopLen; }
            count = c;
            p = loopStart + over;
        } else {
            p += adv;
        }
        cur = p[0]; diff = p[1] - cur;
    }
    chan->fw = fw; chan->count = (u32)count; chan->currentPointer = p;
}

void SoundMain(void) {
    struct SoundInfo *si = SOUND_INFO_PTR;
    if (si->ident != ID_NUMBER) return;
    si->ident++;

    if (si->MPlayMainHead != NULL) si->MPlayMainHead(si->musicPlayerHead);
    si->CgbSound();

    s32 spv = si->pcmSamplesPerVBlank;
    u8 counter = si->pcmDmaCounter;
    s32 chunk = 0;
    if (counter >= 2) chunk = (s32)si->pcmDmaPeriod - ((s32)counter - 1);   // contador<=1: o próximo V-blank recarrega e volta ao começo
    s8 *bufR = si->pcmBuffer + chunk * spv;
    s8 *bufL = bufR + PCM_DMA_BUF_SIZE;

    if (si->reverb) {                                           // eco simples: realimenta a média do quadro atual e do próximo
        s8 *nextR = (counter == 2) ? si->pcmBuffer : bufR + spv;
        s8 *nextL = nextR + PCM_DMA_BUF_SIZE;
        for (s32 i = 0; i < spv; i++) {
            s32 sum = bufL[i] + bufR[i] + nextL[i] + nextR[i];
            s32 v = (sum * (s32)si->reverb) >> 9;
            if (v & 0x80) v += 1;
            bufL[i] = (s8)v; bufR[i] = (s8)v;
        }
    } else {
        memset(bufR, 0, spv);
        memset(bufL, 0, spv);
    }

    struct SoundChannel *chan = si->chans;
    for (s32 n = si->maxChans; n > 0; n--, chan++) MixChannel(si, chan, bufR, spv);

    si->ident = ID_NUMBER;
}

/* ========================= V-blank do som (substitui o DMA) ===================== */

// No hardware, m4aSoundVSync reinicia o DMA do FIFO no começo de cada "período" de buffer; entre reinícios o DMA
// segue lendo o buffer sequencialmente, um quadro (spv amostras) por V-blank. Aqui entregamos esse quadro à saída.
void m4aSoundVSync(void) {
    struct SoundInfo *si = SOUND_INFO_PTR;
    if (si == NULL) return;
    if (si->ident != ID_NUMBER && si->ident != ID_NUMBER + 1) return;
    s32 c = (s32)si->pcmDmaCounter - 1;
    si->pcmDmaCounter = (u8)c;
    if (c <= 0) si->pcmDmaCounter = si->pcmDmaPeriod;          // recarrega e (no hardware) reinicia o DMA no começo do buffer
    s32 chunk = (s32)si->pcmDmaPeriod - (s32)si->pcmDmaCounter;
    s32 spv = si->pcmSamplesPerVBlank;
    if (spv <= 0 || chunk < 0) return;
    const s8 *r = si->pcmBuffer + chunk * spv;
    PcAudioEmit(r, r + PCM_DMA_BUF_SIZE, spv, si->pcmFreq);
}
