// FMOD Ex replacement (the subset games used so far need), on CoreAudio.
//
// Sounds are read completely at creation time through the engine's own file callbacks
// (System::setFileSystem, or per-sound callbacks in FMOD_CREATESOUNDEXINFO) and decoded to
// float PCM: Ogg Vorbis via stb_vorbis, or raw PCM16 (voice: the engine decodes Speex in its
// read callback). Mixing happens on the CoreAudio thread using only host data; channel-end
// callbacks into guest code are delivered from System::update on the engine's thread.
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb_vorbis.c"   // declarations only; implementation in vorbis.c
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <AudioToolbox/AudioToolbox.h>

#define RET(v) (c->eax = (uint32_t)(v))
#define FMOD_OK 0
#define FMOD_ERR_FILE_NOTFOUND 23
#define FMOD_ERR_FORMAT 25

// FMOD Ex mode flags / enums used by the game
#define M_LOOP_NORMAL 0x2
#define M_OPENRAW     0x1000
#define TU_MS 1
#define TU_PCM 2
#define TU_PCMBYTES 4

static int trace = -1;
#define T(...) do { if (trace < 0) trace = getenv("RT32_TRACE_FMOD") != NULL; if (trace) fprintf(stderr, "[fmod] " __VA_ARGS__); } while (0)

// ---------- objects ----------
enum { K_SYSTEM = 1, K_SOUND, K_CHANNEL, K_GROUP };
#define MAXSND 4096
#define MAXCH 64
#define MAXGRP 64
typedef struct DecodeJob DecodeJob;
typedef struct { int used; uint32_t g; float *pcm; uint32_t frames; int nch; float rate; int loopcount; int ready; DecodeJob *job; } Snd;
typedef struct {
    int used, sound, group, paused, playing, ended;
    uint32_t g, cb, userdata;
    double pos;            // in source frames
    float vol, freq;
    int loops_left;
} Chan;
typedef struct { int used; uint32_t g; float vol; int mute, paused, parent; } Grp;

static Snd snds[MAXSND];
static Chan chans[MAXCH];
static Grp grps[MAXGRP];
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_system, fs_open, fs_close, fs_read, fs_seek;
static int master = -1;
static float out_rate = 48000;

static uint32_t new_handle(int kind, int idx) {
    uint32_t g = gmalloc(16);
    wr32(g, 0x464D4F44); wr32(g + 4, (uint32_t)kind); wr32(g + 8, (uint32_t)idx);
    return g;
}
static int idx_of(uint32_t g, int kind) {
    if (!g || rd32(g) != 0x464D4F44 || (int)rd32(g + 4) != kind) return -1;
    return (int)rd32(g + 8);
}
#define SND(g) idx_of((g), K_SOUND)
#define CH(g) idx_of((g), K_CHANNEL)
#define GRP(g) idx_of((g), K_GROUP)

static int new_group(void) {
    for (int i = 0; i < MAXGRP; i++) if (!grps[i].used) {
        grps[i] = (Grp){ 1, new_handle(K_GROUP, i), 1.0f, 0, 0, master };
        return i;
    }
    return -1;
}

// ---------- mixer (CoreAudio thread; host data only) ----------
static void group_state(int g, float *vol, int *mute, int *paused) {
    *vol = 1; *mute = 0; *paused = 0;
    for (int k = 0; g >= 0 && k < 16; k++, g = grps[g].parent) {
        *vol *= grps[g].vol; *mute |= grps[g].mute; *paused |= grps[g].paused;
        if (g == master) break;
    }
}
static OSStatus render(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *io) {
    (void)ref; (void)fl; (void)ts; (void)bus;
    float *L = io->mBuffers[0].mData, *R = io->mNumberBuffers > 1 ? io->mBuffers[1].mData : NULL;
    memset(L, 0, n * sizeof(float)); if (R) memset(R, 0, n * sizeof(float));
    if (pthread_mutex_trylock(&mx)) return noErr;   // never block the audio thread for long
    for (int i = 0; i < MAXCH; i++) {
        Chan *ch = &chans[i];
        if (!ch->used || !ch->playing || ch->paused || ch->sound < 0) continue;
        Snd *s = &snds[ch->sound];
        if (!s->pcm || !s->frames || !s->ready) continue;   // still decoding: stays silent until ready
        float gv; int gm, gp; group_state(ch->group, &gv, &gm, &gp);
        if (gp) continue;
        float v = gm ? 0 : ch->vol * gv;
        double step = ch->freq / out_rate;
        for (UInt32 k = 0; k < n; k++) {
            if (ch->pos >= s->frames) {
                if (ch->loops_left != 0) { ch->pos -= s->frames; if (ch->loops_left > 0) ch->loops_left--; }
                else { ch->playing = 0; ch->ended = 1; break; }
            }
            uint32_t p0 = (uint32_t)ch->pos, p1 = p0 + 1 < s->frames ? p0 + 1 : (ch->loops_left ? 0 : p0);
            float t = (float)(ch->pos - p0);
            float a0 = s->pcm[p0 * s->nch], a1 = s->pcm[p1 * s->nch];
            float b0 = s->nch > 1 ? s->pcm[p0 * s->nch + 1] : a0, b1 = s->nch > 1 ? s->pcm[p1 * s->nch + 1] : a1;
            L[k] += (a0 + (a1 - a0) * t) * v;
            if (R) R[k] += (b0 + (b1 - b0) * t) * v;
            ch->pos += step;
        }
    }
    pthread_mutex_unlock(&mx);
    if (trace > 0) {   // level meter: peak of each ~1 s of output
        static float peak; static UInt32 acc;
        for (UInt32 k = 0; k < n; k++) { float a = fabsf(L[k]); if (a > peak) peak = a; }
        if ((acc += n) >= (UInt32)out_rate) { fprintf(stderr, "[fmod] output peak %.3f\n", peak); peak = 0; acc = 0; }
    }
    for (UInt32 k = 0; k < n; k++) { if (L[k] > 1) L[k] = 1; if (L[k] < -1) L[k] = -1; if (R) { if (R[k] > 1) R[k] = 1; if (R[k] < -1) R[k] = -1; } }
    return noErr;
}
static AudioUnit unit;
static void audio_start(void) {
    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp || AudioComponentInstanceNew(comp, &unit)) { fprintf(stderr, "[fmod] no audio output\n"); return; }
    AudioStreamBasicDescription f = { 0 };
    f.mSampleRate = out_rate; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    f.mChannelsPerFrame = 2; f.mFramesPerPacket = 1; f.mBitsPerChannel = 32; f.mBytesPerFrame = 4; f.mBytesPerPacket = 4;
    AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AURenderCallbackStruct cb = { render, NULL };
    AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    AudioUnitInitialize(unit);
    AudioOutputUnitStart(unit);
}

// ---------- loading through the engine's file callbacks ----------
// returns host buffer with the file contents from `offset`, or NULL
static uint8_t *read_all(CPU *c, uint32_t name, uint32_t offset, uint32_t o, uint32_t cl, uint32_t rd, uint32_t sk, uint32_t *len) {
    uint32_t tmp = gmalloc(32); memset(M + tmp, 0, 32);   // [0] filesize [4] handle [8] userdata [12] bytesread
    uint32_t a[5] = { name, 0, tmp, tmp + 4, tmp + 8 };
    uint32_t r = guest_call(c, o, 5, a);
    if (r) { gfree(tmp); return NULL; }
    uint32_t fsize = rd32(tmp), h = rd32(tmp + 4), ud = rd32(tmp + 8);
    if (offset) { uint32_t s[3] = { h, offset, ud }; guest_call(c, sk, 3, s); }
    uint32_t cap = fsize > offset ? fsize - offset : 1 << 16, n = 0;
    uint8_t *buf = malloc(cap + 1);
    uint32_t chunk = 1 << 16, gbuf = gmalloc(chunk);
    // Like FMOD, never read past the size the open callback reported: the engine's read callbacks
    // (e.g. Speex voice) do not detect end-of-file themselves.
    uint32_t limit = fsize > offset ? fsize - offset : 0xFFFFFFFFu;
    while (n < limit) {
        uint32_t want = limit - n < chunk ? limit - n : chunk;
        wr32(tmp + 12, 0);
        uint32_t ra[5] = { h, gbuf, want, tmp + 12, ud };
        uint32_t rr = guest_call(c, rd, 5, ra), got = rd32(tmp + 12);
        if (got) {
            if (n + got > cap) { cap = (n + got) * 2; buf = realloc(buf, cap + 1); }
            memcpy(buf + n, M + gbuf, got); n += got;
        }
        if (rr || !got) break;
    }
    uint32_t ca[2] = { h, ud }; guest_call(c, cl, 2, ca);
    gfree(gbuf); gfree(tmp);
    *len = n;
    return buf;
}

// ---------- background Ogg decoding ----------
struct DecodeJob { int snd; int abandoned; uint8_t *data; uint32_t len; float *pcm; uint32_t frames; int nch; };
static void *decode_thread(void *p) {
    DecodeJob *j = p;
    int err = 0;
    stb_vorbis *v = stb_vorbis_open_memory(j->data, (int)j->len, &err, NULL);
    uint32_t done = 0;
    if (v) {
        while (done < j->frames) {
            int got = stb_vorbis_get_samples_float_interleaved(v, j->nch, j->pcm + (size_t)done * j->nch, (int)((j->frames - done) * (uint32_t)j->nch));
            if (got <= 0) break;
            done += (uint32_t)got;
            pthread_mutex_lock(&mx); int stop = j->abandoned; pthread_mutex_unlock(&mx);
            if (stop) break;
        }
        stb_vorbis_close(v);
    }
    free(j->data);
    pthread_mutex_lock(&mx);
    if (j->abandoned) free(j->pcm);   // the sound was released while decoding
    else { snds[j->snd].ready = 1; snds[j->snd].job = NULL; if (done < j->frames) snds[j->snd].frames = done; }
    pthread_mutex_unlock(&mx);
    free(j);
    return NULL;
}
static void start_decode(int i, uint8_t *data, uint32_t len) {   // called with mx held
    DecodeJob *j = calloc(1, sizeof *j);
    j->snd = i; j->data = data; j->len = len; j->pcm = snds[i].pcm; j->frames = snds[i].frames; j->nch = snds[i].nch;
    snds[i].job = j;
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, decode_thread, j);
}

// createSound/createStream(this, name_or_data, mode, exinfo*, Sound**)
static void create(CPU *c) {
    uint32_t name = ARG(1), mode = ARG(2), ex = ARG(3), out = ARG(4);
    uint32_t cbsize = ex ? rd32(ex) : 0;
    uint32_t offset = cbsize > 8 ? rd32(ex + 8) : 0;
    uint32_t o = fs_open, cl = fs_close, rd = fs_read, sk = fs_seek;
    if (cbsize > 0x58 && rd32(ex + 0x4c)) { o = rd32(ex + 0x4c); cl = rd32(ex + 0x50); rd = rd32(ex + 0x54); sk = rd32(ex + 0x58); }
    if (!o) { fprintf(stderr, "[fmod] no file system for %s\n", (char *)G2H(name)); RET(FMOD_ERR_FILE_NOTFOUND); return; }
    uint32_t len = 0;
    uint8_t *data = read_all(c, name, offset, o, cl, rd, sk, &len);
    if (!data) { RET(FMOD_ERR_FILE_NOTFOUND); return; }
    float *pcm = NULL; uint32_t frames = 0; int nch = 1; float rate = 44100;
    if (mode & M_OPENRAW) {   // raw PCM16 described by exinfo
        nch = (int)rd32(ex + 12); rate = (float)rd32(ex + 16);
        if (nch < 1) nch = 1;
        frames = len / (2u * (uint32_t)nch);
        pcm = malloc((size_t)frames * nch * sizeof(float) + 4);
        const int16_t *s = (const int16_t *)data;
        for (uint32_t i = 0; i < frames * (uint32_t)nch; i++) pcm[i] = s[i] / 32768.0f;
        free(data); data = NULL;
    } else {
        // Ogg Vorbis: header gives format and exact length now; the audio decodes in the background
        // (like FMOD's non-blocking streams) so opening music never stalls the game thread.
        int err = 0;
        stb_vorbis *v = stb_vorbis_open_memory(data, (int)len, &err, NULL);
        if (!v) { fprintf(stderr, "[fmod] cannot decode %s (%u bytes)\n", (char *)G2H(name), len); free(data); RET(FMOD_ERR_FORMAT); return; }
        stb_vorbis_info vi = stb_vorbis_get_info(v);
        nch = vi.channels; rate = (float)vi.sample_rate; frames = stb_vorbis_stream_length_in_samples(v);
        pcm = calloc((size_t)frames * nch + 4, sizeof(float));
        stb_vorbis_close(v);
    }
    pthread_mutex_lock(&mx);
    int i;
    for (i = 0; i < MAXSND && snds[i].used; i++) ;
    if (i == MAXSND) { pthread_mutex_unlock(&mx); free(pcm); free(data); RET(FMOD_ERR_FORMAT); return; }
    snds[i] = (Snd){ 1, new_handle(K_SOUND, i), pcm, frames, nch, rate, (mode & M_LOOP_NORMAL) ? -1 : 0, data == NULL, NULL };
    if (data) start_decode(i, data, len);
    pthread_mutex_unlock(&mx);
    T("create %s: %u frames, %d ch, %.0f Hz\n", (char *)G2H(name), frames, nch, rate);
    wr32(out, snds[i].g);
    RET(FMOD_OK);
}

// ---------- C API ----------
void I__FMOD_Debug_SetLevel(CPU *c) { RET(FMOD_OK); }
void I__FMOD_Memory_Initialize(CPU *c) { RET(FMOD_OK); }
void I__FMOD_Memory_GetStats(CPU *c) { if (ARG(0)) wr32(ARG(0), 0); if (ARG(1)) wr32(ARG(1), 0); RET(FMOD_OK); }
void I__FMOD_System_Create(CPU *c) {
    if (!g_system) g_system = new_handle(K_SYSTEM, 0);
    wr32(ARG(0), g_system);
    RET(FMOD_OK);
}

// ---------- System ----------
void I___ZN4FMOD6System10getVersionEPj(CPU *c) { wr32(ARG(1), 0x00043400); RET(FMOD_OK); }
void I___ZN4FMOD6System4initEijPv(CPU *c) {
    pthread_mutex_lock(&mx);
    if (master < 0) { master = 0; grps[0] = (Grp){ 1, new_handle(K_GROUP, 0), 1.0f, 0, 0, -1 }; }
    pthread_mutex_unlock(&mx);
    audio_start();
    RET(FMOD_OK);
}
void I___ZN4FMOD6System11setCallbackEPF11FMOD_RESULTP11FMOD_SYSTEM24FMOD_SYSTEM_CALLBACKTYPEPvS5_E(CPU *c) { RET(FMOD_OK); }
void I___ZN4FMOD6System13setFileSystemEPF11FMOD_RESULTPKciPjPPvS6_EPFS1_S5_S5_EPFS1_S5_S5_jS4_S5_EPFS1_S5_jS5_EPFS1_P18FMOD_ASYNCREADINFOS5_ESA_i(CPU *c) {
    fs_open = ARG(1); fs_close = ARG(2); fs_read = ARG(3); fs_seek = ARG(4);
    RET(FMOD_OK);
}
void I___ZN4FMOD6System19setReverbPropertiesEPK22FMOD_REVERB_PROPERTIES(CPU *c) { RET(FMOD_OK); }
void I___ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE(CPU *c) { create(c); }
void I___ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE(CPU *c) { create(c); }
void I___ZN4FMOD6System18createChannelGroupEPKcPPNS_12ChannelGroupE(CPU *c) {
    pthread_mutex_lock(&mx); int g = new_group(); pthread_mutex_unlock(&mx);
    wr32(ARG(2), g >= 0 ? grps[g].g : 0);
    RET(FMOD_OK);
}
void I___ZN4FMOD6System21getMasterChannelGroupEPPNS_12ChannelGroupE(CPU *c) { wr32(ARG(1), master >= 0 ? grps[master].g : 0); RET(FMOD_OK); }
void I___ZN4FMOD6System7releaseEv(CPU *c) { if (unit) { AudioOutputUnitStop(unit); } RET(FMOD_OK); }
// playSound(this, FMOD_CHANNELINDEX, Sound*, bool paused, Channel**)
void I___ZN4FMOD6System9playSoundE17FMOD_CHANNELINDEXPNS_5SoundEbPPNS_7ChannelE(CPU *c) {
    int32_t index = (int32_t)ARG(1); int s = SND(ARG(2)); int paused = ARG(3) & 0xFF; uint32_t out = ARG(4);
    if (s < 0) { RET(FMOD_ERR_FORMAT); return; }
    pthread_mutex_lock(&mx);
    int i = -1;
    if (index == -2 && out) i = CH(rd32(out));      // FMOD_CHANNEL_REUSE
    if (i < 0) for (int k = 0; k < MAXCH; k++) if (!chans[k].used) { i = k; break; }
    if (i < 0) for (int k = 0; k < MAXCH; k++) if (!chans[k].playing) { i = k; break; }   // steal a finished one
    if (i < 0) { pthread_mutex_unlock(&mx); RET(FMOD_ERR_FORMAT); return; }
    uint32_t g = chans[i].used ? chans[i].g : new_handle(K_CHANNEL, i);
    chans[i] = (Chan){ .used = 1, .sound = s, .group = master, .paused = paused, .playing = 1, .g = g,
                       .pos = 0, .vol = 1.0f, .freq = snds[s].rate, .loops_left = snds[s].loopcount };
    pthread_mutex_unlock(&mx);
    T("play sound %d on channel %d%s (loops %d)\n", s, i, paused ? " paused" : "", snds[s].loopcount);
    if (out) wr32(out, g);
    RET(FMOD_OK);
}
// update: deliver end-of-playback callbacks on the engine's thread
void I___ZN4FMOD6System6updateEv(CPU *c) {
    for (int i = 0; i < MAXCH; i++) {
        pthread_mutex_lock(&mx);
        int fire = chans[i].used && chans[i].ended; uint32_t cb = chans[i].cb, g = chans[i].g;
        if (fire) chans[i].ended = 0;
        pthread_mutex_unlock(&mx);
        if (fire && cb) { uint32_t a[4] = { g, 0 /* FMOD_CHANNEL_CALLBACKTYPE_END */, 0, 0 }; guest_call(c, cb, 4, a); }
    }
    RET(FMOD_OK);
}

// ---------- Sound ----------
void I___ZN4FMOD5Sound11getDefaultsEPfS1_S1_Pi(CPU *c) {   // (freq*, volume*, pan*, priority*)
    int s = SND(ARG(0));
    if (ARG(1)) wrf(ARG(1), s >= 0 ? snds[s].rate : 44100.0f);
    if (ARG(2)) wrf(ARG(2), 1.0f);
    if (ARG(3)) wrf(ARG(3), 0.0f);
    if (ARG(4)) wr32(ARG(4), 128);
    RET(FMOD_OK);
}
void I___ZN4FMOD5Sound12getOpenStateEP14FMOD_OPENSTATEPjPbS4_(CPU *c) {
    int s = SND(ARG(0)), ready = s < 0 || snds[s].ready;
    if (ARG(1)) wr32(ARG(1), ready ? 0 : 1);   // FMOD_OPENSTATE_READY / LOADING
    if (ARG(2)) wr32(ARG(2), ready ? 100 : 0);
    if (ARG(3)) wr8(ARG(3), 0);
    if (ARG(4)) wr8(ARG(4), 0);
    RET(FMOD_OK);
}
void I___ZN4FMOD5Sound12setLoopCountEi(CPU *c) { int s = SND(ARG(0)); if (s >= 0) snds[s].loopcount = (int32_t)ARG(1); RET(FMOD_OK); }
void I___ZN4FMOD5Sound9getLengthEPjj(CPU *c) {
    int s = SND(ARG(0)); uint32_t tu = ARG(2), v = 0;
    if (s >= 0) {
        Snd *x = &snds[s];
        v = tu == TU_MS ? (uint32_t)((double)x->frames * 1000.0 / x->rate) : tu == TU_PCMBYTES ? x->frames * 2u * (uint32_t)x->nch : x->frames;
    }
    wr32(ARG(1), v);
    RET(FMOD_OK);
}
void I___ZN4FMOD5Sound7releaseEv(CPU *c) {
    int s = SND(ARG(0));
    if (s >= 0) {
        pthread_mutex_lock(&mx);
        for (int i = 0; i < MAXCH; i++) if (chans[i].used && chans[i].sound == s) { chans[i].playing = 0; chans[i].sound = -1; }
        if (snds[s].job) { snds[s].job->abandoned = 1; snds[s].job = NULL; }   // decode thread frees pcm
        else free(snds[s].pcm);
        snds[s].pcm = NULL; snds[s].used = 0;
        pthread_mutex_unlock(&mx);
    }
    RET(FMOD_OK);
}

// ---------- Channel ----------
#define CHAN_OR_OK(var) int var = CH(ARG(0)); if (var < 0) { RET(FMOD_OK); return; }
void I___ZN4FMOD7Channel9setVolumeEf(CPU *c) { CHAN_OR_OK(i); chans[i].vol = ARGF(1); RET(FMOD_OK); }
void I___ZN4FMOD7Channel9setPausedEb(CPU *c) { CHAN_OR_OK(i); chans[i].paused = ARG(1) & 0xFF; RET(FMOD_OK); }
void I___ZN4FMOD7Channel12setFrequencyEf(CPU *c) { CHAN_OR_OK(i); float f = ARGF(1); if (f > 0) chans[i].freq = f; RET(FMOD_OK); }
void I___ZN4FMOD7Channel12setLoopCountEi(CPU *c) { CHAN_OR_OK(i); chans[i].loops_left = (int32_t)ARG(1); RET(FMOD_OK); }
void I___ZN4FMOD7Channel15setChannelGroupEPNS_12ChannelGroupE(CPU *c) { CHAN_OR_OK(i); int g = GRP(ARG(1)); chans[i].group = g >= 0 ? g : master; RET(FMOD_OK); }
void I___ZN4FMOD7Channel19setReverbPropertiesEPK29FMOD_REVERB_CHANNELPROPERTIES(CPU *c) { RET(FMOD_OK); }
void I___ZN4FMOD7Channel11setUserDataEPv(CPU *c) { CHAN_OR_OK(i); chans[i].userdata = ARG(1); RET(FMOD_OK); }
void I___ZN4FMOD7Channel11getUserDataEPPv(CPU *c) { int i = CH(ARG(0)); wr32(ARG(1), i >= 0 ? chans[i].userdata : 0); RET(FMOD_OK); }
void I___ZN4FMOD7Channel11setCallbackEPF11FMOD_RESULTP12FMOD_CHANNEL25FMOD_CHANNEL_CALLBACKTYPEPvS5_E(CPU *c) { CHAN_OR_OK(i); chans[i].cb = ARG(1); RET(FMOD_OK); }
void I___ZN4FMOD7Channel9isPlayingEPb(CPU *c) { int i = CH(ARG(0)); wr8(ARG(1), i >= 0 && chans[i].playing); RET(FMOD_OK); }
void I___ZN4FMOD7Channel4stopEv(CPU *c) {
    CHAN_OR_OK(i);
    pthread_mutex_lock(&mx); int was = chans[i].playing; chans[i].playing = 0; if (was) chans[i].ended = 1; pthread_mutex_unlock(&mx);
    RET(FMOD_OK);
}
void I___ZN4FMOD7Channel11getPositionEPjj(CPU *c) {
    int i = CH(ARG(0)); uint32_t v = 0;
    if (i >= 0 && chans[i].sound >= 0) {
        Snd *s = &snds[chans[i].sound]; double p = chans[i].pos;
        v = ARG(2) == TU_MS ? (uint32_t)(p * 1000.0 / s->rate) : ARG(2) == TU_PCMBYTES ? (uint32_t)p * 2u * (uint32_t)s->nch : (uint32_t)p;
    }
    wr32(ARG(1), v);
    RET(FMOD_OK);
}
void I___ZN4FMOD7Channel11setPositionEjj(CPU *c) {
    CHAN_OR_OK(i);
    if (chans[i].sound >= 0) {
        Snd *s = &snds[chans[i].sound]; uint32_t v = ARG(1);
        double p = ARG(2) == TU_MS ? v * (double)s->rate / 1000.0 : ARG(2) == TU_PCMBYTES ? v / (2.0 * s->nch) : v;
        pthread_mutex_lock(&mx); chans[i].pos = p < s->frames ? p : 0; pthread_mutex_unlock(&mx);
    }
    RET(FMOD_OK);
}

// ---------- ChannelGroup ----------
#define GRP_OR_OK(var) int var = GRP(ARG(0)); if (var < 0) { RET(FMOD_OK); return; }
void I___ZN4FMOD12ChannelGroup9setVolumeEf(CPU *c) { GRP_OR_OK(g); grps[g].vol = ARGF(1); RET(FMOD_OK); }
void I___ZN4FMOD12ChannelGroup9getVolumeEPf(CPU *c) { int g = GRP(ARG(0)); wrf(ARG(1), g >= 0 ? grps[g].vol : 1.0f); RET(FMOD_OK); }
void I___ZN4FMOD12ChannelGroup7setMuteEb(CPU *c) { GRP_OR_OK(g); grps[g].mute = ARG(1) & 0xFF; RET(FMOD_OK); }
void I___ZN4FMOD12ChannelGroup7getMuteEPb(CPU *c) { int g = GRP(ARG(0)); wr8(ARG(1), g >= 0 && grps[g].mute); RET(FMOD_OK); }
void I___ZN4FMOD12ChannelGroup9setPausedEb(CPU *c) { GRP_OR_OK(g); grps[g].paused = ARG(1) & 0xFF; RET(FMOD_OK); }
void I___ZN4FMOD12ChannelGroup9getPausedEPb(CPU *c) { int g = GRP(ARG(0)); wr8(ARG(1), g >= 0 && grps[g].paused); RET(FMOD_OK); }
void I___ZN4FMOD12ChannelGroup8addGroupEPS0_(CPU *c) { GRP_OR_OK(g); int child = GRP(ARG(1)); if (child >= 0 && child != g) grps[child].parent = g; RET(FMOD_OK); }
void I___ZN4FMOD12ChannelGroup7releaseEv(CPU *c) { GRP_OR_OK(g); if (g != master) grps[g].used = 0; RET(FMOD_OK); }
