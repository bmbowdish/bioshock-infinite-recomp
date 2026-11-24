// ===== Added by Claude Fable - 2026-11-24 — index of changes in this file =====
//  * Movie audio: the intro/cut-scene videos were silent. audio_open / audio_push / aq_cb / audio_close decode the
//    movie's sound track with FFmpeg and play it through a macOS AudioQueue.
//  * BinkPause pauses/resumes the sound, BinkSetVolume scales it, BinkSetSoundTrack picks the track.
//  Known gap: the game's own volume sliders do not reach movie audio (only BinkSetVolume does).
// =====================================================================
// Bink 1 movies for 32-bit games, decoded with FFmpeg's binkvideo decoder (when built with HAVE_FFMPEG).
//
// The game drives the Bink "frame buffers" API: it asks for buffer sizes (BinkGetFrameBuffersInfo),
// allocates the planes itself, registers them (BinkRegisterFrameBuffers), then per frame calls
// BinkWait / BinkDoFrame / BinkGetRects / BinkNextFrame and uploads Frames[FrameNum] to textures.
//
// Guest structures (32-bit):
//   HBINK prefix: +0 Width, +4 Height, +8 Frames, +0xC FrameNum (1-based), +0x10 LastFrameNum,
//                 +0x14 FrameRate, +0x18 FrameRateDiv, +0x1C ReadError (must stay 0)
//   BINKFRAMEBUFFERS: +0 TotalFrames, +4 YABufferWidth, +8 YABufferHeight, +0xC cRcBBufferWidth,
//                 +0x10 cRcBBufferHeight, +0x14 FrameNum (buffer index), +0x18 Frames[]:
//                 each 0x30 = Y, cR, cB, A planes of {Allocate, Buffer, BufferPitch}
// Without FFmpeg, movies still open and play to their end at the right speed, as black frames.
// Audio tracks are ignored (the game's audio engine is not hooked up).
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#ifdef HAVE_FFMPEG
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#endif

#define RET(v) (c->eax = (uint32_t)(v))
#define NB 16
typedef struct {
    uint32_t h;                 // guest HBINK
    const uint8_t *data; size_t size, pos;   // in-memory file
    uint32_t frames, w, ht, fr, frd, alpha;
    double start; uint32_t start_frame;      // pacing clock
    uint32_t fb;                // registered BINKFRAMEBUFFERS (guest), 0 if none
    size_t mapped;              // nonzero: data is an mmap of a movie file
    int paused;
#ifdef HAVE_FFMPEG
    AVFormatContext *fmt; AVIOContext *io; AVCodecContext *dec; int vstream;
    AVPacket *pkt; AVFrame *frm; int dec_ok;
    // audio (Claude Fable - 2026-11-24): one track decoded by FFmpeg and played through an AudioQueue
    AVCodecContext *adec; AVFrame *afrm; int astream, ach, arate, astarted, amu_ok, volset;
    AudioQueueRef aq; float *ring; size_t rcap, rhead, rcount; pthread_mutex_t amu; float vol;
#endif
} Movie;
static Movie mv[NB];

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static Movie *find(uint32_t h) { for (int i = 0; i < NB; i++) if (mv[i].h && mv[i].h == h) return &mv[i]; return NULL; }
static int verbose(void) { static int v = -1; if (v < 0) v = getenv("RT32_BINK_DEBUG") != NULL; return v; }

#ifdef HAVE_FFMPEG
static int io_read(void *o, uint8_t *buf, int n) {
    Movie *m = o; size_t left = m->size - m->pos; if (!left) return AVERROR_EOF;
    if ((size_t)n > left) n = (int)left;
    memcpy(buf, m->data + m->pos, n); m->pos += n; return n;
}
static int64_t io_seek(void *o, int64_t off, int whence) {
    Movie *m = o;
    if (whence == AVSEEK_SIZE) return (int64_t)m->size;
    whence &= ~AVSEEK_FORCE;
    int64_t p = whence == SEEK_SET ? off : whence == SEEK_CUR ? (int64_t)m->pos + off : whence == SEEK_END ? (int64_t)m->size + off : -1;
    if (p < 0 || p > (int64_t)m->size) return -1;
    m->pos = (size_t)p; return p;
}
// ---- movie audio (Claude Fable - 2026-11-24). Bink plays its own sound track; the shim used to ignore it. ----
// The demuxer hands out audio packets interleaved with the video, so audio is decoded as the video advances and queued
// in a ring that an AudioQueue drains. RT32_BINK_NOAUDIO=1 turns it off. BinkSetVolume scales it (32768 = full).
static uint32_t bink_track0; static int bink_track_set;
// AudioQueue callback (runs on a system audio thread): fill one buffer from the ring. If the ring runs dry the rest
// is filled with silence, so a late video frame gives a brief gap instead of stopping the queue.
static void aq_cb(void *ud, AudioQueueRef q, AudioQueueBufferRef b) {
    Movie *m = ud; size_t want = b->mAudioDataBytesCapacity / sizeof(float), got = 0; float *out = b->mAudioData;
    pthread_mutex_lock(&m->amu);
    float v = m->vol;
    while (got < want && m->rcount) { out[got++] = m->ring[m->rhead] * v; m->rhead = (m->rhead + 1) % m->rcap; m->rcount--; }
    pthread_mutex_unlock(&m->amu);
    while (got < want) out[got++] = 0;
    b->mAudioDataByteSize = (UInt32)(want * sizeof(float));
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}
// Called on the game's thread for each decoded audio frame: convert whatever sample format FFmpeg produced to
// interleaved float and append to the ring (samples are dropped if the ring is full: about 4 s of sound).
// Playback starts only once 100 ms is buffered, so it does not stutter at the start.
static void audio_push(Movie *m) {
    AVFrame *f = m->afrm; int ch = m->ach, n = f->nb_samples;
    pthread_mutex_lock(&m->amu);
    for (int i = 0; i < n; i++) for (int k = 0; k < ch; k++) {
        float sm = 0;
        switch (f->format) {
        case AV_SAMPLE_FMT_FLT:  sm = ((const float *)f->data[0])[i * ch + k]; break;
        case AV_SAMPLE_FMT_FLTP: sm = ((const float *)f->extended_data[k])[i]; break;
        case AV_SAMPLE_FMT_S16:  sm = ((const int16_t *)f->data[0])[i * ch + k] / 32768.0f; break;
        case AV_SAMPLE_FMT_S16P: sm = ((const int16_t *)f->extended_data[k])[i] / 32768.0f; break;
        default: break;
        }
        if (m->rcount < m->rcap) { m->ring[(m->rhead + m->rcount) % m->rcap] = sm; m->rcount++; }
    }
    size_t have = m->rcount;
    pthread_mutex_unlock(&m->amu);
    if (!m->astarted && m->aq && have >= (size_t)(m->arate * ch / 10)) {   // 100 ms buffered: start
        m->astarted = 1;
        for (int i = 0; i < 3; i++) { AudioQueueBufferRef b; if (AudioQueueAllocateBuffer(m->aq, (UInt32)((m->arate / 20) * ch * sizeof(float)), &b) == noErr) aq_cb(m, m->aq, b); }
        if (!m->paused) AudioQueueStart(m->aq, NULL);
    }
}
// Stop and free everything audio. Safe to call when audio was never opened.
static void audio_close(Movie *m) {
    if (m->aq) { AudioQueueStop(m->aq, true); AudioQueueDispose(m->aq, true); m->aq = NULL; }
    if (m->adec) avcodec_free_context(&m->adec);
    if (m->afrm) av_frame_free(&m->afrm);
    if (m->ring) { free(m->ring); m->ring = NULL; }
    if (m->amu_ok) { pthread_mutex_destroy(&m->amu); m->amu_ok = 0; }
    m->astream = -1; m->astarted = 0; m->rcount = m->rhead = 0;
}
// Find the sound track (the one chosen with BinkSetSoundTrack, else the first), open its decoder and create the
// AudioQueue. Any failure just leaves the movie silent; video is unaffected.
static void audio_open(Movie *m) {
    m->astream = -1;
    if (getenv("RT32_BINK_NOAUDIO")) return;
    int na = 0;
    for (unsigned i = 0; i < m->fmt->nb_streams; i++) {
        AVStream *st = m->fmt->streams[i]; if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        na++;
        if (m->astream < 0 || (bink_track_set && st->id == (int)bink_track0)) m->astream = (int)i;
    }
    if (m->astream < 0) return;
    AVStream *st = m->fmt->streams[m->astream];
    const AVCodec *cd = avcodec_find_decoder(st->codecpar->codec_id);
    if (!cd) { fprintf(stderr, "[bink] audio: no decoder for codec %d\n", st->codecpar->codec_id); m->astream = -1; return; }
    m->adec = avcodec_alloc_context3(cd);
    avcodec_parameters_to_context(m->adec, st->codecpar);
    if (avcodec_open2(m->adec, cd, NULL) < 0) { fprintf(stderr, "[bink] audio: decoder open failed\n"); audio_close(m); return; }
    m->afrm = av_frame_alloc();
    m->ach = m->adec->ch_layout.nb_channels; m->arate = m->adec->sample_rate;
    if (m->ach < 1 || m->ach > 8 || m->arate < 8000) { fprintf(stderr, "[bink] audio: unsupported layout (%d ch, %d Hz)\n", m->ach, m->arate); audio_close(m); return; }
    pthread_mutex_init(&m->amu, NULL); m->amu_ok = 1;
    m->rcap = (size_t)m->arate * (size_t)m->ach * 4; m->ring = malloc(m->rcap * sizeof(float)); m->rhead = m->rcount = 0;
    if (!m->volset) m->vol = 1.0f;
    AudioStreamBasicDescription d; memset(&d, 0, sizeof d);
    d.mSampleRate = m->arate; d.mFormatID = kAudioFormatLinearPCM; d.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    d.mChannelsPerFrame = (UInt32)m->ach; d.mBitsPerChannel = 32; d.mBytesPerFrame = d.mBytesPerPacket = (UInt32)(4 * m->ach); d.mFramesPerPacket = 1;
    if (AudioQueueNewOutput(&d, aq_cb, m, NULL, NULL, 0, &m->aq) != noErr) { m->aq = NULL; fprintf(stderr, "[bink] audio: AudioQueueNewOutput failed\n"); audio_close(m); return; }
    fprintf(stderr, "[bink] audio: %d track(s), playing stream %d (track id %d): %d ch, %d Hz, volume %.2f\n", na, m->astream, st->id, m->ach, m->arate, m->vol);
}
static void dec_close(Movie *m) {
    audio_close(m);
    if (m->dec) avcodec_free_context(&m->dec);
    if (m->fmt) avformat_close_input(&m->fmt);
    if (m->io) { av_freep(&m->io->buffer); avio_context_free(&m->io); }
    if (m->pkt) av_packet_free(&m->pkt);
    if (m->frm) av_frame_free(&m->frm);
    m->dec_ok = 0;
}
static int dec_open(Movie *m) {
    m->pos = 0;
    uint8_t *iobuf = av_malloc(65536);
    m->io = avio_alloc_context(iobuf, 65536, 0, m, io_read, NULL, io_seek);
    m->fmt = avformat_alloc_context(); m->fmt->pb = m->io;
    if (avformat_open_input(&m->fmt, NULL, NULL, NULL) < 0) { m->fmt = NULL; fprintf(stderr, "[bink] ffmpeg: can't open the in-memory movie\n"); return 0; }
    avformat_find_stream_info(m->fmt, NULL);
    m->vstream = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (m->vstream < 0) { fprintf(stderr, "[bink] ffmpeg: no video stream\n"); return 0; }
    AVStream *st = m->fmt->streams[m->vstream];
    const AVCodec *cd = avcodec_find_decoder(st->codecpar->codec_id);
    if (!cd) { fprintf(stderr, "[bink] ffmpeg: no decoder for codec %d\n", st->codecpar->codec_id); return 0; }
    m->dec = avcodec_alloc_context3(cd);
    avcodec_parameters_to_context(m->dec, st->codecpar);
    if (avcodec_open2(m->dec, cd, NULL) < 0) { fprintf(stderr, "[bink] ffmpeg: decoder open failed\n"); return 0; }
    m->pkt = av_packet_alloc(); m->frm = av_frame_alloc();
    m->dec_ok = 1;
    audio_open(m);
    return 1;
}
// decode the next video frame into m->frm; 0 at end of stream or on error
static int dec_next(Movie *m) {
    for (;;) {
        int r = avcodec_receive_frame(m->dec, m->frm);
        if (r == 0) return 1;
        if (r != AVERROR(EAGAIN)) return 0;
        for (;;) {
            r = av_read_frame(m->fmt, m->pkt);
            if (r < 0) { avcodec_send_packet(m->dec, NULL); break; }
            if (m->pkt->stream_index == m->vstream) { avcodec_send_packet(m->dec, m->pkt); av_packet_unref(m->pkt); break; }
            // audio packets arrive interleaved with video packets: decode them as we pass them on the way to the next video frame
            if (m->adec && m->pkt->stream_index == m->astream) {
                if (avcodec_send_packet(m->adec, m->pkt) == 0) while (avcodec_receive_frame(m->adec, m->afrm) == 0) audio_push(m);
            }
            av_packet_unref(m->pkt);
        }
    }
}
// copy the decoded planes into the registered buffer set idx
static void copy_planes(Movie *m, int idx) {
    if (!m->fb) return;
    uint32_t set = m->fb + 0x18 + 0x30 * (uint32_t)idx;
    AVFrame *f = m->frm;
    for (int p = 0; p < 4; p++) {
        uint32_t buf = rd32(set + 12 * p + 4), pitch = rd32(set + 12 * p + 8);
        if (!buf || !pitch) continue;
        int src = p == 0 ? 0 : p == 1 ? 2 : p == 2 ? 1 : 3;      // Bink order Y, cR, cB, A; FFmpeg Y, U(Cb), V(Cr), A
        if (src >= AV_NUM_DATA_POINTERS || !f->data[src]) {        // no alpha in the stream: opaque
            if (p == 3) { uint32_t hh = rd32(m->fb + 8); for (uint32_t y = 0; y < hh; y++) memset(G2H(buf + y * pitch), 255, pitch); }
            continue;
        }
        int pw = p == 1 || p == 2 ? (f->width + 1) / 2 : f->width, ph = p == 1 || p == 2 ? (f->height + 1) / 2 : f->height;
        int n = pw < (int)pitch ? pw : (int)pitch;
        for (int y = 0; y < ph; y++) memcpy(G2H(buf + (uint32_t)y * pitch), f->data[src] + (size_t)y * f->linesize[src], n);
    }
}
#endif

// BinkOpen(name_or_memory, flags)
// A movie given by file name (no BINKFROMMEMORY flag): Windows-style relative path, mapped read-only.
// Tried relative to the working directory and to Binaries/Win32 (where Windows builds run from).
static const uint8_t *map_movie(const char *name, size_t *size) {
    char rel[1024]; snprintf(rel, sizeof rel, "%s", name);
    for (char *q = rel; *q; q++) if (*q == '\\') *q = '/';
    char cand[2][1100]; snprintf(cand[0], sizeof cand[0], "%s", rel); snprintf(cand[1], sizeof cand[1], "Binaries/Win32/%s", rel);
    for (int i = 0; i < 2; i++) {
        int fd = open(cand[i], O_RDONLY); if (fd < 0) continue;
        struct stat st; if (fstat(fd, &st) != 0 || st.st_size < 44) { close(fd); continue; }
        void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
        if (m == MAP_FAILED) continue;
        *size = (size_t)st.st_size; return m;
    }
    return NULL;
}
static void BinkOpen_impl(CPU *c) {
    const uint8_t *h = ARG(0) ? (const uint8_t *)G2H(ARG(0)) : NULL;
    size_t mapped = 0; const char *fname = NULL;
    if (h && !(ARG(1) & 0x04000000u)) {   // not BINKFROMMEMORY: a file name
        fname = (const char *)h; h = map_movie(fname, &mapped);
        if (!h) { fprintf(stderr, "[bink] BinkOpen(%s): file not found\n", fname); RET(0); return; }
    }
    if (!h || !(h[0] == 'B' && h[1] == 'I' && h[2] == 'K')) {
        fprintf(stderr, "[bink] BinkOpen(%s, 0x%x): not a Bink 1 movie\n", fname ? fname : "memory", ARG(1));
        if (mapped) munmap((void *)h, mapped);
        RET(0); return;
    }
    Movie *m = NULL; for (int i = 0; i < NB; i++) if (!mv[i].h) { m = &mv[i]; break; }
    if (!m) { fprintf(stderr, "[bink] too many open movies\n"); RET(0); return; }
    memset(m, 0, sizeof *m);
    m->data = h; m->size = (size_t)(*(const uint32_t *)(h + 4)) + 8; m->mapped = mapped;
    if (mapped && m->size > mapped) m->size = mapped;
    m->frames = *(const uint32_t *)(h + 8); m->w = *(const uint32_t *)(h + 20); m->ht = *(const uint32_t *)(h + 24);
    m->fr = *(const uint32_t *)(h + 28); m->frd = *(const uint32_t *)(h + 32); if (!m->fr || !m->frd) { m->fr = 30; m->frd = 1; }
    m->alpha = (*(const uint32_t *)(h + 36) >> 20) & 1;
    uint32_t b = gmalloc(4096); memset(G2H(b), 0, 4096);
    wr32(b + 0, m->w); wr32(b + 4, m->ht); wr32(b + 8, m->frames); wr32(b + 0xC, 1); wr32(b + 0x10, 0);
    wr32(b + 0x14, m->fr); wr32(b + 0x18, m->frd);
    m->h = b; m->start = now_s(); m->start_frame = 1;
    int dec = 0;
#ifdef HAVE_FFMPEG
    dec = dec_open(m); if (!dec) dec_close(m);
#endif
    fprintf(stderr, "[bink] BinkOpen(%s %ux%u, %u frames at %.2f fps%s, %zu bytes) -> 0x%x%s\n", fname ? fname : "in-memory", m->w, m->ht, m->frames,
            (double)m->fr / m->frd, m->alpha ? ", alpha" : "", m->size, b, dec ? " (ffmpeg)" : " (no decoder: black frames)");
    RET(b);
}
void I__BinkClose(CPU *c) {
    Movie *m = find(ARG(0)); if (!m) { RET(0); return; }
    if (verbose()) fprintf(stderr, "[bink] BinkClose(0x%x) at frame %u/%u\n", m->h, rd32(m->h + 0xC), m->frames);
#ifdef HAVE_FFMPEG
    dec_close(m);
#endif
    if (m->mapped) munmap((void *)m->data, m->mapped);
    gfree(m->h); m->h = 0; RET(0);
}
// 1 = not time for the next frame yet
void I__BinkWait(CPU *c) {
    Movie *m = find(ARG(0)); if (!m || m->paused) { RET(m ? 1 : 0); return; }
    uint32_t fnum = rd32(m->h + 0xC);
    double due = m->start + (double)(fnum - m->start_frame) * m->frd / m->fr;
    RET(now_s() < due ? 1 : 0);
}
static void BinkDoFrame_impl(CPU *c) {
    Movie *m = find(ARG(0)); if (!m) { RET(0); return; }
    uint32_t fnum = rd32(m->h + 0xC);
    int idx = 0;
    if (m->fb) { uint32_t tot = rd32(m->fb); idx = tot ? (int)((rd32(m->fb + 0x14) + 1) % tot) : 0; }
#ifdef HAVE_FFMPEG
    if (m->dec_ok) {
        if (dec_next(m)) copy_planes(m, idx);
        else if (verbose()) fprintf(stderr, "[bink] decode ended early at frame %u\n", fnum);
    }
#endif
    if (m->fb) wr32(m->fb + 0x14, (uint32_t)idx);
    wr32(m->h + 0x10, fnum);
    if (verbose() && (fnum <= 2 || fnum % 60 == 0)) fprintf(stderr, "[bink] DoFrame 0x%x frame %u/%u -> buffer %d\n", m->h, fnum, m->frames, idx);
    RET(0);
}
void I__BinkNextFrame(CPU *c) {
    Movie *m = find(ARG(0)); if (!m) return;
    uint32_t f = rd32(m->h + 0xC); if (f < m->frames) wr32(m->h + 0xC, f + 1); else wr32(m->h + 0xC, f + 1);   // FrameNum > Frames only transiently
}
void I__BinkGoto(CPU *c) {   // (bink, frame, flags): only restart is supported
    Movie *m = find(ARG(0)); if (!m) return;
    uint32_t to = ARG(1) ? ARG(1) : 1;
    if (verbose()) fprintf(stderr, "[bink] BinkGoto(0x%x, %u)\n", m->h, to);
#ifdef HAVE_FFMPEG
    if (m->dec_ok && to <= 1) { dec_close(m); dec_open(m); }
#endif
    wr32(m->h + 0xC, to); m->start = now_s(); m->start_frame = to;
}
void I__BinkPause(CPU *c) {
    Movie *m = find(ARG(0)); if (!m) { RET(0); return; }
    int p = ARG(1) != 0;
    if (m->paused && !p) { m->start = now_s(); m->start_frame = rd32(m->h + 0xC); }
    m->paused = p;
#ifdef HAVE_FFMPEG
    if (m->aq && m->astarted) { if (p) AudioQueuePause(m->aq); else AudioQueueStart(m->aq, NULL); }
#endif
    RET(p);
}
void I__BinkShouldSkip(CPU *c) { RET(0); }
// (bink, BINKFRAMEBUFFERS*): sizes and which planes to allocate
void I__BinkGetFrameBuffersInfo(CPU *c) {
    Movie *m = find(ARG(0)); uint32_t fb = ARG(1); if (!m || !fb) { RET(0); return; }
    uint32_t w = (m->w + 15) & ~15u, h = (m->ht + 15) & ~15u;
    wr32(fb, 2); wr32(fb + 4, w); wr32(fb + 8, h); wr32(fb + 0xC, w / 2); wr32(fb + 0x10, h / 2); wr32(fb + 0x14, 1);
    for (int i = 0; i < 2; i++) for (int p = 0; p < 4; p++) wr32(fb + 0x18 + 0x30 * i + 12 * p, p < 3 || m->alpha ? 1 : 0);
    RET(0);
}
void I__BinkRegisterFrameBuffers(CPU *c) {
    Movie *m = find(ARG(0)); if (!m) { RET(0); return; }
    m->fb = ARG(1);
    if (verbose()) {
        fprintf(stderr, "[bink] RegisterFrameBuffers(0x%x, 0x%x): total %u, YA %ux%u, cRcB %ux%u, cur %u\n", m->h, m->fb, rd32(m->fb), rd32(m->fb + 4),
                rd32(m->fb + 8), rd32(m->fb + 0xC), rd32(m->fb + 0x10), rd32(m->fb + 0x14));
        for (int i = 0; i < 2; i++) for (int p = 0; p < 4; p++) { uint32_t q = m->fb + 0x18 + 0x30 * i + 12 * p;
            fprintf(stderr, "[bink]   set %d plane %d: alloc %u buf 0x%x pitch %u\n", i, p, rd32(q), rd32(q + 4), rd32(q + 8)); }
    }
    RET(1);
}
// (bink, flags): number of dirty rectangles; the whole frame counts as one
void I__BinkGetRects(CPU *c) { RET(find(ARG(0)) ? 1 : 0); }
void I__BinkGetKeyFrame(CPU *c) { RET(1); }
void I__BinkGetRealtime(CPU *c) { if (ARG(1)) memset(G2H(ARG(1)), 0, 32); }
void I__BinkSetMemory(CPU *c) { RET(0); }
void I__BinkSetVolume(CPU *c) {   // (bink, track id, volume): 32768 = full
#ifdef HAVE_FFMPEG
    Movie *m = find(ARG(0));
    if (m) { float v = (float)(int32_t)ARG(2) / 32768.0f; if (v < 0) v = 0; if (v > 2) v = 2; m->vol = v; m->volset = 1;
             if (verbose()) fprintf(stderr, "[bink] BinkSetVolume(track %u) -> %.2f\n", ARG(1), v); }
#endif
    RET(0);
}
void I__BinkSetSoundTrack(CPU *c) {   // (count, track ids): chosen before BinkOpen
#ifdef HAVE_FFMPEG
    if (ARG(0) && ARG(1)) { bink_track0 = rd32(ARG(1)); bink_track_set = 1; } else bink_track_set = 0;
#else
    (void)c;
#endif
}
void I__BinkSetSpeakerVolumes(CPU *c) { (void)c; }
void I__BinkSetIOSize(CPU *c) { (void)c; }
void I__BinkSetFileOffset(CPU *c) { (void)c; }

// Hitch logging: host time spent opening/decoding movies, read and reset by the host every present
#include <mach/mach_time.h>
volatile uint64_t rt_hitch_bink_ticks;
void I__BinkOpen(CPU *c) { uint64_t t = mach_absolute_time(); BinkOpen_impl(c); rt_hitch_bink_ticks += mach_absolute_time() - t; }
void I__BinkDoFrame(CPU *c) { uint64_t t = mach_absolute_time(); BinkDoFrame_impl(c); rt_hitch_bink_ticks += mach_absolute_time() - t; }
