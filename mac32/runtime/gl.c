// ===== Added by Claude Fable - 2026-11-24 — index of changes in this file =====
//  * gl_ctx_lock/unlock (in gl_enter/gl_leave): every GL call holds the context lock once the game renders on its
//    own thread. Fixed a startup crash inside the GL driver.
//  * gl_tex_* counters: texture uploads / creations / deletions per 100 frames, shown on the [perf] line.
//  * gl_engine_w/h, gl_note_window_rect, gl_frame_publish: learn the game's real output resolution by watching what
//    it draws on the window.
//  * I__glBlitFramebuffer (hand-written; removed from tools/glgen.py): maps the game's final copy-to-window onto the
//    real window, and hands it to MetalFX (host.m host_mfx_blit) when the window is bigger than the game's picture.
//  * Buffer aliasing (RT32_ALIAS) is now on by default.
// Terms: "window framebuffer" = OpenGL framebuffer 0 = what ends up on screen. "engine pixels" = coordinates in the
// game's chosen resolution. gl_win_scale / gl_win_off_x / gl_win_off_y (set by host.m) convert engine pixels to
// real window pixels; MAPX/MAPY/MAPS apply that conversion.
// =====================================================================
#include <math.h>
#include <unistd.h>
#define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED
// Hand-written OpenGL shims: pointer translation, buffer mapping, strings, CGL/AGL queries,
// and the renderer's Objective-C bridge (Present / VRAM).
#define GL_SILENCE_DEPRECATION
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <OpenGL/gl3.h>
#include <OpenGL/OpenGL.h>

#define RET(v) (c->eax = (uint32_t)(v))

static int gl_tracing = -1;
volatile uint32_t gl_calls_frame, gl_draws_frame;
// per-frame histogram of GL entry points (RT32_GL_HIST=<present#>: printed at that present)
static const char *gl_hist_name[512]; static uint32_t gl_hist_n[512]; static uint64_t gl_hist_t[512]; static int gl_hist_cur = -1;
#include <mach/mach_time.h>
void gl_hist_dump(int frame) {   // sorted by host time spent inside the call (generated wrappers only)
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    int idx[512], k = 0; for (int i = 0; i < 512; i++) if (gl_hist_name[i] && gl_hist_n[i]) idx[k++] = i;
    for (int a = 0; a < k; a++) for (int b = a + 1; b < k; b++) if (gl_hist_t[idx[b]] > gl_hist_t[idx[a]]) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }
    double tot = 0; for (int a = 0; a < k; a++) tot += (double)gl_hist_t[idx[a]] * tb.numer / tb.denom / 1e6;
    fprintf(stderr, "[glhist] frame %d: %.2f ms inside generated GL calls\n", frame, tot);
    for (int a = 0; a < k; a++) { int i = idx[a]; double ms = (double)gl_hist_t[i] * tb.numer / tb.denom / 1e6;
        fprintf(stderr, "[glhist] %6u calls %8.3f ms %7.2f us/call  %s\n", gl_hist_n[i], ms, ms * 1000.0 / gl_hist_n[i], gl_hist_name[i]); }
}
void gl_hist_reset(void) { memset(gl_hist_n, 0, sizeof gl_hist_n); memset(gl_hist_t, 0, sizeof gl_hist_t); }
// RT32_GL_CHECK=1: report GL errors (attributed to the previous call) and incomplete draw framebuffers
static int gl_check = -1; static const char *gl_prev;
static void gl_check_step(const char *name) {
    if (gl_check < 0) gl_check = getenv("RT32_GL_CHECK") != NULL;
    if (!gl_check) return;
    static int budget = 300;
    if (gl_prev && budget > 0) {
        GLenum e = glGetError();
        if (e) { budget--; fprintf(stderr, "[glerr] 0x%x after %s\n", e, gl_prev); }
    }
    if (name[2] == 'D' && name[3] == 'r' && (name[6] == 'E' || name[6] == 'A') && budget > 0) {
        GLenum st = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            GLint fb = 0; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb); budget--;
            GLint t0 = 0, o0 = 0, td = 0, od = 0;
            if (fb) {
                glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &t0);
                glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &o0);
                glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &td);
                glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &od);
            }
            fprintf(stderr, "[glfbo] %s with draw fbo %d status 0x%x (color0 type 0x%x obj %d, depth type 0x%x obj %d)\n", name, fb, st, t0, o0, td, od);
            glGetError();
        }
    }
    gl_prev = name;
}
// RT32_GL_SAMPLES=<present#>: during the frame that ends with that present, wrap every draw in a
// GL_SAMPLES_PASSED query and log (draw, fbo, program, samples): tells "pixels written" from "geometry lost"
volatile int gl_samples_active;
uint32_t gl_samp_tex[64], gl_samp_prog[64]; int gl_samp_ntex;
static GLuint samp_q; static int samp_pending, samp_idx; static const char *samp_name; static GLint samp_fb, samp_prog, samp_c0, samp_vp[4];
static void gl_samples_step(const char *name) {
    if (samp_pending) {
        glEndQuery(GL_SAMPLES_PASSED); GLuint r = 0; glGetQueryObjectuiv(samp_q, GL_QUERY_RESULT, &r);
        GLenum e = glGetError();
        fprintf(stderr, "[samples] #%d %s fbo %d color0 %d vp %d,%d %dx%d prog %d -> %u%s\n", samp_idx, samp_name, samp_fb, samp_c0,
                samp_vp[0], samp_vp[1], samp_vp[2], samp_vp[3], samp_prog, r, e ? " (query error)" : "");
        // RT32_GL_PROBE="x,y" (GL coordinates in the target): after each sampled draw, read that texel of colour
        // attachment 0 and log it when it changed, i.e. which draw last wrote that pixel and with what colour
        static int px = -2, py; if (px == -2) { px = -1; const char *s = getenv("RT32_GL_PROBE"); if (s) sscanf(s, "%d,%d", &px, &py); }
        if (px >= 0 && samp_fb && r) {
            GLint rfb = 0, rb = 0; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb); glGetIntegerv(GL_READ_BUFFER, &rb);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)samp_fb); glReadBuffer(GL_COLOR_ATTACHMENT0);
            GLint pb = 0; glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pb); glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            float v[4] = {0}; glReadPixels(px, py, 1, 1, GL_RGBA, GL_FLOAT, v); glGetError();
            glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pb);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb); glReadBuffer((GLenum)rb);
            static float last[4]; static GLint lastfb;
            if (lastfb != samp_fb || memcmp(last, v, sizeof v)) {
                fprintf(stderr, "[probe] #%d prog %d target tex %d: (%g, %g, %g, %g)\n", samp_idx, samp_prog, samp_c0, v[0], v[1], v[2], v[3]);
                memcpy(last, v, sizeof v); lastfb = samp_fb; }
        }
        // RT32_DIAG_FILMSTRIP=1 (diagnostic only): on the first sampled frame, after each draw into the scene colour
        // target (R11F_G11F_B10F, >= 1000 px wide) that changed pixels and used a different program than the last
        // snapshot, save that target at 1/3 size as film_f<frame>_<draw>_p<prog>.ppm in RT32_SHOT_DIR, and log blend state.
        { static int film = -1; if (film < 0) film = getenv("RT32_DIAG_FILMSTRIP") != NULL;
          extern volatile int host_present_count; static int film_frame = -1; static GLint film_last_prog = -1;
          if (film > 0 && film_frame >= 0 && host_present_count > film_frame + 60) { film_frame = -1; film_last_prog = -1; }   // a later capture films again
          if (film > 0 && samp_fb && r && samp_c0 && (film_frame < 0 || film_frame == host_present_count)) {
              GLint rfb = 0, rb = 0; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb); glGetIntegerv(GL_READ_BUFFER, &rb);
              glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)samp_fb); glReadBuffer(GL_COLOR_ATTACHMENT0);
              GLint rs = 0, bs = 0, w = 0, h = 0;
              glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &rs);
              glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE, &bs);
              w = samp_vp[2]; h = samp_vp[3];
              if (rs == 11 && bs == 10 && w >= 1000 && samp_prog != film_last_prog) {
                  if (film_frame < 0) film_frame = host_present_count;
                  film_last_prog = samp_prog;
                  GLint pb = 0; glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pb); glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                  float *px = malloc((size_t)w * h * 12); glPixelStorei(GL_PACK_ALIGNMENT, 1);
                  glReadPixels(samp_vp[0], samp_vp[1], w, h, GL_RGB, GL_FLOAT, px); glGetError();
                  glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pb);
                  int ow = w / 3, oh = h / 3; char path[600]; const char *dir = getenv("RT32_SHOT_DIR") ? getenv("RT32_SHOT_DIR") : ".";
                  snprintf(path, sizeof path, "%s/film_f%d_%05d_p%d.ppm", dir, film_frame, samp_idx, samp_prog);
                  FILE *f = fopen(path, "wb");
                  if (f) { fprintf(f, "P6\n%d %d\n255\n", ow, oh);
                      for (int y = oh - 1; y >= 0; y--) for (int x = 0; x < ow; x++) {
                          const float *q = px + ((size_t)(y * 3) * w + x * 3) * 3;
                          for (int ch = 0; ch < 3; ch++) { float v = q[ch]; v = v / (1.0f + v); v = v < 0 ? 0 : v; v = sqrtf(v); fputc((int)(v * 255.0f + 0.5f), f); } }
                      fclose(f); }
                  free(px);
                  GLint be = glIsEnabled(GL_BLEND), sr = 0, dr = 0, sa = 0, da = 0, eq = 0, eqa = 0; GLboolean cm[4] = {0};
                  glGetIntegerv(GL_BLEND_SRC_RGB, &sr); glGetIntegerv(GL_BLEND_DST_RGB, &dr); glGetIntegerv(GL_BLEND_SRC_ALPHA, &sa); glGetIntegerv(GL_BLEND_DST_ALPHA, &da);
                  glGetIntegerv(GL_BLEND_EQUATION_RGB, &eq); glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eqa); glGetBooleanv(GL_COLOR_WRITEMASK, cm);
                  fprintf(stderr, "[film] f%d draw #%d prog %d (%u px) blend %d src %#x dst %#x srcA %#x dstA %#x eq %#x/%#x mask %d%d%d%d\n",
                          film_frame, samp_idx, samp_prog, r, be, sr, dr, sa, da, eq, eqa, cm[0], cm[1], cm[2], cm[3]);
              }
              glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb); glReadBuffer((GLenum)rb);
          } }
        samp_pending = 0;
    }
    if (!gl_samples_active) return;
    if (name[2] == 'D' && name[3] == 'r' && (name[6] == 'E' || name[6] == 'A')) {
        if (!samp_q) glGenQueries(1, &samp_q);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &samp_fb); glGetIntegerv(GL_CURRENT_PROGRAM, &samp_prog);
        samp_c0 = 0; glGetIntegerv(GL_VIEWPORT, samp_vp);
        if (samp_fb) glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &samp_c0);
        if (samp_fb) {   // remember each colour target drawn this frame, in first-use order (dumped by the host at present)
            GLint t0 = 0; glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &t0);
            if (t0 == GL_TEXTURE && samp_c0) { int seen = 0; for (int k = 0; k < gl_samp_ntex; k++) if (gl_samp_tex[k] == (uint32_t)samp_c0) seen = 1;
                if (!seen && gl_samp_ntex < 64) { gl_samp_prog[gl_samp_ntex] = (uint32_t)samp_prog; gl_samp_tex[gl_samp_ntex++] = (uint32_t)samp_c0; } }
        }
        glGetError(); glBeginQuery(GL_SAMPLES_PASSED, samp_q);
        if (glGetError()) return;
        samp_pending = 1; samp_idx++; samp_name = name;
        // RT32_GL_UBODUMP=<program>: at that program's draws, print its uniform blocks' current contents
        static int ubp = -2; if (ubp == -2) ubp = getenv("RT32_GL_UBODUMP") ? atoi(getenv("RT32_GL_UBODUMP")) : -1;
        if (samp_prog == ubp) {
            glEndQuery(GL_SAMPLES_PASSED);
            GLint nb = 0; glGetProgramiv((GLuint)samp_prog, GL_ACTIVE_UNIFORM_BLOCKS, &nb);
            for (GLint b = 0; b < nb; b++) {
                char bn[64] = ""; glGetActiveUniformBlockName((GLuint)samp_prog, b, sizeof bn, NULL, bn);
                GLint bind = 0, bsize = 0, buf = 0, start = 0;
                glGetActiveUniformBlockiv((GLuint)samp_prog, b, GL_UNIFORM_BLOCK_BINDING, &bind);
                glGetActiveUniformBlockiv((GLuint)samp_prog, b, GL_UNIFORM_BLOCK_DATA_SIZE, &bsize);
                glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, bind, &buf); glGetIntegeri_v(GL_UNIFORM_BUFFER_START, bind, &start);
                fprintf(stderr, "[ubo] draw #%d prog %d block %d '%s' binding %d size %d -> buffer %d start %d\n", samp_idx, samp_prog, b, bn, bind, bsize, buf, start);
                if (buf && bsize > 0 && bsize <= 4096) {
                    GLint old = 0; glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &old);
                    float v[1024]; glBindBuffer(GL_UNIFORM_BUFFER, buf); glGetBufferSubData(GL_UNIFORM_BUFFER, start, bsize, v);
                    glBindBuffer(GL_UNIFORM_BUFFER, old);
                    for (int k = 0; k < bsize / 16; k++) fprintf(stderr, "[ubo]   [%d] %g %g %g %g\n", k, v[4*k], v[4*k+1], v[4*k+2], v[4*k+3]);
                }
            }
            GLint ab = 0; glGetIntegerv(GL_ACTIVE_TEXTURE, &ab);
            for (int u = 0; u < 8; u++) {
                GLint t = 0, w = 0, h = 0, s = 0; glActiveTexture(GL_TEXTURE0 + u); glGetIntegerv(GL_TEXTURE_BINDING_2D, &t);
                glGetIntegerv(GL_SAMPLER_BINDING, &s);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w); glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
                GLint bl = 0, ml = 0; glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &bl); glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &ml);
                GLint sw[4] = {0}; glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, sw);
                fprintf(stderr, "[ubo] unit %d: tex %d (%dx%d) sampler %d levels %d..%d swizzle %x %x %x %x\n", u, t, w, h, s, bl, ml, sw[0], sw[1], sw[2], sw[3]);
                if (t && w > 0 && h > 0 && (size_t)w * h <= 4096 * 4096) {   // pixel stats of level 0
                    GLint fmt = 0; glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
                    GLint pb = 0; glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pb); glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                    float *px = malloc((size_t)w * h * 16); glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, px);
                    GLenum e = glGetError();
                    double mn[4] = {1e30,1e30,1e30,1e30}, mx[4] = {-1e30,-1e30,-1e30,-1e30}, sm[4] = {0};
                    for (size_t k = 0; k < (size_t)w * h; k++) for (int ch = 0; ch < 4; ch++) {
                        double v = px[4*k+ch]; if (v < mn[ch]) mn[ch] = v; if (v > mx[ch]) mx[ch] = v; sm[ch] += v; }
                    fprintf(stderr, "[ubo]   fmt 0x%x err 0x%x  R %g..%g avg %g | G %g..%g avg %g | B %g..%g avg %g | A %g..%g avg %g\n", fmt, e,
                        mn[0], mx[0], sm[0]/(w*h), mn[1], mx[1], sm[1]/(w*h), mn[2], mx[2], sm[2]/(w*h), mn[3], mx[3], sm[3]/(w*h));
                    free(px); glBindBuffer(GL_PIXEL_PACK_BUFFER, pb);
                }
            }
            glActiveTexture((GLenum)ab);
            for (int u = 0; u < 8; u++) { char nm[32]; snprintf(nm, sizeof nm, "samplerPS%d", u);
                GLint loc = glGetUniformLocation((GLuint)samp_prog, nm); if (loc < 0) continue;
                GLint val = -1; glGetUniformiv((GLuint)samp_prog, loc, &val); fprintf(stderr, "[ubo] %s -> unit %d\n", nm, val); }
            glGetError(); glBeginQuery(GL_SAMPLES_PASSED, samp_q);
        }
    }
}

// Profiler support (RT32_PROF): time spent inside host GL calls made by the generated wrappers, and which guest
// CPU is currently inside one (read by the sampler in sys.c)
#include <mach/mach_time.h>
volatile CPU *gl_in_cpu; volatile uint64_t gl_ticks_total; static uint64_t gl_t0;
// Claude Fable - 2026-11-24: once the game renders on its own thread, AppKit still updates the context's drawable from the main
// thread (window activation, resize, screen change). Seen as a crash in gldUpdateDrawFramebuffer during glBlitFramebuffer
// at startup (2 of ~6 runs). Each GL call now holds the context's CGL lock, and the main-thread update takes the same
// lock (host.m), so the two never overlap. The lock is never held across guest code, so it cannot deadlock.
#include <OpenGL/OpenGL.h>
extern void *host_cgl_context(void); extern int host_gl_is_offthread(void);
static _Thread_local int gl_lock_depth;
void gl_ctx_lock(void) { static CGLContextObj lk; if (!lk) { if (!host_gl_is_offthread()) return; lk = (CGLContextObj)host_cgl_context(); if (!lk) return; } CGLLockContext(lk); gl_lock_depth++; }
void gl_ctx_unlock(void) { if (gl_lock_depth > 0) { gl_lock_depth--; CGLUnlockContext((CGLContextObj)host_cgl_context()); } }
void gl_enter(CPU *c) { gl_ctx_lock(); gl_in_cpu = c; gl_t0 = mach_absolute_time(); }
void gl_leave(void) { uint64_t d = mach_absolute_time() - gl_t0; gl_ticks_total += d; if (gl_hist_cur >= 0) gl_hist_t[gl_hist_cur] += d; gl_in_cpu = NULL; gl_ctx_unlock(); }
// RT32_GL_LEAN=1 (performance A/B): skip all per-call debug work (samples, checks, histogram, trace); keep only the
// per-frame call/draw counters used by the [perf] line
static int gl_lean = -1;
static int clamp_r11 = -1, clamp_fb = -1, clamp_flag; static unsigned clamp_gen, clamp_gen_seen = ~0u;
static GLint clamp_loc[65536]; static signed char clamp_set[65536];   // per program: uniform location (+1), last value set
static void gl_clamp_r11_step(const char *name) {
    if (name[2] == 'F' && name[3] == 'r' && name[4] == 'a' && name[5] == 'm' && name[11] != 'B') { clamp_gen++; return; } // glFramebufferTexture*/Renderbuffer
    if (name[2] == 'B' && name[6] == 'F' && name[7] == 'r') { clamp_gen++; return; }                                       // glBindFramebuffer
    if (!(name[2] == 'D' && name[3] == 'r' && (name[6] == 'E' || name[6] == 'A'))) return;                                // only draws
    if (clamp_gen != clamp_gen_seen) {
        clamp_gen_seen = clamp_gen; GLint fb = 0; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb); clamp_fb = fb; clamp_flag = 0;
        if (fb) { GLint t = 0; glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &t);
            if (t) { GLint r = 0, b = 0;
                glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &r);
                glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE, &b);
                clamp_flag = (r == 11 && b == 10); } }
        glGetError();
    }
    GLint prog = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prog); if (prog <= 0 || prog >= 65536) return;
    if (!clamp_loc[prog]) clamp_loc[prog] = glGetUniformLocation((GLuint)prog, "rt_clampneg") + 1 + (1 << 30) * 0;
    if (clamp_loc[prog] <= 0) { clamp_loc[prog] = -1; return; }
    int want = clamp_flag ? 1 : 0;
    if (clamp_set[prog] != want + 1) { glUniform1f(clamp_loc[prog] - 1, (float)want); clamp_set[prog] = (signed char)(want + 1); }
}
volatile uint32_t gl_tex_comp, gl_tex_plain, gl_tex_gen, gl_tex_del;
void gl_trace(const char *name) {
    if (__builtin_expect(clamp_r11, 1)) { if (clamp_r11 < 0) clamp_r11 = getenv("RT32_CLAMP_R11") != NULL; if (clamp_r11 > 0) gl_clamp_r11_step(name); }
    if (__builtin_expect(gl_lean, 0)) {
        if (gl_lean < 0) gl_lean = getenv("RT32_GL_LEAN") != NULL;
        if (gl_lean > 0) {
            gl_calls_frame++;
            if (name[2] == 'D' && name[3] == 'r' && (name[6] == 'E' || name[6] == 'A')) gl_draws_frame++;
            return;
        }
    }
    gl_samples_step(name);
    gl_check_step(name);
    gl_calls_frame++;
    // Claude Fable - 2026-11-24: texture traffic counters for the [perf] line (is the game streaming, thrashing, or idle?)
    // Read them like this while the camera is still: all near zero = settled. "created" about equal to "deleted" with
    // steady disk reads = thrashing (the texture pool is too small, so textures are thrown out and reloaded).
    // This is how the 4K blurry-texture problem was confirmed. The call is recognised by letters at fixed positions
    // in its name because this runs on every GL call and a strcmp would cost more.
    { extern volatile uint32_t gl_tex_comp, gl_tex_plain, gl_tex_gen, gl_tex_del;
      if (name[2] == 'C' && name[3] == 'o' && name[4] == 'm' && name[12] == 'T') gl_tex_comp++;                 // glCompressedTex(Sub)Image*
      else if (name[2] == 'T' && name[3] == 'e' && name[4] == 'x' && (name[5] == 'I' || name[5] == 'S') && name[6] != 'o') gl_tex_plain++;   // glTex(Sub)Image*
      else if (name[2] == 'G' && name[3] == 'e' && name[4] == 'n' && name[5] == 'T') gl_tex_gen++;             // glGenTextures
      else if (name[2] == 'D' && name[3] == 'e' && name[8] == 'T' && name[9] == 'e') gl_tex_del++; }           // glDeleteTextures
    { unsigned i = (unsigned)(((uintptr_t)name >> 3) * 2654435761u) & 511;
      while (gl_hist_name[i] && gl_hist_name[i] != name) i = (i + 1) & 511;
      gl_hist_name[i] = name; gl_hist_n[i]++; gl_hist_cur = (int)i; }
    if (name[2] == 'D' && name[3] == 'r' && (name[6] == 'E' || name[6] == 'A')) gl_draws_frame++;   // glDraw*
    if (gl_tracing < 0) gl_tracing = getenv("RT32_TRACE_GL") != NULL;
    if (gl_tracing) {
        GLenum e = glGetError();
        fprintf(stderr, "[gl] %s%s\n", name, e ? " (previous call raised an error)" : "");
    }
}

// RT32_TRACE_GLARGS=glTexImage2D,glTexParameteri,...: log selected calls with raw argument words
void gl_trace_args(const char *name, CPU *c, int nslots) {
    if (gl_lean > 0) return;
    static const char *sel = (const char *)-1;
    if (sel == (const char *)-1) sel = getenv("RT32_TRACE_GLARGS");
    if (!sel || !strstr(sel, name)) return;
    static int budget = 4000; if (budget-- <= 0) return;
    if (!strcmp(name, "glGetUniformLocation") && ARG(1)) {   // show the queried name and the location the host returns
        const char *un = (const char *)G2H(ARG(1));
        fprintf(stderr, "[glargs] glGetUniformLocation(prog %u, \"%.60s\") -> %d\n", ARG(0), un, glGetUniformLocation(ARG(0), un));
        return;
    }
    fprintf(stderr, "[glargs] %s(", name);
    for (int i = 0; i < nslots; i++) fprintf(stderr, "%s%#x", i ? ", " : "", ARG(i));
    fprintf(stderr, ")\n");
}

// gl*Pointer: with a buffer object bound the "pointer" is a byte offset into it
void *gl_ptr_array(uint32_t g) {
    GLint b = 0; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &b);
    return b ? (void *)(uintptr_t)g : (g ? G2H(g) : NULL);
}
void *gl_ptr_element(uint32_t g) {
    GLint b = 0; glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &b);
    return b ? (void *)(uintptr_t)g : (g ? G2H(g) : NULL);
}

// glGetString: host strings live outside guest memory; copy each into a guest buffer once
void I__glGetString(CPU *c) {
    static uint32_t cache[16]; static GLenum keys[16];
    GLenum e = ARG(0);
    for (int i = 0; i < 16; i++) if (keys[i] == e && cache[i]) { RET(cache[i]); return; }
    const char *s = (const char *)glGetString(e);
    static char *joined;
    if (!s && e == GL_EXTENSIONS) {
        if (!joined) { GLint n = 0; glGetIntegerv(GL_NUM_EXTENSIONS, &n); size_t len = 1; joined = calloc(1, 1);
            for (GLint k = 0; k < n; k++) { const char *x = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)k); size_t l = strlen(x);
                joined = realloc(joined, len + l + 1); memcpy(joined + len - 1, x, l); joined[len - 1 + l] = ' '; joined[len + l] = 0; len += l + 1; }
            // features that are core in the host's context but no longer listed as extensions, which the game tests for by name
            static const char *core_ext[] = { "GL_ARB_texture_swizzle", "GL_EXT_texture_swizzle", 0 };
            for (int k = 0; core_ext[k]; k++) if (!strstr(joined, core_ext[k])) {
                size_t l = strlen(core_ext[k]); joined = realloc(joined, len + l + 1); memcpy(joined + len - 1, core_ext[k], l);
                joined[len - 1 + l] = ' '; joined[len + l] = 0; len += l + 1;
                fprintf(stderr, "[gl] extension string: added %s (core in this context)\n", core_ext[k]); }
        }
        s = joined;
    }
    if (!s) { RET(0); return; }
    for (int i = 0; i < 16; i++) if (!keys[i]) { keys[i] = e; cache[i] = gstrdup(s); RET(cache[i]); return; }
    RET(gstrdup(s));
}

// glShaderSource(shader, count, const char **strings, const GLint *lengths): array of guest pointers
void I__glShaderSource(CPU *c) {
    GLsizei n = (GLsizei)ARG(1);
    const char **strs = calloc(n ? n : 1, sizeof *strs);
    for (GLsizei i = 0; i < n; i++) strs[i] = (const char *)G2H(rd32(ARG(2) + 4 * i));
    // RT32_DIAG_NOFADE=1 (diagnostic only): drop the colour-scale/overlay step of the colour-grading
    // LUT shader, to see what the game draws underneath a full camera fade
    char *patched = NULL;
    int diag_hit = 0;
    if (getenv("RT32_DIAG_NOFADE")) {
        const GLint *L = ARG(3) ? (const GLint *)G2H(ARG(3)) : NULL;
        size_t tot = 0; for (GLsizei i = 0; i < n; i++) tot += (L && L[i] >= 0) ? (size_t)L[i] : strlen(strs[i]);
        char *all = malloc(tot + 1); size_t o = 0;
        for (GLsizei i = 0; i < n; i++) { size_t l = (L && L[i] >= 0) ? (size_t)L[i] : strlen(strs[i]); memcpy(all + o, strs[i], l); o += l; }
        all[o] = 0;
        const char *pat = "clamp(r0.xyz * cb0[4].xyz + cb0[5].xyz, 0.0, 1.0)";
        char *hit = strstr(all, pat);
        if (hit && strstr(all, "FLUTBlenderPixelShader")) {
            const char *rep = "clamp(r0.xyz, 0.0, 1.0)";
            memcpy(hit, rep, strlen(rep)); memset(hit + strlen(rep), ' ', strlen(pat) - strlen(rep));
            patched = all; diag_hit = 1; fprintf(stderr, "[gl] DIAG: shader %u colour-scale step removed\n", ARG(0));
        } else free(all);
    }
    // RT32_DIAG_CLAMPREAD="name" (diagnostic only): in shaders whose source contains `name`, every 2D sampler fetch
    // is clamped to >= 0, to test whether negative values in an unsigned-float render target cause colour artefacts
    if (!diag_hit && getenv("RT32_DIAG_CLAMPREAD")) {
        const GLint *L = ARG(3) ? (const GLint *)G2H(ARG(3)) : NULL;
        size_t tot = 0; for (GLsizei i = 0; i < n; i++) tot += (L && L[i] >= 0) ? (size_t)L[i] : strlen(strs[i]);
        char *all = malloc(tot + 1); size_t o = 0;
        for (GLsizei i = 0; i < n; i++) { size_t l = (L && L[i] >= 0) ? (size_t)L[i] : strlen(strs[i]); memcpy(all + o, strs[i], l); o += l; }
        all[o] = 0;
        char *mainp = strstr(all, "void main()");
        if (strstr(all, getenv("RT32_DIAG_CLAMPREAD")) && mainp) {
            const char *fn = "vec4 rt_clampread(sampler2D s, vec2 uv) { return max(texture(s, uv), vec4(0.0)); }\n";
            size_t cnt = 0; for (char *q = all; (q = strstr(q, "texture(samplerPS")); q += 17) cnt++;
            char *out = malloc(o + strlen(fn) + cnt * 8 + 1), *w = out;
            memcpy(w, all, (size_t)(mainp - all)); w += mainp - all; strcpy(w, fn); w += strlen(fn);
            for (char *q = mainp; *q; ) {
                if (!strncmp(q, "texture(samplerPS", 17)) { memcpy(w, "rt_clampread(samplerPS", 22); w += 22; q += 17; }
                else *w++ = *q++;
            }
            *w = 0; free(all); patched = out; diag_hit = 1;
            fprintf(stderr, "[gl] DIAG: shader %u: %zu texture fetches clamped to >= 0\n", ARG(0), cnt);
        } else free(all);
    }
    // RT32_DIAG_SUBST=file (diagnostic only): each line "marker<TAB>find<TAB>replace"; in shaders whose source
    // contains marker, the first occurrence of find is replaced with replace (several lines may share a marker)
    if (getenv("RT32_DIAG_SUBST")) {
        FILE *sf = fopen(getenv("RT32_DIAG_SUBST"), "r");
        if (sf) {
            char *all = patched;
            if (!all) {
                const GLint *L = ARG(3) ? (const GLint *)G2H(ARG(3)) : NULL;
                size_t tot = 0; for (GLsizei i = 0; i < n; i++) tot += (L && L[i] >= 0) ? (size_t)L[i] : strlen(strs[i]);
                all = malloc(tot + 1); size_t o = 0;
                for (GLsizei i = 0; i < n; i++) { size_t l = (L && L[i] >= 0) ? (size_t)L[i] : strlen(strs[i]); memcpy(all + o, strs[i], l); o += l; }
                all[o] = 0;
            }
            char line[4096]; int hits = 0;
            while (fgets(line, sizeof line, sf)) {
                line[strcspn(line, "\r\n")] = 0;
                char *t1 = strchr(line, '\t'); if (!t1) continue; *t1++ = 0;
                char *t2 = strchr(t1, '\t'); if (!t2) continue; *t2++ = 0;
                if (!strstr(all, line)) continue;
                char *h = strstr(all, t1); if (!h) continue;
                size_t fl = strlen(t1), rl = strlen(t2), al = strlen(all);
                char *nw = malloc(al - fl + rl + 1);
                memcpy(nw, all, (size_t)(h - all)); memcpy(nw + (h - all), t2, rl);
                strcpy(nw + (h - all) + rl, h + fl);
                free(all); all = nw; hits++;
            }
            fclose(sf);
            if (hits) { patched = all; diag_hit = 1; fprintf(stderr, "[gl] DIAG: shader %u: %d substitutions\n", ARG(0), hits); }
            else if (all != patched) free(all);
        }
    }
    // Vertex shaders get `invariant gl_Position;`: the engine draws a mesh in several passes with different vertex shaders
    // and depth-tests later passes against earlier depth (EQUAL/LEQUAL). D3D gives bit-identical positions for identical
    // maths; Apple's GL optimises each shader separately, so some pixels differ in the last bit and fail the test
    // (hatched holes on skinned meshes). RT32_NO_INVARIANT=1 turns this off for A/B tests.
    if (!getenv("RT32_NO_INVARIANT")) {
        GLint st = 0; glGetShaderiv(ARG(0), GL_SHADER_TYPE, &st);
        if (st == GL_VERTEX_SHADER) {
            char *all = patched;
            if (!all) {
                const GLint *L = ARG(3) ? (const GLint *)G2H(ARG(3)) : NULL;
                size_t tot = 0; for (GLsizei i = 0; i < n; i++) tot += (L && L[i] >= 0) ? (size_t)L[i] : strlen(strs[i]);
                all = malloc(tot + 1); size_t o = 0;
                for (GLsizei i = 0; i < n; i++) { size_t l = (L && L[i] >= 0) ? (size_t)L[i] : strlen(strs[i]); memcpy(all + o, strs[i], l); o += l; }
                all[o] = 0;
            }
            if (!strstr(all, "invariant gl_Position")) {
                // insert after the last preprocessor directive line that must stay first (#version / #extension)
                char *ins = all, *q = all;
                while ((q = strstr(q, "#version")) || (q = NULL)) { char *e = strchr(q, '\n'); if (!e) break; ins = e + 1; q = e; break; }
                for (char *x = all; (x = strstr(x, "#extension")); ) { char *e = strchr(x, '\n'); if (!e) break; if (e + 1 > ins) ins = e + 1; x = e; }
                const char *line = "invariant gl_Position;\n";
                size_t al = strlen(all), pre = (size_t)(ins - all);
                char *out = malloc(al + strlen(line) + 1);
                memcpy(out, all, pre); strcpy(out + pre, line); strcat(out, all + pre);
                free(all); patched = out; diag_hit = 1;
            } else if (all != patched) free(all);
        }
    }
    // RT32_DIAG_TINT="marker:r,g,b;marker:r,g,b" (diagnostic only): pixel shaders whose source contains a marker
    // output that solid colour (main renamed to rt_main and called first, so discards and depth still behave)
    if (getenv("RT32_DIAG_TINT")) {
        GLint st = 0; glGetShaderiv(ARG(0), GL_SHADER_TYPE, &st);
        if (st == GL_FRAGMENT_SHADER) {
            char *all = patched;
            if (!all) {
                const GLint *L = ARG(3) ? (const GLint *)G2H(ARG(3)) : NULL;
                size_t tot = 0; for (GLsizei i = 0; i < n; i++) tot += (L && L[i] >= 0) ? (size_t)L[i] : strlen(strs[i]);
                all = malloc(tot + 1); size_t o = 0;
                for (GLsizei i = 0; i < n; i++) { size_t l = (L && L[i] >= 0) ? (size_t)L[i] : strlen(strs[i]); memcpy(all + o, strs[i], l); o += l; }
                all[o] = 0;
            }
            char spec[1024]; snprintf(spec, sizeof spec, "%s", getenv("RT32_DIAG_TINT"));
            const char *nl = strchr(all, '\n'); size_t hl = nl ? (size_t)(nl - all) : strlen(all);   // match on the first line (shader name)
            char head[512]; snprintf(head, sizeof head, "%.*s", (int)(hl < 511 ? hl : 511), all);
            float rgb[3]; int hit = 0;
            for (char *tok = strtok(spec, ";"); tok && !hit; tok = strtok(NULL, ";")) {
                char *colon = strchr(tok, ':'); if (!colon) continue; *colon = 0;
                if (strstr(head, tok) && sscanf(colon + 1, "%f,%f,%f", &rgb[0], &rgb[1], &rgb[2]) == 3) hit = 1;
            }
            char *mp = strstr(all, "void main()");
            if (hit && mp && strstr(all, "o0")) {
                char tail[256]; snprintf(tail, sizeof tail, "\nvoid main() { rt_main(); o0 = vec4(%f, %f, %f, 1.0); }\n", rgb[0], rgb[1], rgb[2]);
                size_t al = strlen(all); char *out = malloc(al + 16 + strlen(tail)); size_t pre = (size_t)(mp - all);
                memcpy(out, all, pre); strcpy(out + pre, "void rt_main()"); strcat(out, mp + strlen("void main()")); strcat(out, tail);
                free(all); patched = out; diag_hit = 1;
                fprintf(stderr, "[gl] DIAG tint: shader %u (%.80s) -> %g,%g,%g\n", ARG(0), head, rgb[0], rgb[1], rgb[2]);
            } else if (all != patched) free(all);
        }
    }
    // RT32_CLAMP_R11=1: on Apple GPUs an R11F_G11F_B10F colour target keeps negative values that PC hardware clamps
    // to 0 on write (and they later spread through bloom as dark blotches). Every pixel shader writing o0 gets an
    // optional clamp of o0, switched on per draw by gl_clamp_r11_step() only when colour target 0 is 11/11/10 bits.
    if (getenv("RT32_CLAMP_R11")) {
        GLint st = 0; glGetShaderiv(ARG(0), GL_SHADER_TYPE, &st);
        if (st == GL_FRAGMENT_SHADER) {
            char *all = patched;
            if (!all) {
                const GLint *L = ARG(3) ? (const GLint *)G2H(ARG(3)) : NULL;
                size_t tot = 0; for (GLsizei i = 0; i < n; i++) tot += (L && L[i] >= 0) ? (size_t)L[i] : strlen(strs[i]);
                all = malloc(tot + 1); size_t o = 0;
                for (GLsizei i = 0; i < n; i++) { size_t l = (L && L[i] >= 0) ? (size_t)L[i] : strlen(strs[i]); memcpy(all + o, strs[i], l); o += l; }
                all[o] = 0;
            }
            char *mp = strstr(all, "void main()");
            if (mp && strstr(all, "o0")) {
                const char *tail = "\nuniform float rt_clampneg;\nvoid main() { rt_main(); if (rt_clampneg > 0.5) o0 = max(o0, vec4(0.0)); }\n";
                size_t al = strlen(all); char *out = malloc(al + 16 + strlen(tail));
                size_t pre = (size_t)(mp - all);
                memcpy(out, all, pre); strcpy(out + pre, "void rt_main()"); strcat(out, mp + strlen("void main()")); strcat(out, tail);
                if (all != patched) free(all); else free(patched);
                patched = out; diag_hit = 1;
            } else if (all != patched) free(all);
        }
    }
    if (diag_hit) glShaderSource(ARG(0), 1, (const char **)&patched, NULL); else
    glShaderSource(ARG(0), n, strs, ARG(3) ? (const GLint *)G2H(ARG(3)) : NULL);
    { const char *dir = getenv("RT32_DUMP_SHADERS");          // debug: save each shader's source
      if (dir) { char path[512]; snprintf(path, sizeof path, "%s/shader_%u.glsl", dir, ARG(0)); FILE *f = fopen(path, "w");
        if (f) { for (GLsizei i = 0; i < n; i++) { const GLint *L = ARG(3) ? (const GLint *)G2H(ARG(3)) : NULL;
                   if (L && L[i] >= 0) fwrite(strs[i], 1, L[i], f); else fputs(strs[i], f); } fclose(f); } } }
    free(strs); free(patched);
}

// glBufferData / glBufferSubData (hand-written): record each buffer's size/usage. RT32_UBO_ORPHAN=1: a SubData that
// rewrites a whole buffer from offset 0 becomes glBufferData (fresh storage, "orphaning"), so the driver neither waits
// for the GPU nor copies the old contents. The engine updates a 2.5 KB uniform buffer ~2000x per frame this way.
static GLenum binding_query(GLenum target);
typedef struct { GLuint buf; GLint size; GLenum usage; } BufInfo;
static BufInfo bufinfo[8192];
static BufInfo *bufinfo_get(GLuint buf, int create) {
    unsigned h = (buf * 2654435761u) & 8191, i = h;
    for (;;) { BufInfo *b = &bufinfo[i]; if (b->buf == buf) return b;
        if (!b->buf) { if (!create) return NULL; b->buf = buf; return b; }
        i = (i + 1) & 8191; if (i == h) return NULL; }
}
static void alias_forget(GLuint buf);
void I__glBufferData(CPU *c) { gl_trace("glBufferData"); gl_trace_args("glBufferData", c, 4); gl_enter(c);
    GLenum target = ARG(0); GLint buf = 0; glGetIntegerv(binding_query(target), &buf);
    if (buf) { BufInfo *b = bufinfo_get((GLuint)buf, 1); if (b) { b->size = (GLint)ARG(1); b->usage = ARG(3); } alias_forget((GLuint)buf); }
    glBufferData(target, (GLsizeiptr)ARG(1), ARG(2) ? G2H(ARG(2)) : NULL, ARG(3));
    gl_leave(); }
void I__glBufferDataARB(CPU *c) { I__glBufferData(c); }
void I__glBufferSubData(CPU *c) { gl_trace("glBufferSubData"); gl_trace_args("glBufferSubData", c, 4); gl_enter(c);
    static int orphan = -1; if (orphan < 0) orphan = getenv("RT32_NO_UBO_ORPHAN") == NULL;
    GLenum target = ARG(0); GLintptr off = (GLintptr)ARG(1); GLsizeiptr len = (GLsizeiptr)ARG(2); const void *data = ARG(3) ? G2H(ARG(3)) : NULL;
    if (orphan > 0 && off == 0 && data) {
        GLint buf = 0; glGetIntegerv(binding_query(target), &buf);
        BufInfo *b = buf ? bufinfo_get((GLuint)buf, 0) : NULL;
        if (b && b->size == (GLint)len) { alias_forget((GLuint)buf); glBufferData(target, len, data, b->usage ? b->usage : GL_STREAM_DRAW); gl_leave(); return; }
    }
    glBufferSubData(target, off, len, data);
    gl_leave(); }
void I__glBufferSubDataARB(CPU *c) { I__glBufferSubData(c); }

// glMapBuffer: hand the guest a shadow copy in guest memory; write it back on unmap
typedef struct { GLenum target; GLuint buf; uint32_t shadow; GLint size; GLenum access; GLint off; int ranged, flushed; GLint dsize; int alias; } Mapping;
static Mapping maps[64];
volatile uint64_t gl_map_count, gl_readback_bytes, gl_unmap_bytes, gl_flush_bytes;   // printed with [perf]
static int gl_mapfix = -1;   // RT32_GL_MAPFIX=1: skip the whole-buffer upload at unmap after explicit (APPLE) flushes
static GLenum binding_query(GLenum target) {
    switch (target) {
    case GL_ARRAY_BUFFER: return GL_ARRAY_BUFFER_BINDING;
    case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
    case GL_PIXEL_PACK_BUFFER: return GL_PIXEL_PACK_BUFFER_BINDING;
    case GL_PIXEL_UNPACK_BUFFER: return GL_PIXEL_UNPACK_BUFFER_BINDING;
    case GL_UNIFORM_BUFFER: return GL_UNIFORM_BUFFER_BINDING;
    case GL_COPY_READ_BUFFER: return GL_COPY_READ_BUFFER;
    case GL_COPY_WRITE_BUFFER: return GL_COPY_WRITE_BUFFER;
    case GL_TEXTURE_BUFFER: return GL_TEXTURE_BUFFER;
    default: return 0;
    }
}
// One persistent guest-memory shadow per buffer object. A mapping that doesn't invalidate must
// see the buffer's previous contents (engines map a whole ring buffer and write only at their
// cursor), so the shadow lives across maps instead of being a fresh allocation each time.
typedef struct { GLuint buf; uint32_t shadow; GLint size; uint32_t raw; } Shadow;
#define DIRTY_MIN (256 * 1024)   // buffers at least this big get page-aligned shadows and dirty-page tracking
static Shadow shadows[4096];
static uint32_t buffer_shadow(GLenum target, GLuint buf, GLint size, int need_contents) {
    unsigned h = (buf * 2654435761u) & 4095, i = h;
    for (;;) {
        Shadow *s = &shadows[i];
        if (s->buf == buf && s->shadow) {
            if (s->size == size) return s->shadow;
            gfree(s->raw ? s->raw : s->shadow); s->shadow = 0; s->raw = 0; break;   // buffer was re-specified with a new size
        }
        if (!s->buf) break;
        i = (i + 1) & 4095; if (i == h) { fprintf(stderr, "[gl] shadow table full\n"); abort(); }
    }
    Shadow *s = &shadows[i];
    s->buf = buf; s->size = size; s->raw = 0;
    if (size >= DIRTY_MIN) {   // own whole pages, so write-protecting them never touches other data
        uint32_t pg = (uint32_t)getpagesize(); s->raw = gmalloc((uint32_t)size + 2 * pg);
        s->shadow = (s->raw + pg - 1) & ~(pg - 1);
    } else s->shadow = gmalloc((uint32_t)size);
    memset(M + s->shadow, 0, (size_t)size);
    if (need_contents) glGetBufferSubData(target, 0, size, M + s->shadow);
    return s->shadow;
}
// Dirty-page tracking for whole-buffer maps (RT32_GL_MAPFIX=1): the engine maps a multi-MB ring buffer hundreds of
// times a frame and writes a few KB each time; uploading the whole shadow at every unmap cost ~2 GB/frame.
// At map the shadow is made read-only; the first write to each page faults, gl_dirty_fault() records the page and
// re-enables writing; unmap uploads only the recorded pages. Called from the runtime's SIGSEGV/SIGBUS handler.
#include <sys/mman.h>
#include <unistd.h>
typedef struct { uint8_t *lo, *hi; uint8_t *bits; uint32_t npages, cap; volatile int active; } Dirty;
static Dirty dirty[64]; static size_t dpg;
int gl_dirty_fault(uintptr_t fa) {
    uint8_t *p = (uint8_t *)fa;
    for (int i = 0; i < 64; i++) {
        Dirty *d = &dirty[i];
        if (!__atomic_load_n(&d->active, __ATOMIC_ACQUIRE) || p < d->lo || p >= d->hi) continue;
        size_t k = (size_t)(p - d->lo) / dpg; d->bits[k] = 1; { extern volatile uint32_t rt_hitch_faults; rt_hitch_faults++; }
        mprotect(d->lo + k * dpg, dpg, PROT_READ | PROT_WRITE);
        return 1;
    }
    return 0;
}
// Host (kernel) writes into a protected page fail with EFAULT instead of faulting, so file-read shims call this
// first: an atomic no-op write to each page of [g, g+n) that lies in a tracked buffer, which takes the normal
// fault path (page recorded dirty and unlocked) before the kernel writes there.
static volatile int dirty_active_n;
void rt_touch_write(uint32_t g, uint32_t n) {
    if (!__atomic_load_n(&dirty_active_n, __ATOMIC_ACQUIRE) || !n) return;
    uint8_t *lo = M + g, *hi = lo + n;
    for (int i = 0; i < 64; i++) {
        Dirty *d = &dirty[i];
        if (!__atomic_load_n(&d->active, __ATOMIC_ACQUIRE) || hi <= d->lo || lo >= d->hi) continue;
        uint8_t *a = lo > d->lo ? lo : d->lo, *b = hi < d->hi ? hi : d->hi;
        for (uint8_t *p = d->lo + ((size_t)(a - d->lo) / dpg) * dpg; p < b; p += dpg) {
            uint8_t *q = p < a ? a : p; __atomic_fetch_add(q, 0, __ATOMIC_RELAXED); }
    }
}
static void dirty_begin(int i, uint32_t shadow, GLint size) {
    Dirty *d = &dirty[i]; if (!dpg) dpg = (size_t)getpagesize();
    d->npages = (uint32_t)(((size_t)size + dpg - 1) / dpg);
    if (d->cap < d->npages) { free(d->bits); d->bits = malloc(d->npages); d->cap = d->npages; }
    memset(d->bits, 0, d->npages); d->lo = M + shadow; d->hi = d->lo + (size_t)d->npages * dpg;
    __atomic_store_n(&d->active, 1, __ATOMIC_RELEASE); __atomic_add_fetch(&dirty_active_n, 1, __ATOMIC_ACQ_REL);
    mprotect(d->lo, (size_t)d->npages * dpg, PROT_READ);
}
static void dirty_end(int i, GLenum target, GLint size, GLint rlo, GLint rhi) {   // upload written pages within [rlo, rhi), coalesced
    Dirty *d = &dirty[i];
    mprotect(d->lo, (size_t)d->npages * dpg, PROT_READ | PROT_WRITE);
    __atomic_store_n(&d->active, 0, __ATOMIC_RELEASE); __atomic_sub_fetch(&dirty_active_n, 1, __ATOMIC_ACQ_REL);
    for (uint32_t k = 0; k < d->npages; ) {
        if (!d->bits[k]) { k++; continue; }
        uint32_t e = k; while (e < d->npages && d->bits[e]) e++;
        size_t off = (size_t)k * dpg, len = (size_t)(e - k) * dpg; if (off + len > (size_t)size) len = (size_t)size - off;
        if (off < (size_t)rlo) { size_t cut = (size_t)rlo - off; if (cut >= len) { k = e; continue; } off += cut; len -= cut; }
        if (off + len > (size_t)rhi) { if (off >= (size_t)rhi) { k = e; continue; } len = (size_t)rhi - off; }
        glBufferSubData(target, (GLintptr)off, (GLsizeiptr)len, d->lo + off); gl_unmap_bytes += len;
        k = e;
    }
}
void I__glMapBuffer(CPU *c) { gl_trace("glMapBuffer");
    GLenum target = ARG(0), access = ARG(1);
    GLint buf = 0, size = 0;
    glGetIntegerv(binding_query(target), &buf);
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    if (!buf || size <= 0) { RET(0); return; }
    for (int i = 0; i < 64; i++) if (!maps[i].shadow) {
        Mapping *m = &maps[i];
        m->target = target; m->buf = (GLuint)buf; m->size = size; m->access = access; m->off = 0; m->ranged = 0; m->flushed = 0; m->alias = 0;
        m->shadow = buffer_shadow(target, (GLuint)buf, size, 1);
        gl_map_count++;
        { static int logged; if (logged < 12 && size >= DIRTY_MIN) { logged++; fprintf(stderr, "[gl] big glMapBuffer: buf %d size %d access %#x\n", buf, size, access); } }
        if (access != GL_WRITE_ONLY) { glGetBufferSubData(target, 0, size, M + m->shadow); gl_readback_bytes += (uint64_t)size; }
        if (gl_mapfix < 0) gl_mapfix = getenv("RT32_NO_MAPFIX") == NULL;
        if (gl_mapfix && access != GL_READ_ONLY && size >= DIRTY_MIN && (m->shadow & ((uint32_t)getpagesize() - 1)) == 0) {
            m->flushed = 2; dirty_begin(i, m->shadow, size); }   // flushed == 2: dirty-tracked mapping
        RET(m->shadow); return;
    }
    fprintf(stderr, "[gl] too many mapped buffers\n"); abort();
}
extern volatile uint64_t rt_hitch_unmap_ticks;
static void alias_unmap(Mapping *m);
static void glUnmapBuffer_impl(CPU *c);
void I__glUnmapBuffer(CPU *c) { uint64_t t0 = mach_absolute_time(); glUnmapBuffer_impl(c); rt_hitch_unmap_ticks += mach_absolute_time() - t0; }
static void glUnmapBuffer_impl(CPU *c) { gl_trace("glUnmapBuffer");
    GLenum target = ARG(0);
    GLint buf = 0; glGetIntegerv(binding_query(target), &buf);
    for (int i = 0; i < 64; i++) if (maps[i].shadow && maps[i].buf == (GLuint)buf && maps[i].target == target) {
        Mapping *m = &maps[i];
        if (m->alias) { alias_unmap(m); m->shadow = 0; m->alias = 0; RET(GL_TRUE); return; }
        if (gl_mapfix < 0) gl_mapfix = getenv("RT32_NO_MAPFIX") == NULL;
        if (m->ranged && m->flushed == 2) dirty_end(i, target, m->dsize, m->off, m->off + m->size);
        else if (m->ranged) {
            if ((m->access & GL_MAP_WRITE_BIT) && !m->flushed) { glBufferSubData(target, m->off, m->size, M + m->shadow + m->off); gl_unmap_bytes += (uint64_t)m->size; }
        } else if (m->flushed == 2) dirty_end(i, target, m->size, 0, m->size);
        else if (m->access != GL_READ_ONLY) { glBufferSubData(target, 0, m->size, M + m->shadow); gl_unmap_bytes += (uint64_t)m->size; }
        m->shadow = 0;                                        // the shadow itself persists with the buffer
        RET(GL_TRUE); return;
    }
    RET(GL_FALSE);
}

// CGL: the guest only ever sees "the current context" as handle 1
void I__CGLGetCurrentContext(CPU *c) { RET(CGLGetCurrentContext() ? 1 : 0); }
void I__CGLGetParameter(CPU *c) {
    GLint v[4] = {0};
    CGLError e = CGLGetParameter(CGLGetCurrentContext(), (CGLContextParameter)ARG(1), v);
    if (ARG(2)) wr32(ARG(2), (uint32_t)v[0]);
    RET(e);
}
void I__CGLSetParameter(CPU *c) {
    GLint v = ARG(2) ? (GLint)rd32(ARG(2)) : 0;
    RET(CGLSetParameter(CGLGetCurrentContext(), (CGLContextParameter)ARG(1), &v));
}

// Total video memory, for an engine's VRAM query (capped for 32-bit engines: they overflow above ~2 GiB)
uint32_t gl_total_vram(void) {
    GLint mem = 0;
    CGLRendererInfoObj info; GLint n = 0;
    if (CGLQueryRendererInfo(0xFFFFFFFF, &info, &n) == kCGLNoError) {
        for (GLint i = 0; i < n; i++) {
            GLint v = 0;
            if (CGLDescribeRenderer(info, i, kCGLRPVideoMemoryMegabytes, &v) == kCGLNoError && v > mem) mem = v;
        }
        CGLDestroyRendererInfo(info);
    }
    if (mem <= 0 || mem > 2047) mem = 2047;
    return (uint32_t)mem * 1024u * 1024u;
}

// Window-surface mapping: the engine renders at its own resolution; on the default framebuffer
// its viewports/scissors are mapped onto the real drawable with a uniform scale and centering
// offset (letterbox/pillarbox), recomputed by the host every frame.
float gl_win_scale = 1, gl_win_off_x = 0, gl_win_off_y = 0;
// Claude Fable - 2026-11-24: learn the engine's real output size from what it draws on the window framebuffer (largest
// viewport / blit destination anchored at 0,0 in a frame), published at each present. game32_window_size() was a
// fixed 1280x720, so any other resolution was scaled wrongly.
volatile int gl_engine_w, gl_engine_h; static int gl_fr_w, gl_fr_h;
// Remember the largest rectangle anchored at (0,0) drawn to the window this frame. Small or offset rectangles are
// ignored (they are partial updates, not the full picture).
static void gl_note_window_rect(GLint x0, GLint y0, GLint w, GLint h) {
    if (x0 != 0 || y0 != 0 || w < 320 || h < 200) return;
    if ((long)w * h > (long)gl_fr_w * gl_fr_h) { gl_fr_w = w; gl_fr_h = h; }
}
void gl_frame_publish(void) {   // called by host_present on the render thread
    if (gl_fr_w >= 320 && (gl_fr_w != gl_engine_w || gl_fr_h != gl_engine_h)) {
        fprintf(stderr, "[gl] engine output size seen on the window framebuffer: %dx%d (was %dx%d)\n", gl_fr_w, gl_fr_h, gl_engine_w, gl_engine_h);
        gl_engine_w = gl_fr_w; gl_engine_h = gl_fr_h; }
    gl_fr_w = gl_fr_h = 0;
}
static int on_window_fb(void) { GLint fb = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &fb); return fb == 0; }
#define MAPX(v) ((GLint)(gl_win_off_x + (v) * gl_win_scale))
#define MAPY(v) ((GLint)(gl_win_off_y + (v) * gl_win_scale))
#define MAPS(v) ((GLsizei)((v) * gl_win_scale + 0.5f))
void I__glViewport(CPU *c) { gl_trace("glViewport");
    GLint x = (GLint)ARG(0), y = (GLint)ARG(1); GLsizei w = (GLsizei)ARG(2), h = (GLsizei)ARG(3);
    if (on_window_fb()) { gl_note_window_rect(x, y, w, h); glViewport(MAPX(x), MAPY(y), MAPS(w), MAPS(h)); } else glViewport(x, y, w, h);
}
// Claude Fable - 2026-11-24: blits to (or from) the window framebuffer use engine coordinates too. They were passed through
// unmapped, so in fullscreen on a display larger than the engine resolution the picture sat in a corner at 1:1.
void I__glBlitFramebuffer(CPU *c) { gl_trace("glBlitFramebuffer"); gl_trace_args("glBlitFramebuffer", c, 10); gl_enter(c);
    GLint sx0 = (GLint)ARG(0), sy0 = (GLint)ARG(1), sx1 = (GLint)ARG(2), sy1 = (GLint)ARG(3);
    GLint dx0 = (GLint)ARG(4), dy0 = (GLint)ARG(5), dx1 = (GLint)ARG(6), dy1 = (GLint)ARG(7);
    GLbitfield mask = ARG(8); GLenum filter = ARG(9);
    GLint dfb = 0, rfb = 0; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb); glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    if (dfb == 0) { gl_note_window_rect(dx0 < dx1 ? dx0 : dx1, dy0 < dy1 ? dy0 : dy1, dx1 > dx0 ? dx1 - dx0 : dx0 - dx1, dy1 > dy0 ? dy1 - dy0 : dy0 - dy1);
                    dx0 = MAPX(dx0); dy0 = MAPY(dy0); dx1 = MAPX(dx1); dy1 = MAPY(dy1);
                    if (gl_win_scale != 1.0f && mask == GL_COLOR_BUFFER_BIT) filter = GL_LINEAR; }   // smooth when scaling the final image
    if (rfb == 0) { sx0 = MAPX(sx0); sy0 = MAPY(sy0); sx1 = MAPX(sx1); sy1 = MAPY(sy1); }
    {   // Claude Fable - 2026-11-24: the final image going to a larger window is upscaled with MetalFX (host.m) when possible.
        // All four conditions together mean "this is the game presenting its finished frame, and it needs enlarging":
        //   dfb == 0                 destination is the window
        //   rfb != 0                 source is one of the game's own off-screen buffers
        //   mask == COLOR            a colour copy (not depth/stencil)
        //   gl_win_scale > 1.01      the window is larger than the game's resolution
        // host_mfx_blit returns 1 if it showed the frame through Metal; then the plain GL copy below is skipped.
        // It returns 0 (unavailable, switched off with F9, or failed) and the plain linear stretch runs as before.
        extern int host_mfx_blit(int, int, int, int, int, int, int, int);
        if (dfb == 0 && rfb != 0 && mask == GL_COLOR_BUFFER_BIT && gl_win_scale > 1.01f &&
            host_mfx_blit(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1)) { gl_leave(); return; }
    }
    glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
    gl_leave(); }
void I__glBlitFramebufferEXT(CPU *c) { I__glBlitFramebuffer(c); }
void I__glScissor(CPU *c) { gl_trace("glScissor");
    GLint x = (GLint)ARG(0), y = (GLint)ARG(1); GLsizei w = (GLsizei)ARG(2), h = (GLsizei)ARG(3);
    if (on_window_fb()) glScissor(MAPX(x), MAPY(y), MAPS(w), MAPS(h)); else glScissor(x, y, w, h);
}

// glGetProgramivARB(target, pname, int *out)
// Apple's Metal-backed GL reports some NATIVE_* program counters as 0 even for programs it runs fine.
// The engine treats "instructions > 0 but native instructions == 0" as "not supported in hardware",
// deletes the program and drops to a lower-quality effect (e.g. blocky shadows). Report native
// counters as the plain counters when the driver says the program is under native limits.
void I__glGetProgramivARB(CPU *c) {
    GLenum target = ARG(0), pname = ARG(1); GLint v = 0;
    glGetProgramivARB(target, pname, &v);
    static const GLenum native_to_plain[][2] = {
        { GL_PROGRAM_NATIVE_INSTRUCTIONS_ARB, GL_PROGRAM_INSTRUCTIONS_ARB },
        { GL_PROGRAM_NATIVE_TEMPORARIES_ARB, GL_PROGRAM_TEMPORARIES_ARB },
        { GL_PROGRAM_NATIVE_PARAMETERS_ARB, GL_PROGRAM_PARAMETERS_ARB },
        { GL_PROGRAM_NATIVE_ATTRIBS_ARB, GL_PROGRAM_ATTRIBS_ARB },
        { GL_PROGRAM_NATIVE_ALU_INSTRUCTIONS_ARB, GL_PROGRAM_ALU_INSTRUCTIONS_ARB },
        { GL_PROGRAM_NATIVE_TEX_INSTRUCTIONS_ARB, GL_PROGRAM_TEX_INSTRUCTIONS_ARB },
        { GL_PROGRAM_NATIVE_TEX_INDIRECTIONS_ARB, GL_PROGRAM_TEX_INDIRECTIONS_ARB },
    };
    int fixed = 0;
    for (size_t i = 0; i < sizeof native_to_plain / sizeof native_to_plain[0]; i++)
        if (pname == native_to_plain[i][0] && v == 0) {
            GLint under = 0, plain = 0;
            glGetProgramivARB(target, GL_PROGRAM_UNDER_NATIVE_LIMITS_ARB, &under);
            glGetProgramivARB(target, native_to_plain[i][1], &plain);
            if (under && plain > 0) { v = plain; fixed = 1; }
        }
    if (getenv("RT32_TRACE_PROGIV")) fprintf(stderr, "[gl] glGetProgramivARB(%#x, %#x) = %d%s\n", target, pname, v, fixed ? " (native count substituted)" : "");
    if (ARG(2)) wr32(ARG(2), (uint32_t)v);
}

// ARB-suffixed aliases of the hand-written map/unmap shims
void I__glMapBufferARB(CPU *c) { I__glMapBuffer(c); }
void I__glUnmapBufferARB(CPU *c) { I__glUnmapBuffer(c); }



// ---------- RT32_ALIAS: alias the driver's own mapping into guest memory (no shadow, no faults, no upload) ----------
// 1 = remap when the host pointer changes or the map invalidates; 2 = remap every map (safety A/B); 3 = only on pointer change.
#include <mach/mach.h>
#include <mach/mach_vm.h>
typedef struct { GLuint buf; GLint size; uint32_t raw, region, rcap; uintptr_t host; size_t maplen; int bad; } Alias;
static Alias aliases[16];
static int gl_alias = -1;
static uint64_t alias_maps, alias_remaps, alias_checks;
static void alias_release(Alias *a) {   // give the guest region back its own anonymous pages before freeing it
    if (!a->region) return;
    mmap(M + a->region, a->rcap, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    gfree(a->raw); a->raw = a->region = a->rcap = 0; a->host = 0; a->maplen = 0;
}
static Alias *alias_for(GLuint buf, GLint size) {
    Alias *fr = NULL;
    for (int i = 0; i < 16; i++) {
        if (aliases[i].buf == buf) { if (aliases[i].size != size) { alias_release(&aliases[i]); aliases[i].size = size; aliases[i].bad = 0; } return &aliases[i]; }
        if (!aliases[i].buf && !fr) fr = &aliases[i];
    }
    if (fr) { memset(fr, 0, sizeof *fr); fr->buf = buf; fr->size = size; }
    return fr;
}
static uint64_t alias_forgets;
static void alias_forget(GLuint buf) {   // storage re-specified: the cached host pointer may name old storage, so force a remap
    for (int i = 0; i < 16; i++) if (aliases[i].buf == buf && aliases[i].host) { aliases[i].host = 0; aliases[i].maplen = 0; alias_forgets++; }
}
static uint32_t alias_map(GLenum target, GLuint buf, GLint size, GLint off, GLint len, GLbitfield acc) {
    Alias *a = alias_for(buf, size); if (!a || a->bad) return 0;
    void *p = glMapBufferRange(target, off, len, acc);
    if (!p) { a->bad = 1; fprintf(stderr, "[alias] buf %u: host glMapBufferRange returned NULL (err %#x); using shadow path\n", buf, glGetError()); return 0; }
    size_t pg = (size_t)getpagesize(); uintptr_t hp = (uintptr_t)p, hb = hp & ~(uintptr_t)(pg - 1); size_t in = hp - hb;
    size_t maplen = (in + (size_t)len + pg - 1) & ~(pg - 1);
    if (!a->region) {
        a->raw = gmalloc((uint32_t)size + 3u * (uint32_t)pg);
        a->region = (a->raw + (uint32_t)pg - 1) & ~((uint32_t)pg - 1);
        a->rcap = ((uint32_t)size + 2u * (uint32_t)pg) & ~((uint32_t)pg - 1);
    }
    if (maplen > a->rcap) { glUnmapBuffer(target); a->bad = 1; fprintf(stderr, "[alias] buf %u: mapping %zu bytes exceeds region; using shadow path\n", buf, maplen); return 0; }
    int need = gl_alias == 2 || a->host != hb || a->maplen != maplen || (gl_alias == 1 && (acc & (GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_INVALIDATE_RANGE_BIT)));
    if (need) {
        mach_vm_address_t dst = (mach_vm_address_t)(uintptr_t)(M + a->region); vm_prot_t cur = 0, mx = 0;
        kern_return_t kr = mach_vm_remap(mach_task_self(), &dst, maplen, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                         mach_task_self(), (mach_vm_address_t)hb, FALSE, &cur, &mx, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS || !(cur & VM_PROT_WRITE)) {
            glUnmapBuffer(target); a->bad = 1;
            mmap(M + a->region, a->rcap, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
            fprintf(stderr, "[alias] buf %u: mach_vm_remap failed (kr %d, prot %d); using shadow path\n", buf, kr, cur); return 0;
        }
        a->host = hb; a->maplen = maplen; alias_remaps++;
    }
    alias_maps++;
    if (alias_maps <= 6 || (alias_maps % 50000) == 0)
        fprintf(stderr, "[alias] map #%llu buf %u host %p (in-page %zu) off %d len %d acc %#x | remaps %llu, forgets %llu\n",
                (unsigned long long)alias_maps, buf, p, in, off, len, acc, (unsigned long long)alias_remaps, (unsigned long long)alias_forgets);
    return a->region + (uint32_t)in;
}
static void alias_unmap(Mapping *m) {
    // correctness check: every 20000th alias map (first 5), compare the guest view with what the driver holds
    if (alias_checks < 5 && (alias_maps % 20000) == 0) {
        alias_checks++;
        uint8_t *tmp = malloc((size_t)m->size); uint8_t *g = M + m->shadow;
        glUnmapBuffer(m->target);
        glGetBufferSubData(m->target, m->off, m->size, tmp);
        size_t bad = 0, first = (size_t)-1; for (GLint k = 0; k < m->size; k++) if (tmp[k] != g[k]) { bad++; if (first == (size_t)-1) first = (size_t)k; }
        fprintf(stderr, "[alias] check %llu (map #%llu): %zu of %d bytes differ%s\n", (unsigned long long)alias_checks, (unsigned long long)alias_maps, bad, m->size, bad ? " <-- ALIAS IS NOT THE BUFFER" : " (alias matches driver)");
        if (bad) fprintf(stderr, "[alias]   first difference at byte %zu\n", first);
        free(tmp); return;
    }
    glUnmapBuffer(m->target);
}
// passive log of the guest call chain for big ring-buffer maps (first 30 distinct chains)
static void map_chain_log(CPU *c, GLint len, GLbitfield acc) {
    static int seen, calls; static uint32_t hashes[32];
    if (seen >= 30 || len < 4 * 1024 * 1024 || ++calls > 20000) return;
    uint32_t ch[9]; int n = 0; ch[n++] = rd32(c->esp);
    for (uint32_t f = c->ebp; n < 9 && f > 0x1000 && f < 0xF0000000u; ) { ch[n++] = rd32(f + 4); uint32_t nf = rd32(f); if (nf <= f) break; f = nf; }
    uint32_t h = 2166136261u; for (int k = 0; k < n; k++) h = (h ^ ch[k]) * 16777619u; h ^= acc;
    for (int k = 0; k < seen; k++) if (hashes[k] == h) return;
    hashes[seen++] = h;
    fprintf(stderr, "[mapchain] #%d (call %d) acc %#x len %d esp %08x ebp %08x:", seen, calls, acc, len, c->esp, c->ebp);
    for (int k = 0; k < n; k++) fprintf(stderr, " %08x", ch[k]);
    fprintf(stderr, "\n");
}

// ---------- core-profile (GL 3.2) calls that pass pointers or objects (BioShock Infinite) ----------
// glMapBufferRange(target, offset, length, access): guest shadow of the range, written back on unmap or explicit flush
void I__glMapBufferRange(CPU *c) { gl_trace("glMapBufferRange"); gl_trace_args("glMapBufferRange", c, 4);
    GLenum target = ARG(0); GLint off = (GLint)ARG(1), len = (GLint)ARG(2); GLbitfield acc = ARG(3);
    GLint buf = 0, size = 0; glGetIntegerv(binding_query(target), &buf);
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    if (!buf || len <= 0 || size <= 0 || off + len > size) { RET(0); return; }
    map_chain_log(c, len, acc);
    // "Alias" = the game's mapped vertex/index buffers point straight at the GL driver's memory (mach_vm_remap) instead
    // of being copied on every map/unmap. Default ON since 2026-11-24; RT32_NO_ALIAS=1 turns it off, RT32_ALIAS=n forces a mode.
    if (gl_alias < 0) { const char *e = getenv("RT32_ALIAS"); gl_alias = e ? atoi(e) : (getenv("RT32_NO_ALIAS") ? 0 : 1); if (gl_alias) fprintf(stderr, "[alias] mode %d%s\n", gl_alias, e ? "" : " (default; RT32_NO_ALIAS=1 turns it off)"); }   /* Claude Fable - 2026-11-24: default on after ~3 h of play with it on and 0-byte-difference checks */
    if (gl_alias && (acc & GL_MAP_WRITE_BIT) && !(acc & (GL_MAP_READ_BIT | GL_MAP_FLUSH_EXPLICIT_BIT)) && size >= DIRTY_MIN) {
        uint32_t g = alias_map(target, (GLuint)buf, size, off, len, acc);
        if (g) for (int i = 0; i < 64; i++) if (!maps[i].shadow) {
            Mapping *m = &maps[i];
            m->target = target; m->buf = (GLuint)buf; m->size = len; m->access = acc; m->off = off; m->ranged = 1; m->flushed = 0; m->alias = 1; m->shadow = g;
            gl_map_count++; RET(g); return;
        }
        if (g) { fprintf(stderr, "[gl] too many mapped buffers\n"); abort(); }
    }
    for (int i = 0; i < 64; i++) if (!maps[i].shadow) {
        Mapping *m = &maps[i];
        m->target = target; m->buf = (GLuint)buf; m->size = len; m->access = acc; m->off = off; m->ranged = 1; m->alias = 0;
        m->flushed = (acc & GL_MAP_FLUSH_EXPLICIT_BIT) != 0;
        int keep = !(acc & (GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_INVALIDATE_RANGE_BIT));
        m->shadow = buffer_shadow(target, (GLuint)buf, size, keep);
        gl_map_count++;
        if (acc & GL_MAP_READ_BIT) { glGetBufferSubData(target, off, len, M + m->shadow + off); gl_readback_bytes += (uint64_t)len; }
        { static int logged; if (logged < 12 && len >= DIRTY_MIN) { logged++; fprintf(stderr, "[gl] big glMapBufferRange: buf %d size %d off %d len %d access %#x\n", buf, size, off, len, acc); } }
        if (gl_mapfix < 0) gl_mapfix = getenv("RT32_NO_MAPFIX") == NULL;
        if (gl_mapfix && (acc & GL_MAP_WRITE_BIT) && !m->flushed && size >= DIRTY_MIN && (m->shadow & ((uint32_t)getpagesize() - 1)) == 0) {
            m->flushed = 2; m->dsize = size; dirty_begin(i, m->shadow, size); }   // whole buffer protected; upload clipped to the range
        RET(m->shadow + (uint32_t)off); return;
    }
    fprintf(stderr, "[gl] too many mapped buffers\n"); abort();
}
void I__glFlushMappedBufferRange(CPU *c) { gl_trace("glFlushMappedBufferRange");   // (target, offset within the mapping, length)
    GLenum target = ARG(0); GLint buf = 0; glGetIntegerv(binding_query(target), &buf);
    for (int i = 0; i < 64; i++) if (maps[i].shadow && maps[i].buf == (GLuint)buf && maps[i].target == target) {
        Mapping *m = &maps[i];
        // ranged: offset is relative to the mapped range; APPLE style on a whole-buffer map: relative to the buffer (off = 0)
        glBufferSubData(target, m->off + (GLint)ARG(1), (GLsizeiptr)ARG(2), M + m->shadow + m->off + ARG(1));
        gl_flush_bytes += ARG(2);
        if (!m->ranged && m->flushed != 2) m->flushed = 1;
        return;
    }
}
void I__glFlushMappedBufferRangeAPPLE(CPU *c) { I__glFlushMappedBufferRange(c); }
// sync objects: host GLsync pointers behind small guest handles
static GLsync syncs[4096]; static uint32_t nsync = 1;
static uint32_t sync_h(GLsync s) { for (uint32_t i = 1; i < 4096; i++) { uint32_t k = (nsync + i) % 4096; if (k && !syncs[k]) { syncs[k] = s; nsync = k; return k; } } fprintf(stderr, "[gl] sync table full\n"); abort(); }
static GLsync sync_of(uint32_t h) { return h && h < 4096 ? syncs[h] : NULL; }
void I__glFenceSync(CPU *c) { GLsync s = glFenceSync((GLenum)ARG(0), (GLbitfield)ARG(1)); RET(s ? sync_h(s) : 0); }
void I__glDeleteSync(CPU *c) { GLsync s = sync_of(ARG(0)); if (s) { glDeleteSync(s); syncs[ARG(0)] = NULL; } }
void I__glClientWaitSync(CPU *c) {   // (sync, flags, GLuint64 timeout = 2 slots)
    GLsync s = sync_of(ARG(0)); uint64_t t = (uint64_t)ARG(2) | ((uint64_t)ARG(3) << 32);
    RET(s ? glClientWaitSync(s, (GLbitfield)ARG(1), t) : GL_ALREADY_SIGNALED);
}
void I__glWaitSync(CPU *c) { GLsync s = sync_of(ARG(0)); if (s) glWaitSync(s, (GLbitfield)ARG(1), GL_TIMEOUT_IGNORED); }
// glGetStringi: guest copies, cached per index
// Features that are core in the host context (so no longer listed as extensions) but that the game looks up
// by name; they are appended after the host's own extensions, for glGetStringi and GL_NUM_EXTENSIONS alike.
static const char *core_ext_names[] = { "GL_ARB_texture_swizzle", "GL_EXT_texture_swizzle" };
#define NCORE_EXT 2
static GLint host_num_ext(void) { static GLint n = -1; if (n < 0) glGetIntegerv(GL_NUM_EXTENSIONS, &n); return n; }
void I__glGetIntegerv(CPU *c) {   // straight into guest memory (some pnames return arrays), then adjust GL_NUM_EXTENSIONS
    GLenum pname = ARG(0); gl_trace("glGetIntegerv"); gl_trace_args("glGetIntegerv", c, 2);
    if (!ARG(1)) return;
    glGetIntegerv(pname, (GLint *)G2H(ARG(1)));
    if (pname == GL_NUM_EXTENSIONS) { wr32(ARG(1), (uint32_t)(host_num_ext() + NCORE_EXT));
        static int once; if (!once++) fprintf(stderr, "[gl] GL_NUM_EXTENSIONS: %d host + %d core\n", host_num_ext(), NCORE_EXT); }
}
void I__glGetStringi(CPU *c) {
    static uint32_t ext[1024];
    GLenum name = ARG(0); uint32_t i = ARG(1);
    if (name == GL_EXTENSIONS && i >= (uint32_t)host_num_ext() && i < (uint32_t)host_num_ext() + NCORE_EXT) {
        static uint32_t extra[NCORE_EXT]; uint32_t k = i - (uint32_t)host_num_ext();
        if (!extra[k]) { extra[k] = gstrdup(core_ext_names[k]); fprintf(stderr, "[gl] glGetStringi: reporting %s (core in this context)\n", core_ext_names[k]); }
        RET(extra[k]); return;
    }
    if (name == GL_EXTENSIONS && i < 1024) {
        if (!ext[i]) { const char *e = (const char *)glGetStringi(name, i); ext[i] = e ? gstrdup(e) : 0; }
        RET(ext[i]); return;
    }
    const char *e = (const char *)glGetStringi(name, i); RET(e ? gstrdup(e) : 0);
}

// Shader compile/link with diagnostics: failures print the driver's log (the first 30), successes are counted.
static int sh_ok, sh_bad, pr_ok, pr_bad;
static void glCompileShader_impl(CPU *c) {
    GLuint s = ARG(0); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (ok) { if (++sh_ok % 200 == 0) fprintf(stderr, "[gl] %d shaders compiled (%d failed)\n", sh_ok, sh_bad); return; }
    if (++sh_bad <= 30) {
        char log[2048] = ""; glGetShaderInfoLog(s, sizeof log, NULL, log);
        GLint type = 0; glGetShaderiv(s, GL_SHADER_TYPE, &type);
        fprintf(stderr, "[gl] shader %u (%s) failed to compile:\n%s\n", s, type == GL_VERTEX_SHADER ? "vertex" : type == GL_FRAGMENT_SHADER ? "fragment" : "other", log);
        if (sh_bad <= 2) {   // show the start of the generated GLSL once or twice
            char src[1500] = ""; glGetShaderSource(s, sizeof src, NULL, src);
            fprintf(stderr, "[gl] --- source (first 1500 chars) ---\n%s\n[gl] ---\n", src);
        }
    }
}
static void glLinkProgram_impl(CPU *c) {
    GLuint p = ARG(0); glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (getenv("RT32_DUMP_SHADERS")) {   // debug: which shader sources make up each program
        GLuint sh[4] = {0}; GLsizei k = 0; glGetAttachedShaders(p, 4, &k, sh);
        fprintf(stderr, "[gl] link prog %u:", p); for (GLsizei i = 0; i < k; i++) fprintf(stderr, " shader %u", sh[i]); fprintf(stderr, "\n");
    }
    if (ok) { pr_ok++; return; }
    if (++pr_bad <= 20) { char log[2048] = ""; glGetProgramInfoLog(p, sizeof log, NULL, log); fprintf(stderr, "[gl] program %u failed to link:\n%s\n", p, log); }
}

// Hitch logging: host time spent compiling/linking shaders, read and reset by the host every present
volatile uint64_t rt_hitch_shader_ticks; volatile uint32_t rt_hitch_shader_n;
void I__glCompileShader(CPU *c) { uint64_t t = mach_absolute_time(); glCompileShader_impl(c); rt_hitch_shader_ticks += mach_absolute_time() - t; rt_hitch_shader_n++; }
void I__glLinkProgram(CPU *c) { uint64_t t = mach_absolute_time(); glLinkProgram_impl(c); rt_hitch_shader_ticks += mach_absolute_time() - t; }

volatile uint64_t rt_hitch_io_ticks, rt_hitch_io_bytes, rt_hitch_unmap_ticks; volatile uint32_t rt_hitch_faults;
