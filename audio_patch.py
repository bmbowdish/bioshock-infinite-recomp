import os
R=os.path.expandvars("$HOME/mnt/bioshock-infinite-recomp/mac32/runtime/")
g=open(R+"cf.c").read()
a=g.index("void I__AUGraphStart(CPU *c)")
b=g.index("void I__AudioOutputUnitStop(CPU *c) { RET(0); }")+len("void I__AudioOutputUnitStop(CPU *c) { RET(0); }")
new=r'''// The game's mixer (Wwise) feeds its graph through a render callback. A real default-output AudioUnit
// stands in for the whole graph: CoreAudio's IO thread calls ca_render, which runs the guest callback on a
// guest CPU of its own with guest-memory buffers in the 32-bit AudioBufferList layout, then copies the
// samples out. The stream format is the one the game set on its units (or float32 stereo 48 kHz).
#include <AudioToolbox/AudioToolbox.h>
static uint32_t ca_proc, ca_ref;
static AudioStreamBasicDescription ca_fmt; static int ca_have_fmt;
static AudioUnit ca_out;
static OSStatus ca_render(void *refcon, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)refcon; (void)bus;
    static CPU *ac; static uint32_t g_flags, g_ts, g_abl, g_data, g_cap;
    uint32_t nb = io->mNumberBuffers, bytes = io->mBuffers[0].mDataByteSize;
    if (!ac) { ac = cpu_new(1u << 20); g_flags = gmalloc(16); g_ts = gmalloc(64); g_abl = gmalloc(4 + 12 * 8); }
    if (bytes * nb > g_cap) { if (g_data) gfree(g_data); g_cap = bytes * nb; g_data = gmalloc(g_cap); }
    wr32(g_flags, *flags);
    memset(G2H(g_ts), 0, 64); if (ts) { wrd(g_ts, ts->mSampleTime); wr64(g_ts + 8, ts->mHostTime); wr32(g_ts + 56, ts->mFlags & ~kAudioTimeStampWordClockTimeValid); }
    wr32(g_abl, nb);
    for (uint32_t i = 0; i < nb && i < 8; i++) { wr32(g_abl + 4 + 12 * i, io->mBuffers[i].mNumberChannels); wr32(g_abl + 8 + 12 * i, bytes); wr32(g_abl + 12 + 12 * i, g_data + i * bytes); }
    memset(G2H(g_data), 0, bytes * nb);
    uint32_t a[6] = { ca_ref, g_flags, g_ts, 0, frames, g_abl };
    uint32_t r = guest_call(ac, ca_proc, 6, a);
    for (uint32_t i = 0; i < nb && i < 8; i++) {
        uint32_t gp = rd32(g_abl + 12 + 12 * i), gn = rd32(g_abl + 8 + 12 * i);
        if (gn > io->mBuffers[i].mDataByteSize) gn = io->mBuffers[i].mDataByteSize;
        memcpy(io->mBuffers[i].mData, G2H(gp), gn);
    }
    *flags = rd32(g_flags);
    static int n; if (n++ == 0) fprintf(stderr, "[audio] first render: %u frames, %u buffers of %u bytes, guest returned %d\n", frames, nb, bytes, (int)r);
    return 0;
}
void I__AUGraphStart(CPU *c) {
    if (!ca_proc) { fprintf(stderr, "[audio] AUGraphStart without a render callback: silent\n"); RET(0); return; }
    if (getenv("RT32_NO_AUDIO")) { fprintf(stderr, "[audio] RT32_NO_AUDIO: silent\n"); RET(0); return; }
    if (!ca_out) {
        AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
        AudioComponent comp = AudioComponentFindNext(NULL, &d);
        if (!comp || AudioComponentInstanceNew(comp, &ca_out)) { fprintf(stderr, "[audio] no default output unit\n"); RET(0); return; }
        if (!ca_have_fmt) {
            ca_fmt = (AudioStreamBasicDescription){ 48000, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved,
                                                    4, 1, 4, 2, 32, 0 };
        }
        OSStatus e = AudioUnitSetProperty(ca_out, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &ca_fmt, sizeof ca_fmt);
        AURenderCallbackStruct cb = { ca_render, NULL };
        OSStatus e2 = AudioUnitSetProperty(ca_out, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
        OSStatus e3 = AudioUnitInitialize(ca_out);
        fprintf(stderr, "[audio] output: %.0f Hz, %u ch, %u bits, flags 0x%x (format %d, callback %d, init %d)\n", ca_fmt.mSampleRate,
                (unsigned)ca_fmt.mChannelsPerFrame, (unsigned)ca_fmt.mBitsPerChannel, (unsigned)ca_fmt.mFormatFlags, (int)e, (int)e2, (int)e3);
    }
    OSStatus s = AudioOutputUnitStart(ca_out);
    fprintf(stderr, "[audio] AUGraphStart -> real output %s (%d)\n", s ? "FAILED" : "started", (int)s);
    RET(0);
}
void I__AUGraphStop(CPU *c) { if (ca_out) AudioOutputUnitStop(ca_out); RET(0); }
void I__AUGraphConnectNodeInput(CPU *c) { RET(0); }
void I__AUGraphSetNodeInputCallback(CPU *c) {   // (graph, node, bus, const AURenderCallbackStruct *{proc, refCon})
    if (ARG(3)) { ca_proc = rd32(ARG(3)); ca_ref = rd32(ARG(3) + 4);
        fprintf(stderr, "[audio] render callback proc=%s refCon=0x%x (node %u bus %u)\n", rt_func_name(ca_proc), ca_ref, ARG(1), ARG(2)); }
    RET(0);
}
void I__AudioUnitSetProperty(CPU *c) {   // (unit, prop, scope, element, data*, size)
    uint32_t prop = ARG(1), scope = ARG(2), el = ARG(3), data = ARG(4), size = ARG(5);
    if (prop == kAudioUnitProperty_StreamFormat && data && size >= 40) {
        AudioStreamBasicDescription f; f.mSampleRate = rdd(data); f.mFormatID = rd32(data + 8); f.mFormatFlags = rd32(data + 12);
        f.mBytesPerPacket = rd32(data + 16); f.mFramesPerPacket = rd32(data + 20); f.mBytesPerFrame = rd32(data + 24);
        f.mChannelsPerFrame = rd32(data + 28); f.mBitsPerChannel = rd32(data + 32); f.mReserved = 0;
        fprintf(stderr, "[audio] SetProperty StreamFormat unit 0x%x scope %u el %u: %.0f Hz, %u ch, %u bits, flags 0x%x, %u bytes/frame\n",
                ARG(0), scope, el, f.mSampleRate, (unsigned)f.mChannelsPerFrame, (unsigned)f.mBitsPerChannel, (unsigned)f.mFormatFlags, (unsigned)f.mBytesPerFrame);
        if (scope == kAudioUnitScope_Input && f.mFormatID == kAudioFormatLinearPCM) { ca_fmt = f; ca_have_fmt = 1; }
    } else if (prop == kAudioUnitProperty_SetRenderCallback && data) {
        ca_proc = rd32(data); ca_ref = rd32(data + 4);
        fprintf(stderr, "[audio] SetRenderCallback proc=%s refCon=0x%x\n", rt_func_name(ca_proc), ca_ref);
    } else { static int n; if (n++ < 12) fprintf(stderr, "[audio] SetProperty %u scope %u el %u size %u (ignored)\n", prop, scope, el, size); }
    RET(0);
}
void I__AudioUnitSetParameter(CPU *c) { RET(0); }
void I__AudioUnitUninitialize(CPU *c) { RET(0); }
void I__AudioOutputUnitStop(CPU *c) { if (ca_out) AudioOutputUnitStop(ca_out); RET(0); }'''
g=g[:a]+new+g[b:]
open(R+"cf.c","w").write(g); print("ok")
