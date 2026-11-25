// ===== Added by Claude Fable - 2026-11-24 — index of changes in this file =====
//  * MetalFX upscaling (search "MetalFX spatial upscaling"): when the game renders smaller than the window, its
//    finished frame is enlarged by Apple's MetalFX spatial scaler and shown through a Metal layer. F9 = plain stretch.
//  * Performance overlay (search "performance overlay"): fps / frame time / resolution / memory. F12 or RT32_HUD=1.
//  * GL context locking (-update, -reshape, host_present): main-thread surface updates no longer race the game's
//    render thread (startup crash in the GL driver).
//  * Resolution + fullscreen following (frameStep, fitWindowToEngineWidth, followEngineFullscreen): the window follows
//    the resolution and Fullscreen option chosen in the game's own menu.
//  * Extra log lines: [mem] every 1000 frames; texture traffic and disk reads on the [perf] line every 100 frames.
//  * Supported-build check (check_game_binary): the game executable's SHA-256 must match the build the port fits.
//  * Frame probe (search "frame probe"): GPU time, CPU time in GL calls, time waiting for the display and pacing
//    buckets as a [frame] line every 100 frames; [hitch+] names the slowest GL call of each hitch.
//  * On-screen timing (search "on glass"): when frames really appeared, against the display's refresh steps.
//  * Frame pacing by the display (RT32_PACE=display, in startEngine): a display link starts the frames instead of
//    the 60 Hz timer, which lifts the 60 fps ceiling and lets a variable-refresh screen follow the game.
//  * Test runs: scripted controller buttons ("pad:" in RT32_INPUT); a scripted run counts as focused.
//  * Render-thread CPU time per frame on the [frame] line (Claude Opus - 2026-11-25).
//  * Frame burst (present_burst, 2026-11-25): F10 saves the next 48 presented frames as images, for one-frame glitches.
// How a frame reaches the screen now:
//   game draws into its own off-screen buffer  ->  game calls glBlitFramebuffer to the window (gl.c)
//     -> window same size as the game's picture: plain GL copy, then CGLFlushDrawable in host_present()
//     -> window larger: host_mfx_blit() below shows it through Metal instead, and host_present() skips the GL swap
// =====================================================================
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <time.h>
#include <math.h>
// mac32: the native macOS host for a recompiled 32-bit Mac game. Replaces the original app's Cocoa glue (window,
// OpenGL view, event loop) and drives the engine through the game32_* hooks (game32.h).
//   App bundle: finds the user's copy of the original game (remembered choice, Steam, else asks), logs to
//               ~/Library/Logs/<log_name>.log.
//   Development: <binary> ORIG_BINARY RESOURCES_DIR
// Testing: RT32_SHOT=frame[,frame...] writes shot_<frame>.png (to RT32_SHOT_DIR); RT32_EXIT_AFTER=n quits after n
// frames; RT32_INPUT="frame:move:x,y;frame:click;frame:down;frame:up;frame:key:c" scripts input (the real mouse and
// keyboard are then ignored).
#define GL_SILENCE_DEPRECATION
#import <Cocoa/Cocoa.h>
#include <os/lock.h>
#include <CommonCrypto/CommonDigest.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <MetalFX/MetalFX.h>
#import <IOSurface/IOSurface.h>
#import <OpenGL/CGLIOSurface.h>
#import <OpenGL/gl.h>
#include <pthread.h>
#import <OpenGL/glext.h>
#define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED
#import <OpenGL/gl3.h>
#include "rt.h"
#include "tables.h"
#include "game32.h"

void steam_set_app_id(uint32_t id);
extern float gl_win_scale, gl_win_off_x, gl_win_off_y; // gl.c: engine viewport -> drawable mapping

static CPU *gcpu;
static char resources_dir[2048];
const char *host_resources_dir(void) {
    return resources_dir;
}
// GL profile the game expects (e.g. 0x3200 = 3.2 Core). A game32.c that needs a core context defines this symbol.
__attribute__((weak)) int game32_gl_profile = 0;
// The one real GL context. The game may render on its own thread: CGLSetCurrentContext / makeCurrentContext from the
// guest bind this context on the calling thread (see cf.c / objc.c), and presenting flushes it directly.
static CGLContextObj host_ctx;
void *host_cgl_context(void) {
    return host_ctx;
}
// Set once the guest binds the context on its own (render) thread: from then on the main thread must not use it.
static volatile int host_gl_offthread;
// Asked by gl.c: only once the game has moved rendering to its own thread do GL calls need the context lock.
int host_gl_is_offthread(void) {
    return host_gl_offthread;
}
void host_note_gl_bind(void) {
    if (!pthread_main_np() && !host_gl_offthread) {
        host_gl_offthread = 1;
        fprintf(stderr, "[host] the game renders on its own thread; main thread stops using GL\n");
    }
}
static volatile int host_back_w, host_back_h; // window surface size in pixels (set by the main thread)
static void write_png(unsigned char *px, int w, int h, NSString *name) {
    @autoreleasepool {
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                                        pixelsWide:w
                                                                        pixelsHigh:h
                                                                     bitsPerSample:8
                                                                   samplesPerPixel:4
                                                                          hasAlpha:YES
                                                                          isPlanar:NO
                                                                    colorSpaceName:NSDeviceRGBColorSpace
                                                                       bytesPerRow:w * 4
                                                                      bitsPerPixel:32];
        for (int y = 0; y < h; y++) {
            unsigned char *dst = rep.bitmapData + (size_t)y * w * 4, *src = px + (size_t)(h - 1 - y) * w * 4;
            memcpy(dst, src, (size_t)w * 4);
            for (int x = 0; x < w; x++)
                dst[4 * x + 3] = 255;
        }
        NSString *dir = getenv("RT32_SHOT_DIR") ? @(getenv("RT32_SHOT_DIR")) : @".";
        NSString *path = [dir stringByAppendingPathComponent:name];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
        fprintf(stderr, "[host] wrote %s (%dx%d)\n", path.UTF8String, w, h);
    }
}
// size of an FBO's color attachment 0 (texture or renderbuffer); 0 if none
static void fbo_color_size(GLuint fb, int *w, int *h) {
    *w = *h = 0;
    GLint type = 0, obj = 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &obj);
    if (type == GL_TEXTURE) {
        GLint old = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &old);
        glBindTexture(GL_TEXTURE_2D, (GLuint)obj);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, h);
        glBindTexture(GL_TEXTURE_2D, (GLuint)old);
    } else if (type == GL_RENDERBUFFER) {
        GLint old = 0;
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &old);
        glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)obj);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, w);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, h);
        glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)old);
    }
    fprintf(stderr, "[host] fbo %u color0: type 0x%x obj %d size %dx%d\n", fb, type, obj, *w, *h);
}
// RT32_SHOT_PRESENT="60,300": save the frame being presented (read on the presenting thread, which owns the context)
// RT32_SHOT_FBOS="1,4,5": also save color attachment 0 of these guest FBOs at the same frames
// Claude Fable - 2026-11-25: F10 saves the next BURST_N presented frames (every frame, exactly as drawn) for hunting
// one-frame glitches. Frames are kept in memory and written as burst<k>_<present#>.png once the burst is complete,
// so capturing does not slow the frames being captured. Files go to RT32_SHOT_DIR (default: the working directory).
#define BURST_N 48
volatile int host_burst_req; // set by the key handler (main thread), consumed on the presenting thread
static void present_burst(int n) {
    static unsigned char *buf[BURST_N];
    static int num[BURST_N], got, bw, bh, id, active;
    if (host_burst_req && !active) {
        host_burst_req = 0;
        active = 1;
        got = 0;
        bw = host_back_w;
        bh = host_back_h;
        id++;
        fprintf(stderr, "[host] F10: capturing %d frames from present #%d (%dx%d)\n", BURST_N, n, bw, bh);
    }
    if (!active || bw <= 0 || bh <= 0) {
        active = 0;
        return;
    }
    GLint rfb = 0, rbuf = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    glGetIntegerv(GL_READ_BUFFER, &rbuf);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    buf[got] = malloc((size_t)bw * bh * 4);
    glReadPixels(0, 0, bw, bh, GL_RGBA, GL_UNSIGNED_BYTE, buf[got]);
    num[got++] = n;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb);
    glReadBuffer((GLenum)rbuf);
    if (got == BURST_N) {
        for (int i = 0; i < got; i++) {
            write_png(buf[i], bw, bh, [NSString stringWithFormat:@"burst%d_%d.png", id, num[i]]);
            free(buf[i]);
        }
        active = 0;
        fprintf(stderr, "[host] F10: burst %d written\n", id);
    }
}
static void present_shot(int n) {
    present_burst(n);
    const char *shots = getenv("RT32_SHOT_PRESENT");
    if (!shots)
        return;
    char key[32], list[512];
    snprintf(key, sizeof key, ",%d,", n);
    snprintf(list, sizeof list, ",%s,", shots);
    if (!strstr(list, key))
        return;
    int w = host_back_w, h = host_back_h;
    if (w <= 0 || h <= 0)
        return;
    GLint rfb = 0, rbuf = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    glGetIntegerv(GL_READ_BUFFER, &rbuf);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    unsigned char *px = malloc((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    write_png(px, w, h, [NSString stringWithFormat:@"present_%d.png", n]);
    free(px);
    const char *fbos = getenv("RT32_SHOT_FBOS");
    for (const char *q = fbos; q && *q;) {
        GLuint fb = (GLuint)strtoul(q, (char **)&q, 0);
        if (*q == ',')
            q++;
        int fw, fh;
        fbo_color_size(fb, &fw, &fh);
        if (fw > 0 && fh > 0 && fw <= 8192 && fh <= 8192) {
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            unsigned char *fp = calloc((size_t)fw * fh, 4);
            glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, fp);
            GLenum e = glGetError();
            if (e)
                fprintf(stderr, "[host] fbo %u readpixels error 0x%x\n", fb, e);
            write_png(fp, fw, fh, [NSString stringWithFormat:@"present_%d_fbo%u.png", n, fb]);
            free(fp);
        }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb);
    glReadBuffer((GLenum)rbuf);
}
// ---- MetalFX spatial upscaling of the final image, presented through Metal (Claude Fable - 2026-11-24) ----
// When the engine resolution is smaller than the window surface, the engine's final blit to the window is taken over:
//   backbuffer -> IOSurface A[i] (GL blit, flipped for Metal) -> MTLFXSpatialScaler -> texture B -> CAMetalLayer drawable.
// The picture is presented by a Metal layer view laid over the GL view; nothing waits for the GPU (the first version
// went back into GL and had to waitUntilCompleted: measured 17.0 -> 24.5 ms per frame). The GL swap is skipped for
// frames presented this way (host_present). F9 switches back to the plain GL stretch; RT32_NO_METALFX=1 disables.
// ⚠ GL -> Metal ordering relies on glFlush before the Metal commit (no explicit sync object between the two APIs).
//
// Vocabulary for the code below:
//   IOSurface      a block of graphics memory that two APIs can both use. Here OpenGL writes into it and Metal reads
//                  from it, so the frame crosses from one API to the other without passing through the CPU.
//   "A" textures   the small input frames (engine resolution). A ring of MFX_RING of them, so the frame Metal is still
//                  reading is never the one OpenGL is writing next.
//   "B" texture    the enlarged output of the scaler (window resolution), Metal-only.
//   drawable       the image a CAMetalLayer will show next; we copy B into it and present it.
// Each of the "A" surfaces is wrapped twice: as a GL texture attached to a GL framebuffer (so glBlitFramebuffer can
// write to it) and as a Metal texture (so the scaler can read it). Same memory, two handles.
#define MFX_RING 3
static int mfx_state; // 0 = not tried, 1 = working, -1 = unavailable
static int mfx_sw, mfx_sh, mfx_dw, mfx_dh, mfx_i;
static id<MTLDevice> mfx_dev;
static id<MTLCommandQueue> mfx_q;
static id mfx_scaler;
static id<MTLTexture> mfx_ta[MFX_RING], mfx_tb;
static IOSurfaceRef mfx_sa[MFX_RING];
static GLuint mfx_gta[MFX_RING], mfx_fa[MFX_RING];
// Claude Fable - 2026-11-24: "on glass" timing, to verify adaptive sync. macOS reports the moment each frame presented through
// the Metal layer actually appeared on the screen. On a fixed-rate screen the gaps between frames can only be whole
// refresh periods (8.3, 16.7, 25 ms at 120 Hz); on a variable-refresh screen that is following the game they take
// in-between values and stay steady. One "[glass]" line per 100 frames says which of the two is happening.
volatile float host_screen_step_ms = 8.333f; // shortest refresh period of the screen the window is on (main thread)
static os_unfair_lock glass_lock = OS_UNFAIR_LOCK_INIT;
static void glass_note(double when) { // any thread; when = seconds, 0 if the frame was never shown
    static double last, sum, sumsq, lo, hi;
    static int n, on_step[5], between, dropped, calls;
    os_unfair_lock_lock(&glass_lock);
    calls++;
    if (when <= 0) {
        dropped++;
    } else {
        if (last > 0 && when > last) {
            double dt = (when - last) * 1000.0, step = host_screen_step_ms > 1 ? host_screen_step_ms : 8.333;
            double k = dt / step;
            int ki = (int)(k + 0.5);
            if (ki >= 1 && ki <= 4 && fabs(dt - ki * step) < 0.6)
                on_step[ki]++;
            else if (ki > 4 && fabs(dt - ki * step) < 0.6)
                on_step[0]++;
            else
                between++;
            if (!n || dt < lo)
                lo = dt;
            if (!n || dt > hi)
                hi = dt;
            sum += dt;
            sumsq += dt * dt;
            n++;
        }
        last = when;
    }
    if (calls >= 100 && n < 50) { // macOS is not reporting display times (seen after the window lost focus once)
        fprintf(stderr, "[glass] last %d frames: only %d came back with a display time, %d reported as never shown\n",
                calls, n, dropped);
        calls = n = between = dropped = 0;
        sum = sumsq = 0;
        memset(on_step, 0, sizeof on_step);
    }
    if (n == 100) {
        calls = 0;
        double avg = sum / n, var = sumsq / n - avg * avg;
        fprintf(
            stderr,
            "[glass] last 100 frames on screen: every %.1f ms on average (shortest %.1f, longest %.1f, spread %.1f) | "
            "exactly on a %.1f ms refresh step: %d (x1 %d, x2 %d, x3 %d, x4 %d, more %d) | between steps: %d | never "
            "shown: %d\n",
            avg, lo, hi, var > 0 ? sqrt(var) : 0.0, (double)host_screen_step_ms,
            on_step[1] + on_step[2] + on_step[3] + on_step[4] + on_step[0], on_step[1], on_step[2], on_step[3],
            on_step[4], on_step[0], between, dropped);
        n = 0;
        sum = sumsq = 0;
        between = dropped = 0;
        memset(on_step, 0, sizeof on_step);
    }
    os_unfair_lock_unlock(&glass_lock);
}
static CAMetalLayer *mfx_layer;
static NSView *mfx_view; // created on the main thread (mfx_make_view)
static volatile int mfx_view_shown, mfx_frame_presented;
volatile int host_mfx_active, host_mfx_off;
volatile uint32_t host_mfx_us; // for the overlay
// Show or hide the Metal layer view. Called from the render thread; AppKit views may only be touched on the main
// thread, so the actual change is posted there. While hidden, the ordinary GL picture underneath is what you see.
static void mfx_show(int on) {
    if (mfx_view_shown == on)
        return;
    mfx_view_shown = on;
    dispatch_async(dispatch_get_main_queue(), ^{
      mfx_view.hidden = !mfx_view_shown;
    });
}
// One shareable BGRA image, w x h.
static IOSurfaceRef mfx_surface(int w, int h) {
    NSDictionary *d = @{
        (id)kIOSurfaceWidth : @(w),
        (id)kIOSurfaceHeight : @(h),
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA')
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef)d);
}
// Give OpenGL a way to draw into an IOSurface: a rectangle texture backed by the surface, attached to a framebuffer.
// (Rectangle textures are the only kind macOS lets you back with an IOSurface.) Leaves that framebuffer bound.
static int mfx_gl_attach(IOSurfaceRef surf, int w, int h, GLuint *tex, GLuint *fbo) {
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_RECTANGLE, *tex);
    CGLError e = CGLTexImageIOSurface2D(CGLGetCurrentContext(), GL_TEXTURE_RECTANGLE, GL_RGBA, w, h, GL_BGRA,
                                        GL_UNSIGNED_INT_8_8_8_8_REV, surf, 0);
    if (e) {
        fprintf(stderr, "[mfx] CGLTexImageIOSurface2D failed: %d\n", (int)e);
        return 0;
    }
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, *tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[mfx] framebuffer incomplete: 0x%x\n", st);
        return 0;
    }
    return 1;
}
// (Re)build everything for one input size -> output size pair. A MetalFX scaler is fixed to its sizes, so this runs
// again whenever the game's resolution or the window size changes (e.g. entering/leaving fullscreen).
// Colour mode "perceptual" = the input is an ordinary gamma-encoded picture, which is what the game's final frame is.
static int mfx_setup(int sw, int sh, int dw, int dh) { // GL state it disturbs is restored by the caller
    if (@available(macOS 13.0, *)) {
        if (!mfx_dev || !mfx_layer) {
            fprintf(stderr, "[mfx] no Metal device/layer\n");
            return 0;
        }
        if (!mfx_q)
            mfx_q = [mfx_dev newCommandQueue];
        for (int i = 0; i < MFX_RING; i++) {
            if (mfx_gta[i]) {
                glDeleteFramebuffers(1, &mfx_fa[i]);
                glDeleteTextures(1, &mfx_gta[i]);
                mfx_gta[i] = mfx_fa[i] = 0;
            }
            mfx_ta[i] = nil;
            if (mfx_sa[i]) {
                CFRelease(mfx_sa[i]);
                mfx_sa[i] = NULL;
            }
        }
        mfx_tb = nil;
        mfx_scaler = nil;
        MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
        d.inputWidth = sw;
        d.inputHeight = sh;
        d.outputWidth = dw;
        d.outputHeight = dh;
        d.colorTextureFormat = MTLPixelFormatBGRA8Unorm;
        d.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
        d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
        id<MTLFXSpatialScaler> sc = [d newSpatialScalerWithDevice:mfx_dev];
        if (!sc) {
            fprintf(stderr, "[mfx] MetalFX refused %dx%d -> %dx%d\n", sw, sh, dw, dh);
            return 0;
        }
        MTLTextureDescriptor *ta = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:sw
                                                                                     height:sh
                                                                                  mipmapped:NO];
        ta.usage = sc.colorTextureUsage | MTLTextureUsageShaderRead;
        for (int i = 0; i < MFX_RING; i++) {
            mfx_sa[i] = mfx_surface(sw, sh);
            if (!mfx_sa[i]) {
                fprintf(stderr, "[mfx] IOSurface creation failed\n");
                return 0;
            }
            mfx_ta[i] = [mfx_dev newTextureWithDescriptor:ta iosurface:mfx_sa[i] plane:0];
            if (!mfx_ta[i] || !mfx_gl_attach(mfx_sa[i], sw, sh, &mfx_gta[i], &mfx_fa[i])) {
                fprintf(stderr, "[mfx] input texture setup failed\n");
                return 0;
            }
        }
        MTLTextureDescriptor *tb = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:dw
                                                                                     height:dh
                                                                                  mipmapped:NO];
        tb.usage = sc.outputTextureUsage | MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        tb.storageMode = MTLStorageModePrivate;
        mfx_tb = [mfx_dev newTextureWithDescriptor:tb];
        if (!mfx_tb) {
            fprintf(stderr, "[mfx] output texture creation failed\n");
            return 0;
        }
        mfx_scaler = sc;
        mfx_sw = sw;
        mfx_sh = sh;
        mfx_dw = dw;
        mfx_dh = dh;
        fprintf(stderr, "[mfx] MetalFX spatial upscaling on (Metal present): %dx%d -> %dx%d (%.2fx)\n", sw, sh, dw, dh,
                (double)dw / sw);
        return 1;
    }
    fprintf(stderr, "[mfx] MetalFX needs macOS 13 or later\n");
    return 0;
}
// Called from glBlitFramebuffer when the destination is the window framebuffer (draw fb = 0, read fb = the engine's
// backbuffer). Rects: source in engine pixels, destination already mapped to window pixels. Returns 1 if it took the frame.
int host_mfx_blit(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1) {
    if (host_mfx_off || mfx_state < 0) {
        host_mfx_active = 0;
        mfx_show(0);
        return 0;
    }
    if (mfx_state == 0 && getenv("RT32_NO_METALFX")) {
        mfx_state = -1;
        return 0;
    }
    int sw = abs(sx1 - sx0), sh = abs(sy1 - sy0), dw = abs(dx1 - dx0), dh = abs(dy1 - dy0);
    int bw = host_back_w, bh = host_back_h;
    if (sw < 320 || sh < 200 || dw <= sw || dh <= sh || bw < dw || bh < dh) {
        host_mfx_active = 0;
        mfx_show(0);
        return 0;
    }
    int handled = 0;
    @autoreleasepool {
        if (@available(macOS 13.0, *)) {
            GLint rfb = 0, trect = 0;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
            glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE, &trect);
            GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
            if (sc)
                glDisable(GL_SCISSOR_TEST);
            int ok = 1;
            if (!mfx_scaler || sw != mfx_sw || sh != mfx_sh || dw != mfx_dw || dh != mfx_dh) {
                ok = mfx_setup(sw, sh, dw, dh);
                if (!ok) {
                    mfx_state = -1;
                    mfx_scaler = nil;
                    fprintf(stderr, "[mfx] disabled; using the plain stretch\n");
                } else
                    mfx_state = 1;
            }
            if (ok) {
                uint64_t t0 = mach_absolute_time();
                // Step 1: make the Metal layer's images the size of the window surface in pixels.
                CGSize ds = mfx_layer.drawableSize;
                if ((int)ds.width != bw || (int)ds.height != bh)
                    mfx_layer.drawableSize = CGSizeMake(bw, bh);
                // Step 2 (OpenGL): copy the game's finished frame into the next "A" surface, then glFlush so the
                // GL driver actually submits that copy before Metal is asked to read the surface.
                int i = mfx_i = (mfx_i + 1) % MFX_RING;
                // GL rows run bottom-up, Metal's top-down: flip here unless the engine's own blit already flips
                int engine_flips = ((sy1 - sy0) < 0) != ((dy1 - dy0) < 0);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mfx_fa[i]);
                if (engine_flips)
                    glBlitFramebuffer(sx0, sy0 < sy1 ? sy0 : sy1, sx1, sy0 < sy1 ? sy1 : sy0, 0, 0, sw, sh,
                                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
                else
                    glBlitFramebuffer(sx0, sy0 < sy1 ? sy0 : sy1, sx1, sy0 < sy1 ? sy1 : sy0, 0, sh, sw, 0,
                                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
                glFlush();
                uint64_t t1 = mach_absolute_time();
                // Step 3 (Metal): get the image the layer will show next. This call can block until one is free;
                // that waiting is the display's pacing, not work, so it is timed separately (t1..t2).
                id<CAMetalDrawable> dr = [mfx_layer nextDrawable];
                uint64_t t2 = mach_absolute_time();
                {
                    extern volatile uint64_t host_probe_wait_ticks;
                    host_probe_wait_ticks += t2 - t1; // frame probe: time spent waiting for the display
                }
                if (dr) {
                    id<MTLFXSpatialScaler> s = mfx_scaler;
                    id<MTLCommandBuffer> cb = [mfx_q commandBuffer];
                    // Step 4: queue the upscale A[i] -> B.
                    s.colorTexture = mfx_ta[i];
                    s.outputTexture = mfx_tb;
                    [s encodeToCommandBuffer:cb];
                    // Step 5: queue the copy of B into the drawable at the position the game's picture has in the
                    // window (ox, oy; Metal's y runs downward, GL's upward, hence bh - top).
                    int ox = dx0 < dx1 ? dx0 : dx1, oy = bh - (dy0 > dy1 ? dy0 : dy1);
                    if (ox != 0 || oy != 0 || dw != bw || dh != bh) { // letterboxed: clear the bars
                        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                        rp.colorAttachments[0].texture = dr.texture;
                        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
                        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
                    }
                    if (ox < 0)
                        ox = 0;
                    if (oy < 0)
                        oy = 0;
                    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
                    [be copyFromTexture:mfx_tb
                              sourceSlice:0
                              sourceLevel:0
                             sourceOrigin:MTLOriginMake(0, 0, 0)
                               sourceSize:MTLSizeMake(dw, dh, 1)
                                toTexture:dr.texture
                         destinationSlice:0
                         destinationLevel:0
                        destinationOrigin:MTLOriginMake(ox, oy, 0)];
                    [be endEncoding];
                    // Step 6: show it and return immediately. Nothing here waits for the GPU; the first version did
                    // (waitUntilCompleted) and that alone cost about 7.5 ms per frame.
                    [dr addPresentedHandler:^(id<MTLDrawable> shown) {
                      glass_note(shown.presentedTime);
                    }];
                    [cb presentDrawable:dr];
                    [cb commit];
                    handled = 1;
                    host_mfx_active = 1;
                    mfx_frame_presented = 1;
                    mfx_show(1);
                    static mach_timebase_info_data_t tb;
                    if (!tb.denom)
                        mach_timebase_info(&tb);
                    static uint64_t acc, accw;
                    static int cnt;
                    acc += ((t1 - t0) + (mach_absolute_time() - t2)) * tb.numer / tb.denom / 1000;
                    accw += (t2 - t1) * tb.numer / tb.denom / 1000;
                    cnt++;
                    if (cnt == 100) {
                        host_mfx_us = (uint32_t)(acc / cnt);
                        static int rep;
                        if (rep++ % 10 == 0)
                            fprintf(stderr,
                                    "[mfx] per frame: %.2f ms work, %.2f ms waiting for a free drawable (vsync) (%dx%d "
                                    "-> %dx%d)\n",
                                    acc / cnt / 1000.0, accw / cnt / 1000.0, sw, sh, dw, dh);
                        acc = accw = 0;
                        cnt = 0;
                    }
                } else {
                    static int w;
                    if (w++ < 5)
                        fprintf(stderr, "[mfx] no drawable this frame; plain stretch\n");
                }
            }
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb);
            glBindTexture(GL_TEXTURE_RECTANGLE, (GLuint)trect);
            if (sc)
                glEnable(GL_SCISSOR_TEST);
        }
    }
    if (!handled) {
        host_mfx_active = 0;
        mfx_show(0);
    }
    return handled;
}
@interface RT32MetalView : NSView
@end
@implementation RT32MetalView
- (NSView *)hitTest:(NSPoint)p {
    return nil;
} // mouse events go to the game view underneath
@end
// Create the Metal layer view once, as a hidden subview covering the whole GL view.
//   framebufferOnly = NO   we copy into the drawable with a blit, which needs it to be an ordinary texture
//   colorspace = nil       no colour management, the same as the GL view, so both paths show identical colours
static void mfx_make_view(NSView *parent) { // main thread, at window creation
    if (getenv("RT32_NO_METALFX"))
        return;
    mfx_dev = MTLCreateSystemDefaultDevice();
    if (!mfx_dev)
        return;
    mfx_layer = [CAMetalLayer layer];
    mfx_layer.device = mfx_dev;
    mfx_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    mfx_layer.framebufferOnly = NO;
    mfx_layer.opaque = YES;
    mfx_layer.colorspace = nil;
    mfx_view = [[RT32MetalView alloc] initWithFrame:parent.bounds];
    mfx_view.layer = mfx_layer;
    mfx_view.wantsLayer = YES;
    mfx_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    mfx_view.hidden = YES;
    [parent addSubview:mfx_view];
}

static id host_pace_link; // the CADisplayLink when RT32_PACE=display is in use (main thread)
static float host_pace_top;
// ---- frame probe (Claude Fable - 2026-11-24): where each frame's time goes ----
// Answers "is a slow or uneven frame the GPU, the driver, or waiting for the display?" with four numbers per frame:
//   GPU        time the GPU spent on the frame (an OpenGL timer query around everything between two presents;
//              the result arrives a frame late, which is fine for averages)
//   GL calls   CPU time the render thread spent inside OpenGL calls (the driver's share of the CPU work)
//   wait       time blocked in the buffer swap / waiting for a drawable (that is the display's pacing, not work)
//   interval   time between presents, sorted into refresh-sized buckets to show how even the pacing is
// Printed as one "[frame]" line per 100 frames, and a "[hitch+]" line after each "[hitch]" line naming the slowest
// single GL call. RT32_NO_GPU_TIME=1 leaves the timer query out (the other numbers stay).
volatile float host_gpu_ms;              // latest GPU frame time, for the overlay
volatile uint64_t host_probe_wait_ticks; // added to by whoever blocks for the display (swap, nextDrawable)
static GLuint probe_q[2];
static int probe_q_active = -1, probe_q_state = 0; // state: 0 untested, 1 working, -1 unavailable
static int probe_q_ended[2];
static struct {
    int n, gpu_n, b[4];
    double dt, gpu, gpu_max, gl, wait, cpu;
} probe_acc;
static double probe_ms(uint64_t ticks) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    return (double)ticks * tb.numer / tb.denom / 1e6;
}
// Render thread, just before the frame is shown.
static void probe_before_swap(int present) {
    extern volatile uint64_t gl_ticks_total, gl_slow_ticks;
    extern const char *volatile gl_slow_name;
    static uint64_t t_last, gl_last, cpu_last;
    uint64_t now = mach_absolute_time();
    // Claude Opus - 2026-11-25: CPU time this thread used per frame. Unlike the frame interval it is not hidden by the
    // 60 Hz frame cap, so it shows small CPU savings (or costs) directly.
    uint64_t cpu_now = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
    if (probe_q_state == 0 && getenv("RT32_NO_GPU_TIME"))
        probe_q_state = -1;
    if (probe_q_state >= 0) {
        if (!probe_q[0]) {
            while (glGetError()) {
            }
            glGenQueries(2, probe_q);
        }
        if (probe_q_active >= 0) {
            glEndQuery(GL_TIME_ELAPSED);
            probe_q_ended[probe_q_active] = 1;
        }
        int other = probe_q_active < 0 ? 0 : 1 - probe_q_active;
        if (probe_q_ended[other]) { // ended one present ago: its result should be ready
            GLint ready = 0;
            glGetQueryObjectiv(probe_q[other], GL_QUERY_RESULT_AVAILABLE, &ready);
            if (ready) {
                GLuint64 ns = 0;
                glGetQueryObjectui64v(probe_q[other], GL_QUERY_RESULT, &ns);
                double ms = (double)ns / 1e6;
                host_gpu_ms = (float)ms;
                probe_acc.gpu += ms;
                probe_acc.gpu_n++;
                if (ms > probe_acc.gpu_max)
                    probe_acc.gpu_max = ms;
            }
            probe_q_ended[other] = 0;
        }
        if (probe_q_state == 0 && present > 30) { // decide once whether the driver really supports this
            GLenum e = glGetError();
            probe_q_state = (e == GL_NO_ERROR && probe_acc.gpu_n > 0) ? 1 : -1;
            fprintf(stderr, "[frame] GPU timer query: %s (GL error 0x%x, %d results so far)\n",
                    probe_q_state > 0 ? "working" : "NOT available, GPU time will not be reported", e, probe_acc.gpu_n);
        }
    }
    if (t_last) {
        double dt = probe_ms(now - t_last), gl = probe_ms(gl_ticks_total - gl_last),
               wait = probe_ms(host_probe_wait_ticks), slow = probe_ms(gl_slow_ticks);
        probe_acc.n++;
        probe_acc.dt += dt;
        probe_acc.gl += gl;
        probe_acc.wait += wait;
        if (cpu_now >= cpu_last)
            probe_acc.cpu += (double)(cpu_now - cpu_last) / 1e6;
        probe_acc.b[dt <= 18.0 ? 0 : dt <= 27.0 ? 1 : dt <= 36.0 ? 2 : 3]++;
        if (dt > 50.0)
            fprintf(stderr,
                    "[hitch+] present #%d: GL calls %.1f ms of CPU, slowest single call %s %.1f ms | waiting for the "
                    "display %.1f ms | GPU (frame before) %.1f ms\n",
                    present, gl, gl_slow_name, slow, wait, (double)host_gpu_ms);
        if (probe_acc.n == 100) {
            fprintf(stderr,
                    "[frame] last 100 frames: interval %.1f ms | GPU avg %.1f max %.1f ms (%d samples) | GL calls %.1f "
                    "ms CPU | waiting for the display %.1f ms | pacing: %d at <=18 ms, %d at 18-27, %d at 27-36, %d "
                    "slower | render thread CPU %.2f ms\n",
                    probe_acc.dt / 100, probe_acc.gpu_n ? probe_acc.gpu / probe_acc.gpu_n : 0.0, probe_acc.gpu_max,
                    probe_acc.gpu_n, probe_acc.gl / 100, probe_acc.wait / 100, probe_acc.b[0], probe_acc.b[1],
                    probe_acc.b[2], probe_acc.b[3], probe_acc.cpu / 100);
            {
                extern volatile uint32_t gl_sh_new, gl_sh_repeat;
                extern volatile uint64_t gl_sh_new_ticks, gl_sh_repeat_ticks;
                static uint32_t n0, r0;
                if (gl_sh_new != n0 || gl_sh_repeat != r0) { // only when something was compiled since the last line
                    n0 = gl_sh_new;
                    r0 = gl_sh_repeat;
                    fprintf(stderr,
                            "[shader] so far: %u first-time sources compiled in %.0f ms (%.2f ms each), %u repeats in "
                            "%.0f ms (%.2f ms each)\n",
                            n0, probe_ms(gl_sh_new_ticks), n0 ? probe_ms(gl_sh_new_ticks) / n0 : 0.0, r0,
                            probe_ms(gl_sh_repeat_ticks), r0 ? probe_ms(gl_sh_repeat_ticks) / r0 : 0.0);
                }
            }
            memset(&probe_acc, 0, sizeof probe_acc);
        }
    }
    t_last = now;
    gl_last = gl_ticks_total;
    cpu_last = cpu_now;
    host_probe_wait_ticks = 0;
    gl_slow_ticks = 0;
}
// Render thread, right after the frame was handed over: start timing the next one.
static void probe_after_swap(void) {
    if (probe_q_state < 0 || !probe_q[0])
        return;
    int next = probe_q_active < 0 ? 0 : 1 - probe_q_active;
    glBeginQuery(GL_TIME_ELAPSED, probe_q[next]);
    probe_q_active = next;
}

volatile int host_present_count;
// Bytes the game read from disk since the last [perf] line (summed from the per-frame hitch counter).
volatile uint64_t rt_perf_io_bytes;
volatile int host_capture_request,
    host_capture_at[12]; // backslash arms; the next Return or click captures 3 frames (RT32_DUMP_TARGETS)
static int capture_armed;
static int gl_samples_active_prev_check(void) {
    extern volatile int gl_samples_active;
    return gl_samples_active;
}
void host_present(void) {
    static int n;
    n++;
    host_present_count = n;
    CGLContextObj c = host_ctx ? host_ctx : CGLGetCurrentContext();
    extern volatile uint32_t gl_calls_frame, gl_draws_frame;
    GLenum err = glGetError();
    if (n == 1 || n % 300 == 0 || (err && n < 20))
        fprintf(stderr, "[host] present #%d: %u GL calls, %u draws this frame, glGetError=0x%x\n", n, gl_calls_frame,
                gl_draws_frame, err);
    { // frame timer: average ms per present and GL calls/draws per frame, printed every 100 presents
        static double t_last, t_acc;
        static uint64_t calls_acc, draws_acc;
        static int fr;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
        static double worst;
        static int hitches;
        extern volatile uint64_t rt_hitch_shader_ticks, rt_hitch_bink_ticks;
        extern volatile uint32_t rt_hitch_shader_n;
        static mach_timebase_info_data_t tbi;
        if (!tbi.denom)
            mach_timebase_info(&tbi);
        double sh_ms = (double)rt_hitch_shader_ticks * tbi.numer / tbi.denom / 1e6,
               bk_ms = (double)rt_hitch_bink_ticks * tbi.numer / tbi.denom / 1e6;
        if (t_last > 0) {
            double dt = now - t_last;
            t_acc += dt;
            calls_acc += gl_calls_frame;
            draws_acc += gl_draws_frame;
            fr++;
            if (dt > worst)
                worst = dt;
            {
                extern volatile float host_hud_ms, host_hud_worst;
                static double a, w;
                static int k; // Claude Fable - 2026-11-24: overlay numbers (30-frame windows)
                a += dt;
                k++;
                if (dt > w)
                    w = dt;
                if (k == 30) {
                    host_hud_ms = (float)(a / k);
                    host_hud_worst = (float)w;
                    a = 0;
                    w = 0;
                    k = 0;
                }
            }
            extern volatile uint64_t rt_hitch_io_ticks, rt_hitch_io_bytes, rt_hitch_unmap_ticks;
            extern volatile uint32_t rt_hitch_faults;
            double io_ms = (double)rt_hitch_io_ticks * tbi.numer / tbi.denom / 1e6,
                   um_ms = (double)rt_hitch_unmap_ticks * tbi.numer / tbi.denom / 1e6;
            if (dt > 50.0) {
                hitches++;
                fprintf(stderr,
                        "[hitch] present #%d: %.1f ms | shaders %.1f ms (%u) | movie %.1f ms | file I/O %.1f ms (%.2f "
                        "MB) | unmap %.1f ms, %u page faults | %u draws\n",
                        n, dt, sh_ms, rt_hitch_shader_n, bk_ms, io_ms, rt_hitch_io_bytes / 1e6, um_ms, rt_hitch_faults,
                        gl_draws_frame);
            }
        }
        {
            extern volatile uint64_t rt_hitch_io_ticks, rt_hitch_io_bytes, rt_hitch_unmap_ticks;
            extern volatile uint32_t rt_hitch_faults;
            {
                extern volatile uint64_t rt_perf_io_bytes;
                rt_perf_io_bytes += rt_hitch_io_bytes;
            }
            rt_hitch_io_ticks = 0;
            rt_hitch_io_bytes = 0;
            rt_hitch_unmap_ticks = 0;
            rt_hitch_faults = 0;
        }
        rt_hitch_shader_ticks = 0;
        rt_hitch_bink_ticks = 0;
        rt_hitch_shader_n = 0;
        t_last = now;
        if (fr == 100) {
            extern volatile uint64_t gl_map_count, gl_readback_bytes, gl_unmap_bytes, gl_flush_bytes;
            static uint64_t m0, r0, u0, f0;
            fprintf(stderr,
                    "[perf] present #%d: %.1f ms/frame (%.1f fps), %llu GL calls, %llu draws per frame | per frame: "
                    "%llu maps, %.2f MB read back, %.2f MB unmap upload, %.2f MB flushed\n",
                    n, t_acc / fr, 1000.0 * fr / t_acc, (unsigned long long)(calls_acc / fr),
                    (unsigned long long)(draws_acc / fr), (unsigned long long)((gl_map_count - m0) / fr),
                    (gl_readback_bytes - r0) / 1e6 / fr, (gl_unmap_bytes - u0) / 1e6 / fr,
                    (gl_flush_bytes - f0) / 1e6 / fr);
            m0 = gl_map_count;
            r0 = gl_readback_bytes;
            u0 = gl_unmap_bytes;
            f0 = gl_flush_bytes;
            {
                static int k;
                if ((k++ % 10) == 0) {
                    task_vm_info_data_t vi;
                    mach_msg_type_number_t cnt =
                        TASK_VM_INFO_COUNT; // Claude Fable - 2026-11-24: memory every 1000 frames
                    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) == KERN_SUCCESS) {
                        extern uint32_t rt_heap_used_mb(void);
                        fprintf(stderr,
                                "[mem] present #%d: physical footprint %.0f MB, game heap reached %u MB of its 3328 MB "
                                "limit\n",
                                n, (double)vi.phys_footprint / 1048576.0, rt_heap_used_mb());
                    }
                }
            }
            {
                extern volatile uint32_t gl_tex_comp, gl_tex_plain, gl_tex_gen, gl_tex_del;
                extern volatile uint64_t rt_perf_io_bytes;
                fprintf(stderr,
                        "[perf]   worst frame %.1f ms, %d frames over 50 ms | last 100 frames: %u compressed + %u "
                        "plain texture uploads, %u textures created, %u deleted, %.1f MB read from disk\n",
                        worst, hitches, gl_tex_comp, gl_tex_plain, gl_tex_gen, gl_tex_del,
                        (double)rt_perf_io_bytes / 1e6);
                gl_tex_comp = gl_tex_plain = gl_tex_gen = gl_tex_del = 0;
                rt_perf_io_bytes = 0;
            }
            worst = 0;
            hitches = 0;
            t_acc = 0;
            calls_acc = draws_acc = 0;
            fr = 0;
        }
    }
    gl_calls_frame = gl_draws_frame = 0;
    {
        extern void gl_hist_dump(int), gl_hist_reset(void);
        static int hf = -2;
        if (hf == -2)
            hf = getenv("RT32_GL_HIST") ? atoi(getenv("RT32_GL_HIST")) : -1;
        if (n == hf)
            gl_hist_dump(n);
        gl_hist_reset();
    }
    { // RT32_GL_SAMPLES frame just ended: save every colour target it drew into, in draw order (tex_<order>_<id>.png)
        extern uint32_t gl_samp_tex[64], gl_samp_prog[64];
        extern int gl_samp_ntex;
        if (gl_samp_ntex && !gl_samples_active_prev_check() && getenv("RT32_DUMP_TARGETS")) {
            GLint old = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &old);
            GLint pb = 0;
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pb);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            for (int k = 0; k < gl_samp_ntex; k++) {
                GLint w = 0, hh = 0, fmt = 0;
                glBindTexture(GL_TEXTURE_2D, gl_samp_tex[k]);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &hh);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
                if (w <= 0 || hh <= 0 || w > 4096 || hh > 4096)
                    continue;
                float *f = malloc((size_t)w * hh * 16);
                unsigned char *px = malloc((size_t)w * hh * 4);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, f);
                for (size_t i = 0; i < (size_t)w * hh * 4; i++) {
                    float v = f[i];
                    v = v < 0 ? 0 : v > 1 ? 1 : v;
                    if (v != v)
                        v = 1;
                    px[i] = (unsigned char)(v * 255.0f + 0.5f);
                }
                { // float statistics per channel: NaN / Inf / negative counts, min / max, and a sample of "purple" texels
                    double mn[4] = {1e30, 1e30, 1e30, 1e30}, mx[4] = {-1e30, -1e30, -1e30, -1e30};
                    size_t nan[4] = {0}, inf[4] = {0}, neg[4] = {0}, purple = 0;
                    int shown = 0;
                    for (size_t i = 0; i < (size_t)w * hh; i++) {
                        for (int ch = 0; ch < 4; ch++) {
                            float v = f[4 * i + ch];
                            if (v != v) {
                                nan[ch]++;
                                continue;
                            }
                            if (isinf(v)) {
                                inf[ch]++;
                                continue;
                            }
                            if (v < 0)
                                neg[ch]++;
                            if (v < mn[ch])
                                mn[ch] = v;
                            if (v > mx[ch])
                                mx[ch] = v;
                        }
                        float r = f[4 * i], g = f[4 * i + 1], b = f[4 * i + 2];
                        if (r > 0.45f && b > 0.45f && g < 0.6f * fminf(r, b)) {
                            purple++;
                            if (shown < 4 && (i % 997) == 0) {
                                shown++;
                                fprintf(stderr, "[host]     purple texel (%zu,%zu): %g %g %g\n", i % (size_t)w,
                                        i / (size_t)w, r, g, b);
                            }
                        }
                    }
                    fprintf(stderr,
                            "[host]     stats R %g..%g nan %zu inf %zu neg %zu | G %g..%g nan %zu inf %zu neg %zu | B "
                            "%g..%g nan %zu inf %zu neg %zu | purple %zu\n",
                            mn[0], mx[0], nan[0], inf[0], neg[0], mn[1], mx[1], nan[1], inf[1], neg[1], mn[2], mx[2],
                            nan[2], inf[2], neg[2], purple);
                }
                write_png(px, w, hh, [NSString stringWithFormat:@"tex_f%d_%02d_%u.png", n, k, gl_samp_tex[k]]);
                fprintf(stderr, "[host] target %d: tex %u %dx%d fmt 0x%x (first drawn by prog %u)\n", k, gl_samp_tex[k],
                        w, hh, fmt, gl_samp_prog[k]);
                free(f);
                free(px);
            }
            glBindTexture(GL_TEXTURE_2D, (GLuint)old);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pb);
            gl_samp_ntex = 0;
            { // also the presented frame
                int w = host_back_w, hh = host_back_h;
                if (w > 0 && hh > 0) {
                    GLint rfb = 0;
                    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                    glReadBuffer(GL_BACK);
                    unsigned char *px = malloc((size_t)w * hh * 4);
                    glReadPixels(0, 0, w, hh, GL_RGBA, GL_UNSIGNED_BYTE, px);
                    write_png(px, w, hh, [NSString stringWithFormat:@"capture_f%d.png", n]);
                    free(px);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb);
                }
            }
        }
    }
    {
        extern volatile int gl_samples_active;
        static int sf = -2;
        if (sf == -2)
            sf = getenv("RT32_GL_SAMPLES") ? atoi(getenv("RT32_GL_SAMPLES")) : -1;
        extern volatile int host_capture_request, host_capture_at[12];
        if (host_capture_request) { // armed capture triggered: sample every 3rd frame from +3 to +36 (the selection light effect is brief)
            host_capture_request = 0;
            for (int k = 0; k < 12; k++)
                host_capture_at[k] = n + 3 * (k + 1);
            fprintf(stderr, "[host] capture triggered at frame %d: 12 captures, %d..%d\n", n, host_capture_at[0],
                    host_capture_at[11]);
        }
        for (int k = 0; k < 12; k++)
            if (host_capture_at[k] && host_capture_at[k] == n + 1) {
                sf = n + 1;
                host_capture_at[k] = 0;
                fprintf(stderr, "[host] capturing frame %d\n", sf);
            }
        gl_samples_active = (n + 1 == sf);
    }
    present_shot(n);
    // gl.c: publish the output resolution the game used this frame (drives window resizing in frameStep)
    {
        extern void gl_frame_publish(void);
        gl_frame_publish();
    }
    probe_before_swap(n);
    if (mfx_frame_presented) {
        mfx_frame_presented = 0;
        glFlush();
    } // this frame went out through the Metal layer
    else if (c) {
        uint64_t w0 = mach_absolute_time();
        CGLLockContext(c);
        CGLFlushDrawable(c);
        CGLUnlockContext(c);
        host_probe_wait_ticks += mach_absolute_time() - w0;
    } // serialised with main-thread -update (Claude Fable - 2026-11-24)
    probe_after_swap();
}

static BOOL game_wants_system_cursor; // set by the game through host_show_cursor
static NSView *game_view;

// ---- performance overlay (Claude Fable - 2026-11-24) ----
// Apple's Metal HUD does not attach to an OpenGL game, so this is our own: a small click-through child window in the
// top-left corner. RT32_HUD=1 shows it at launch; F12 toggles it.
// It is a separate borderless window attached to the game window (a "child window"), not something drawn by the game:
// that way it costs the renderer nothing and cannot disturb the game's GL state. A timer refreshes it twice a second.
// Line 1: fps and frame time averaged over the last 30 frames, and the slowest of those 30 (host_present fills these).
//         "GPU" = how long the graphics chip worked on the latest frame (frame probe above).
// Line 2: the game's resolution, and how it reaches the window: "native", "MetalFX" (+ CPU-side cost) or "stretch".
// Line 3: the process's memory footprint, and the game heap's high-water mark out of its 3328 MB address-space limit.
volatile float host_hud_ms, host_hud_worst;
static NSWindow *hud_win;
static NSTextField *hud_label;
static NSTimer *hud_timer;
// Refresh the text and keep the overlay pinned to the top-left of the game window's content area (main thread).
static void hud_update(void) {
    NSWindow *pw = game_view.window;
    if (!hud_win || !pw)
        return;
    extern volatile int gl_engine_w, gl_engine_h, host_mfx_active;
    extern volatile uint32_t host_mfx_us;
    extern uint32_t rt_heap_used_mb(void);
    double foot = 0;
    task_vm_info_data_t vi;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) == KERN_SUCCESS)
        foot = (double)vi.phys_footprint / 1073741824.0;
    float ms = host_hud_ms;
    int bw = host_back_w, bh = host_back_h, ew = gl_engine_w, eh = gl_engine_h;
    NSString *res = (ew == bw && eh == bh) ? [NSString stringWithFormat:@"%dx%d native", ew, eh]
                    : host_mfx_active ? [NSString stringWithFormat:@"%dx%d -> %dx%d MetalFX (%.1f ms)", ew, eh, bw, bh,
                                                                   host_mfx_us / 1000.0]
                                      : [NSString stringWithFormat:@"%dx%d -> %dx%d stretch", ew, eh, bw, bh];
    hud_label.stringValue = [NSString
        stringWithFormat:
            @" %3.0f fps   %.1f ms   worst %.0f ms   GPU %.1f ms \n %@ \n memory %.1f GB   game heap %u / 3328 MB ",
            ms > 0 ? 1000.0 / ms : 0.0, ms, host_hud_worst, (double)host_gpu_ms, res, foot, rt_heap_used_mb()];
    [hud_label sizeToFit];
    NSRect c = [pw contentRectForFrameRect:pw.frame]; // screen coordinates
    NSSize sz = hud_label.frame.size;
    [hud_win setFrame:NSMakeRect(c.origin.x + 12, c.origin.y + c.size.height - sz.height - 12, sz.width, sz.height)
              display:YES];
}
// Create and attach the overlay, or remove it if it exists (main thread).
static void hud_toggle(void) {
    NSWindow *pw = game_view.window;
    if (!pw)
        return;
    if (hud_win) {
        [hud_timer invalidate];
        hud_timer = nil;
        [pw removeChildWindow:hud_win];
        [hud_win orderOut:nil];
        hud_win = nil;
        hud_label = nil;
        fprintf(stderr, "[hud] off\n");
        return;
    }
    hud_win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 300, 60)
                                          styleMask:NSWindowStyleMaskBorderless
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    hud_win.opaque = NO;
    hud_win.backgroundColor = [NSColor clearColor];
    hud_win.ignoresMouseEvents = YES;
    hud_win.hasShadow = NO;
    hud_win.releasedWhenClosed = NO;
    hud_win.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorIgnoresCycle;
    hud_label = [NSTextField labelWithString:@""];
    hud_label.font = [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightMedium];
    hud_label.textColor = [NSColor whiteColor];
    hud_label.drawsBackground = YES;
    hud_label.backgroundColor = [NSColor colorWithCalibratedWhite:0 alpha:0.6];
    hud_label.maximumNumberOfLines = 0;
    hud_win.contentView = hud_label;
    [pw addChildWindow:hud_win ordered:NSWindowAbove];
    hud_update();
    hud_timer = [NSTimer timerWithTimeInterval:0.5
                                       repeats:YES
                                         block:^(NSTimer *t) {
                                           hud_update();
                                         }];
    [[NSRunLoop mainRunLoop] addTimer:hud_timer forMode:NSRunLoopCommonModes];
    fprintf(stderr, "[hud] on (F12 toggles)\n");
}
static NSCursor *blank_cursor(void) {
    static NSCursor *c;
    if (!c) {
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(16, 16)];
        [img lockFocus];
        [[NSColor clearColor] set];
        NSRectFill(NSMakeRect(0, 0, 16, 16));
        [img unlockFocus];
        c = [[NSCursor alloc] initWithImage:img hotSpot:NSZeroPoint];
    }
    return c;
}
extern void rt_hid_key_event(uint16_t mac_keycode, int down);
extern void rt_hid_pump(CPU *c);
extern void rt_carbon_key(uint16_t keycode, uint8_t ch, uint32_t mods, int down);
extern void rt_carbon_mouse(uint32_t kind, float lx, float ly, float dx, float dy, int button, int clicks, int wheel,
                            uint32_t mods);
extern void rt_mouse_state(float lx, float ly, uint32_t buttons);
static float last_lx, last_ly; // last mouse position in engine coordinates (for button and wheel events)
// NSEvent modifier flags -> Carbon modifiers (cmdKey 0x100, shiftKey 0x200, alphaLock 0x400, optionKey 0x800, controlKey 0x1000)
static uint32_t carbon_mods(NSEventModifierFlags f) {
    return (f & NSEventModifierFlagCommand ? 0x100u : 0) | (f & NSEventModifierFlagShift ? 0x200u : 0) |
           (f & NSEventModifierFlagCapsLock ? 0x400u : 0) | (f & NSEventModifierFlagOption ? 0x800u : 0) |
           (f & NSEventModifierFlagControl ? 0x1000u : 0);
}
static BOOL scripted(void) {
    return getenv("RT32_INPUT") != NULL;
}
void host_show_cursor(int show) {
    if (game_wants_system_cursor == (BOOL)show)
        return;
    game_wants_system_cursor = (BOOL)show;
    dispatch_async(dispatch_get_main_queue(), ^{
      [game_view.window invalidateCursorRectsForView:game_view];
    });
}

@interface RT32View : NSOpenGLView
@property(strong) NSTimer *timer;
@end

@implementation RT32View
- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)prepareOpenGL {
    [super prepareOpenGL];
    GLint swap = 1;
    [[self openGLContext] setValues:&swap forParameter:NSOpenGLContextParameterSwapInterval];
    [[self openGLContext] makeCurrentContext];
    host_ctx = [[self openGLContext] CGLContextObj];
    fprintf(stderr, "[host] GL %s / %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    // Start the engine from the run loop, not from here: AppKit calls prepareOpenGL from -[NSOpenGLContext
    // setView:] with the context's CGL lock held, and engine Init can wait on its render thread, whose
    // CGLSetParameter/flush calls need that same lock (deadlock seen with a startup movie playing).
    [self performSelector:@selector(startEngine) withObject:nil afterDelay:0];
}

- (void)startEngine {
    [[self openGLContext] makeCurrentContext];
    if (!game32_start(gcpu)) {
        fprintf(stderr, "[host] the game failed to start\n");
        [NSApp terminate:nil];
        return;
    }
    float w = 0, h = 0;
    game32_window_size(gcpu, &w, &h);
    fprintf(stderr, "[host] engine started, window %gx%g\n", w, h);
    if (w > 0 && h > 0)
        [self fitWindowToEngineWidth:w height:h];
    [[self window] center];
    // Claude Fable - 2026-11-24: frame pacing. Default: a 60 Hz timer starts each game frame, so 60 fps is the ceiling
    // on any display and frames are not lined up with the screen's refresh. RT32_PACE=display lets the display start
    // the frames instead, through a display link asked for "anything from 30 fps up to the screen's maximum".
    // Measured once, 30 minutes fullscreen on a 48-120 Hz variable-refresh monitor with MetalFX presenting: the game
    // ran at up to 120 fps (99.7 average at low settings), 59% of frames were shown for exactly one refresh and 39%
    // landed between the fixed refresh steps, i.e. the display was following the game. Still opt-in until it has
    // been tried on more displays (a fixed 60 Hz screen, a laptop's built-in ProMotion screen).
    const char *pace = getenv("RT32_PACE");
    if (pace && !strcmp(pace, "display")) {
        if (@available(macOS 14.0, *)) {
            CADisplayLink *dl = [self displayLinkWithTarget:self selector:@selector(tick:)];
            float top = (float)(self.window.screen ?: [NSScreen mainScreen]).maximumFramesPerSecond;
            if (top < 60)
                top = 60;
            dl.preferredFrameRateRange = CAFrameRateRangeMake(30, top, top);
            host_pace_link = dl; // the range is refreshed in frameStep when the window moves to another screen
            host_pace_top = top;
            [dl addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
            fprintf(stderr, "[host] frame pacing: display link, up to %.0f fps on this screen\n", top);
            return;
        }
        fprintf(stderr, "[host] RT32_PACE=display needs macOS 14 or later; using the 60 Hz timer\n");
    }
    self.timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0
                                                  target:self
                                                selector:@selector(tick:)
                                                userInfo:nil
                                                 repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:self.timer forMode:NSEventTrackingRunLoopMode];
}

// Once the game renders on its own thread, run frames straight from the timer instead of through
// setNeedsDisplay: AppKit's layer display touches (and locks) the shared GL context on the main thread,
// which races the render thread (driver crash in glBlitFramebuffer) or deadlocks against it.
- (void)tick:(id)t { // called by the 60 Hz timer, or by the display link with RT32_PACE=display
    if (@available(macOS 14.0, *)) {
        if ([t isKindOfClass:[CADisplayLink class]]) { // how long the display says each refresh lasts
            CADisplayLink *dl = t;
            static double sum, lo, hi;
            static int n;
            double d = (dl.targetTimestamp - dl.timestamp) * 1000.0;
            if (!n || d < lo)
                lo = d;
            if (!n || d > hi)
                hi = d;
            sum += d;
            if (++n == 200) {
                fprintf(stderr,
                        "[link] display link, last 200 callbacks: refresh period %.1f ms on average (shortest %.1f, "
                        "longest %.1f)\n",
                        sum / n, lo, hi);
                n = 0;
                sum = 0;
            }
        }
    }
    if (host_gl_offthread)
        [self frameStep];
    else
        [self setNeedsDisplay:YES];
}

// Map the engine's resolution onto the real drawable (uniform scale, centered) and clear the bars.
- (void)updateSurfaceMapping {
    float ew = 0, eh = 0;
    game32_window_size(gcpu, &ew, &eh);
    if (ew < 1 || eh < 1)
        return;
    NSSize d = [self convertSizeToBacking:self.bounds.size];
    float s = (float)MIN(d.width / ew, d.height / eh);
    gl_win_scale = s;
    gl_win_off_x = (float)((d.width - ew * s) / 2);
    gl_win_off_y = (float)((d.height - eh * s) / 2);
    host_back_w = (int)d.width;
    host_back_h = (int)d.height;
    if (host_gl_offthread)
        return; // the render thread owns the context: no GL from here
    // clear the bars without disturbing GL state the engine has cached
    GLint fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    GLfloat cc[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST), cm[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    if (fb)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, (GLsizei)d.width, (GLsizei)d.height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (sc)
        glEnable(GL_SCISSOR_TEST);
    if (fb)
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb);
}

// Follow the game's own fullscreen setting (only when it changes, so the green button keeps working).
- (void)followEngineFullscreen {
    static int last = -1;
    int windowed = game32_windowed(gcpu);
    if (windowed == last)
        return;
    BOOL isFull = (self.window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (last != -1 && windowed == isFull)
        [self.window toggleFullScreen:nil];
    else if (last == -1 && !windowed && !isFull)
        [self.window toggleFullScreen:nil];
    last = windowed;
}

// The engine's resolution is in backing pixels: on a Retina display the window is half that in points.
// If it still does not fit the screen, scale it down (keeping aspect): the engine's viewport is mapped onto the
// smaller surface.
- (void)fitWindowToEngineWidth:(float)w height:(float)h {
    NSWindow *win = [self window];
    win.contentAspectRatio = NSMakeSize(w, h);
    if (win.styleMask & NSWindowStyleMaskFullScreen)
        return; // fullscreen: the mapping handles it
    CGFloat scale = win.backingScaleFactor > 0 ? win.backingScaleFactor : 1;
    NSSize pts = NSMakeSize(w / scale, h / scale);
    NSRect vis = (win.screen ?: [NSScreen mainScreen]).visibleFrame;
    NSRect frame = [win frameRectForContentRect:NSMakeRect(0, 0, pts.width, pts.height)];
    CGFloat chrome = frame.size.height - pts.height;
    CGFloat k = MIN(1.0, MIN(vis.size.width / pts.width, (vis.size.height - chrome) / pts.height));
    pts.width *= k;
    pts.height *= k;
    [win setContentSize:pts];
    if (k < 1)
        fprintf(stderr, "[host] %gx%g does not fit the screen; window scaled by %.2f\n", w, h, k);
    [win center];
    [self
        update]; // Claude Fable - 2026-11-24: tell the GL context its surface changed size now (locked override below), not at some later moment
    [self updateTrackingAreas];
    [win invalidateCursorRectsForView:self];
}

// ---- mouse ----
extern volatile int rt_mouse_detached;
// Mouse look: macOS does not reliably keep the pointer frozen while it is detached from the mouse (the game sets
// that from its own thread), so the hidden pointer drifts off the window and a click then lands on another app,
// which deactivates the game (pause menu) and makes menu clicks miss. While detached and active, pin the pointer
// to the window centre; movement still arrives through the event deltas.
static void confine_pointer(NSWindow *w) {
    if (!rt_mouse_detached || !w || ![w isKeyWindow] || ![NSApp isActive])
        return;
    static BOOL once;
    if (!once) {
        once = YES;
        CGEventSourceRef src = CGEventSourceCreate(kCGEventSourceStateCombinedSessionState);
        if (src) {
            CGEventSourceSetLocalEventsSuppressionInterval(src, 0.0);
            CFRelease(src);
        }
    }
    NSRect f = [w frame];
    NSScreen *s0 = [[NSScreen screens] firstObject];
    if (NSPointInRect([NSEvent mouseLocation], NSInsetRect(f, f.size.width / 4, f.size.height / 4)))
        return;                                                           // still well inside
    CGPoint ctr = CGPointMake(NSMidX(f), NSMaxY([s0 frame]) - NSMidY(f)); // CG global coordinates: top-left origin
    CGWarpMouseCursorPosition(ctr);
    CGAssociateMouseAndMouseCursorPosition(false);
}
- (void)moveTo:(NSEvent *)e {
    if (scripted())
        return;
    confine_pointer([self window]);
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    NSPoint px = [self convertPointToBacking:p]; // engine coordinates are backing pixels
    NSSize sz = [self convertSizeToBacking:self.bounds.size];
    float lx = (float)((px.x - gl_win_off_x) / gl_win_scale),
          ly = (float)((sz.height - px.y - gl_win_off_y) / gl_win_scale);
    game32_mouse_move(gcpu, lx, ly);
    NSEventType t = [e type];
    if (t == NSEventTypeMouseMoved || t == NSEventTypeLeftMouseDragged || t == NSEventTypeRightMouseDragged ||
        t == NSEventTypeOtherMouseDragged)
        rt_carbon_mouse(t == NSEventTypeMouseMoved ? 5 : 6, lx, ly, (float)[e deltaX], (float)[e deltaY],
                        (int)[e buttonNumber], 0, 0, carbon_mods([e modifierFlags]));
    last_lx = lx;
    last_ly = ly;
    rt_mouse_state(lx, ly, (uint32_t)[NSEvent pressedMouseButtons] & 7);
}
- (void)mouseMoved:(NSEvent *)e {
    [self moveTo:e];
}
- (void)mouseDragged:(NSEvent *)e {
    [self moveTo:e];
}
- (void)rightMouseDragged:(NSEvent *)e {
    [self moveTo:e];
}
- (void)otherMouseDragged:(NSEvent *)e {
    [self moveTo:e];
}
- (void)button:(int)b down:(BOOL)down event:(NSEvent *)e {
    if (scripted())
        return;
    [self moveTo:e];
    if (down && capture_armed) {
        capture_armed = 0;
        host_capture_request = 1;
    }
    game32_mouse_button(gcpu, b, down, (int)[e clickCount]);
    rt_carbon_mouse(down ? 1 : 2, last_lx, last_ly, 0, 0, b, (int)[e clickCount], 0, carbon_mods([e modifierFlags]));
    {
        uint32_t bit = b == 0 ? 1 : b == 1 ? 2 : 4;
        static uint32_t btn;
        btn = down ? (btn | bit) : (btn & ~bit);
        rt_mouse_state(last_lx, last_ly, btn);
    }
}
- (void)mouseDown:(NSEvent *)e {
    [self button:0 down:YES event:e];
}
- (void)mouseUp:(NSEvent *)e {
    [self button:0 down:NO event:e];
}
- (void)rightMouseDown:(NSEvent *)e {
    [self button:1 down:YES event:e];
}
- (void)rightMouseUp:(NSEvent *)e {
    [self button:1 down:NO event:e];
}
- (void)otherMouseDown:(NSEvent *)e {
    [self button:2 down:YES event:e];
}
- (void)otherMouseUp:(NSEvent *)e {
    [self button:2 down:NO event:e];
}
- (void)scrollWheel:(NSEvent *)e {
    if (scripted())
        return;
    float d = (float)[e deltaY];
    if ((int)d) {
        game32_wheel(gcpu, (int)d);
        rt_carbon_mouse(10, last_lx, last_ly, 0, 0, 0, 0, (int)d, carbon_mods([e modifierFlags]));
    }
}
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea *t in self.trackingAreas)
        [self removeTrackingArea:t];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds
                                                       options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                                                               NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                                         owner:self
                                                      userInfo:nil]];
}
// Games of this era draw their own cursor: over the game view the macOS pointer is invisible, unless the game
// asked for the system cursor (host_show_cursor). macOS restores the pointer outside the view.
- (void)resetCursorRects {
    [self addCursorRect:self.bounds cursor:game_wants_system_cursor ? [NSCursor arrowCursor] : blank_cursor()];
}

// ---- keyboard ----
- (uint32_t)keyChar:(NSEvent *)e {
    NSString *s = [e charactersIgnoringModifiers];
    return s.length ? (uint32_t)[s characterAtIndex:0] : 0;
}
- (void)keyDown:(NSEvent *)e {
    if (scripted() || [e isARepeat])
        return;
    if ([e keyCode] == 111) {
        hud_toggle();
        return;
    } // F12: performance overlay
    if ([e keyCode] == 109) { // Claude Fable - 2026-11-25: F10 saves the next frames as images (see present_burst)
        host_burst_req = 1;
        if (getenv("RT32_DIAG_FILM_BOX")) // also run the per-draw box capture (gl.c) on the next sampled frames
            host_capture_request = 1;
        return;
    }
    if ([e keyCode] == 101) {
        host_mfx_off = !host_mfx_off;
        fprintf(stderr, "[mfx] F9: switched to %s at present #%d\n", host_mfx_off ? "PLAIN STRETCH" : "METALFX",
                host_present_count);
        return;
    }
    if ([e keyCode] == 42 && getenv("RT32_DUMP_TARGETS")) {
        capture_armed = 1;
        fprintf(stderr, "[host] capture armed (next Return or click)\n");
        NSBeep();
        return;
    } // backslash: arm capture
    if (capture_armed && [e keyCode] == 36) {
        capture_armed = 0;
        host_capture_request = 1;
    }
    rt_hid_key_event([e keyCode], 1);
    {
        NSString *s = [e characters];
        rt_carbon_key([e keyCode], s.length ? (uint8_t)[s characterAtIndex:0] : 0, carbon_mods([e modifierFlags]), 1);
    }
    game32_key(gcpu, 1, [self keyChar:e], (uint32_t)[e modifierFlags]);
}
- (void)keyUp:(NSEvent *)e {
    if (scripted())
        return;
    rt_hid_key_event([e keyCode], 0);
    {
        NSString *s = [e characters];
        rt_carbon_key([e keyCode], s.length ? (uint8_t)[s characterAtIndex:0] : 0, carbon_mods([e modifierFlags]), 0);
    }
    game32_key(gcpu, 0, [self keyChar:e], (uint32_t)[e modifierFlags]);
}
- (void)flagsChanged:(NSEvent *)e {
    if (scripted())
        return;
    static BOOL was[3];
    NSEventModifierFlags f = [e modifierFlags];
    const NSEventModifierFlags mask[3] = {NSEventModifierFlagShift, NSEventModifierFlagControl,
                                          NSEventModifierFlagOption};
    for (int i = 0; i < 3; i++) {
        BOOL now = (f & mask[i]) != 0;
        if (now != was[i]) {
            game32_modifier(gcpu, i, now);
            was[i] = now;
            rt_hid_key_event((uint16_t)[e keyCode], now);
        }
    }
}

// AppKit calls these on the main thread whenever the surface changes. Serialise with the render thread's GL calls (Claude Fable - 2026-11-24).
- (void)update {
    if (host_ctx)
        CGLLockContext(host_ctx);
    [super update];
    if (host_ctx)
        CGLUnlockContext(host_ctx);
}
- (void)reshape {
    if (host_ctx)
        CGLLockContext(host_ctx);
    [super reshape];
    if (host_ctx)
        CGLUnlockContext(host_ctx);
}
- (void)drawRect:(NSRect)r {
    if (!host_gl_offthread)
        [self frameStep];
}
- (void)frameStep {
    if (!host_gl_offthread)
        [[self openGLContext] makeCurrentContext];
    { // tell the game when its window gains or loses focus (it enables mouse input only while active)
        extern void rt_carbon_window(int active);
        static int was = -1;
        // Claude Fable - 2026-11-24: a scripted run (RT32_INPUT) counts as focused, so unattended measurement runs behave
        // the same whether or not macOS let the window come to the front.
        int now = scripted() || ([NSApp isActive] && [[self window] isKeyWindow]);
        if (now != was) {
            rt_carbon_window(now);
            was = now;
        }
    }
    if (game32_quit_requested(gcpu)) {
        [NSApp terminate:nil];
        return;
    }
    { // follow resolution changes made in the game's settings
        float w = 0, h = 0;
        game32_window_size(gcpu, &w, &h);
        static float lw, lh;
        if (w >= 320 && h >= 200 && (w != lw || h != lh)) {
            lw = w;
            lh = h;
            fprintf(stderr, "[host] engine resolution -> %gx%g\n", w, h);
            [self fitWindowToEngineWidth:w height:h];
        }
    }
    {
        static int k;
        if ((k++ % 60) == 0) { // keep the refresh step of the window's current screen up to date
            NSScreen *sc = self.window.screen ?: [NSScreen mainScreen];
            if (@available(
                    macOS 12.0, *)) { // Claude Fable - 2026-11-25: NSScreen's refresh interval exists from macOS 12
                if (sc.minimumRefreshInterval > 0)
                    host_screen_step_ms = (float)(sc.minimumRefreshInterval * 1000.0);
            }
            if (@available(macOS 14.0, *)) {
                float top = (float)sc.maximumFramesPerSecond;
                if (top < 60)
                    top = 60;
                if (host_pace_link && top != host_pace_top) { // moved to a faster or slower screen
                    ((CADisplayLink *)host_pace_link).preferredFrameRateRange = CAFrameRateRangeMake(30, top, top);
                    host_pace_top = top;
                    fprintf(stderr, "[host] frame pacing: now up to %.0f fps (%s)\n", top, sc.localizedName.UTF8String);
                }
            }
        }
    }
    [self updateSurfaceMapping];
    [self followEngineFullscreen];
    rt_hid_pump(gcpu); // keyboard as HID values, for games that read IOHIDManager
    game32_frame(gcpu);
    if (game32_quit_requested(gcpu))
        [NSApp terminate:nil];
    [self afterFrame];
}

- (void)scriptedInput:(int)frame {
    const char *spec = getenv("RT32_INPUT");
    if (!spec)
        return;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *save = NULL, *t = strtok_r(buf, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
        int f = atoi(t);
        char *act = strchr(t, ':');
        if (f != frame || !act)
            continue;
        act++;
        if (!strncmp(act, "move:", 5)) {
            float x = 0, y = 0;
            sscanf(act + 5, "%f,%f", &x, &y);
            game32_mouse_move(gcpu, x, y);
        } else if (!strcmp(act, "down"))
            game32_mouse_button(gcpu, 0, 1, 1);
        else if (!strcmp(act, "up"))
            game32_mouse_button(gcpu, 0, 0, 1);
        else if (!strcmp(act, "click")) {
            game32_mouse_button(gcpu, 0, 1, 1);
            game32_mouse_button(gcpu, 0, 0, 1);
        } else if (!strncmp(act, "cmove:",
                            6)) { // Carbon mouse move to engine coordinates x,y (with the delta from the last one)
            float x = 0, y = 0;
            sscanf(act + 6, "%f,%f", &x, &y);
            rt_carbon_mouse(5, x, y, x - last_lx, y - last_ly, 0, 0, 0, 0);
            last_lx = x;
            last_ly = y;
            rt_mouse_state(x, y, 0);
        } else if (!strncmp(act, "cclick", 6)) {
            rt_carbon_mouse(1, last_lx, last_ly, 0, 0, 0, 1, 0, 0);
            rt_carbon_mouse(2, last_lx, last_ly, 0, 0, 0, 1, 0, 0);
        } else if (!strncmp(act,
                            "hidkey:", 7)) { // macOS virtual key code: held down for 6 frames (pollers need to see it)
            static uint16_t held_k;
            held_k = (uint16_t)strtoul(act + 7, NULL, 0);
            rt_hid_key_event(held_k, 1);
            uint8_t ch = held_k == 36 ? 13 : held_k == 49 ? ' ' : held_k == 53 ? 27 : 0;
            rt_carbon_key(held_k, ch, 0, 1);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
              rt_hid_key_event(held_k, 0);
              rt_carbon_key(held_k, ch, 0, 0);
            });
        } else if (!strncmp(act, "pad:", 4)) { // controller buttons (XInput bits, e.g. 0x1000 = A) held for 12 polls
            host_pad_script((uint16_t)strtoul(act + 4, NULL, 0), 12);
        } else if (!strncmp(act, "key:", 4)) {
            uint32_t k = (uint32_t)(act[4] == '#' ? strtoul(act + 5, NULL, 0) : (uint8_t)act[4]);
            game32_key(gcpu, 1, k, 0);
            game32_key(gcpu, 0, k, 0);
        }
        fprintf(stderr, "[host] frame %d: input %s\n", frame, act);
    }
}

- (void)afterFrame {
    static int frame = 0;
    frame++;
    [self scriptedInput:frame];
    if (getenv("RT32_TRACE_GL"))
        fprintf(stderr, "[host] ---- end of frame %d ----\n", frame);
    const char *shots = getenv("RT32_SHOT");
    if (shots) {
        char key[32];
        snprintf(key, sizeof key, ",%d,", frame);
        char list[512];
        snprintf(list, sizeof list, ",%s,", shots);
        if (strstr(list, key)) {
            NSSize sz = [self convertSizeToBacking:self.bounds.size];
            int w = (int)sz.width, h = (int)sz.height;
            unsigned char *px = malloc((size_t)w * h * 4);
            glReadBuffer(GL_FRONT);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
            NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                                            pixelsWide:w
                                                                            pixelsHigh:h
                                                                         bitsPerSample:8
                                                                       samplesPerPixel:4
                                                                              hasAlpha:YES
                                                                              isPlanar:NO
                                                                        colorSpaceName:NSDeviceRGBColorSpace
                                                                           bytesPerRow:w * 4
                                                                          bitsPerPixel:32];
            for (int y = 0; y < h; y++) { // flip and force opaque
                unsigned char *dst = rep.bitmapData + (size_t)y * w * 4, *src = px + (size_t)(h - 1 - y) * w * 4;
                memcpy(dst, src, (size_t)w * 4);
                for (int x = 0; x < w; x++)
                    dst[4 * x + 3] = 255;
            }
            NSString *path = [NSString stringWithFormat:@"shot_%d.png", frame];
            NSString *dir = getenv("RT32_SHOT_DIR") ? @(getenv("RT32_SHOT_DIR")) : @".";
            [[rep representationUsingType:NSBitmapImageFileTypePNG
                               properties:@{}] writeToFile:[dir stringByAppendingPathComponent:path] atomically:YES];
            fprintf(stderr, "[host] wrote %s (%dx%d)\n", path.UTF8String, w, h);
            free(px);
        }
    }
    const char *ex = getenv("RT32_EXIT_AFTER");
    if (ex && frame >= atoi(ex)) {
        fprintf(stderr, "[host] exiting after %d frames\n", frame);
        exit(0);
    }
}
@end

@interface RT32AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(strong) NSWindow *window;
@end
@implementation RT32AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    NSOpenGLPixelFormatAttribute attrs[] = {NSOpenGLPFAOpenGLProfile,
                                            game32_gl_profile ? (NSOpenGLPixelFormatAttribute)game32_gl_profile
                                                              : NSOpenGLProfileVersionLegacy,
                                            NSOpenGLPFADoubleBuffer,
                                            NSOpenGLPFAAccelerated,
                                            NSOpenGLPFAColorSize,
                                            24,
                                            NSOpenGLPFAAlphaSize,
                                            8,
                                            NSOpenGLPFADepthSize,
                                            24,
                                            NSOpenGLPFAStencilSize,
                                            8,
                                            0};
    NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    NSRect frame =
        NSMakeRect(0, 0, rt32_game.width > 0 ? rt32_game.width : 1024, rt32_game.height > 0 ? rt32_game.height : 768);
    self.window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.window.title = @(rt32_game.name ? rt32_game.name : "Game");
    self.window.delegate = self;
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    self.window.styleMask |= NSWindowStyleMaskResizable;
    RT32View *v = [[RT32View alloc] initWithFrame:frame pixelFormat:pf];
    v.wantsBestResolutionOpenGLSurface = YES; // full-resolution GL surface; engine sizes are backing pixels
    self.window.contentView = v;
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:v];
    game_view = v;
    mfx_make_view(v);
    {
        const char *h = getenv("RT32_HUD");
        if (h && atoi(h))
            hud_toggle();
    }
    self.window.acceptsMouseMovedEvents = YES;
    [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a {
    return YES;
}
@end

// The user's copy of the original Mac game. Remembered once chosen.
static NSString *bundle_exe(NSString *app) {
    return
        [app stringByAppendingPathComponent:[@"Contents/MacOS" stringByAppendingPathComponent:@(rt32_game.exe_name)]];
}
static BOOL valid_game(NSString *p) {
    return [[NSFileManager defaultManager] fileExistsAtPath:bundle_exe(p)];
}
static NSString *find_game(void) {
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    NSString *saved = [ud stringForKey:@"GamePath"];
    if (saved && valid_game(saved))
        return saved;
    if (rt32_game.steam_folder) {
        NSString *rel = [NSString stringWithFormat:@"Library/Application Support/Steam/steamapps/common/%s/%s",
                                                   rt32_game.steam_folder, rt32_game.app_bundle];
        NSString *steam = [NSHomeDirectory() stringByAppendingPathComponent:rel];
        if (valid_game(steam)) {
            [ud setObject:steam forKey:@"GamePath"];
            return steam;
        }
    }
    [NSApplication sharedApplication];
    NSAlert *a = [NSAlert new];
    a.messageText = [NSString stringWithFormat:@"Locate %s", rt32_game.name];
    a.informativeText =
        [NSString stringWithFormat:@"Choose %s from your installation of %s.", rt32_game.app_bundle, rt32_game.name];
    [a runModal];
    NSOpenPanel *op = [NSOpenPanel openPanel];
    op.canChooseDirectories = NO;
    op.canChooseFiles = YES;
    op.treatsFilePackagesAsDirectories = NO;
    op.allowedFileTypes = @[ @"app" ];
    if ([op runModal] != NSModalResponseOK)
        return nil;
    NSString *p = op.URL.path;
    if (!valid_game(p)) {
        NSAlert *e = [NSAlert new];
        e.messageText = [NSString stringWithFormat:@"That is not the Mac version of %s.", rt32_game.name];
        [e runModal];
        return nil;
    }
    [ud setObject:p forKey:@"GamePath"];
    return p;
}

// Claude Fable - 2026-11-24: version check. A recompiled port only fits the exact executable it was generated from: the
// translated code, the function list and every hard-coded address are positions inside that file. With any other
// build (another store's version, a patch) it would start and then fail in confusing ways, so compare the file's
// SHA-256 with the one the game project declares (rt32_game.exe_sha256) and stop with a clear message if it differs.
// Returns 1 to continue. RT32_SKIP_VERSION_CHECK=1 skips it (for experiments with other builds).
static int check_game_binary(const char *path, BOOL interactive) {
    if (!rt32_game.exe_sha256 || getenv("RT32_SKIP_VERSION_CHECK"))
        return 1;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 1; // not readable: the loader reports that in its own words
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    static unsigned char buf[1 << 20];
    size_t n;
    unsigned long long total = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
        total += n;
    }
    fclose(f);
    unsigned char md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(md, &ctx);
    char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
        snprintf(hex + 2 * i, 3, "%02x", md[i]);
    const char *ver = rt32_game.exe_version ? rt32_game.exe_version : rt32_game.name;
    if (!strcasecmp(hex, rt32_game.exe_sha256)) {
        fprintf(stderr, "[host] game executable verified: %s (sha256 %.16s..., %llu bytes)\n", ver, hex, total);
        return 1;
    }
    fprintf(stderr,
            "[host] WRONG GAME VERSION: %s\n[host]   this port was built for: %s\n[host]   expected sha256 %s\n"
            "[host]   found    sha256 %s (%llu bytes)\n[host]   RT32_SKIP_VERSION_CHECK=1 starts it anyway (it will "
            "very likely crash)\n",
            path, ver, rt32_game.exe_sha256, hex, total);
    if (interactive) {
        [NSApplication sharedApplication];
        NSAlert *a = [NSAlert new];
        a.alertStyle = NSAlertStyleCritical;
        a.messageText = [NSString stringWithFormat:@"This copy of %s is a different version", rt32_game.name];
        a.informativeText = [NSString
            stringWithFormat:@"This port only works with one exact build of the game:\n%s\n\nThe copy at\n%s\nis a "
                             @"different build (another store's version, or a newer or older patch), so the port "
                             @"cannot run it.\n\nDetails are in the log file.",
                             ver, path];
        [a addButtonWithTitle:@"Quit"];
        [a runModal];
        [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"GamePath"]; // ask again next time
    }
    return 0;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        const char *bin = argc > 2 && argv[1][0] != '-' ? argv[1] : NULL;
        const char *res = argc > 2 && argv[1][0] != '-' ? argv[2] : NULL;
        static char binbuf[2048];
        if (!bin || !res) {
            // Launched as an app: log to ~/Library/Logs and locate the user's copy of the game.
            if (!isatty(2)) {
                NSString *log = [NSHomeDirectory()
                    stringByAppendingPathComponent:[NSString stringWithFormat:@"Library/Logs/%s.log",
                                                                              rt32_game.log_name ? rt32_game.log_name
                                                                                                 : "recomp32"]];
                freopen(log.UTF8String, "w", stderr);
                freopen(log.UTF8String, "a", stdout);
                setvbuf(stdout, NULL, _IONBF, 0);
                setvbuf(stderr, NULL, _IONBF, 0); // a freeze must not lose the last lines
            }
            NSString *game = find_game();
            if (!game)
                return 1;
            snprintf(binbuf, sizeof binbuf, "%s", bundle_exe(game).UTF8String);
            snprintf(resources_dir, sizeof resources_dir, "%s/Contents/Resources", game.UTF8String);
            bin = binbuf;
        } else
            snprintf(resources_dir, sizeof resources_dir, "%s", res);
        if (!check_game_binary(bin, argc <= 2 || argv[1][0] == '-'))
            return 1;
        gcpu = rt_init(bin);
        if (chdir(resources_dir)) {
            perror(resources_dir);
            return 1;
        }
        // Steam games ship steam_appid.txt next to the executable
        char p[2048];
        snprintf(p, sizeof p, "%s/../MacOS/steam_appid.txt", resources_dir);
        FILE *f = fopen(p, "r");
        unsigned id = 0;
        if (f) {
            if (fscanf(f, "%u", &id) != 1)
                id = 0;
            fclose(f);
        }
        steam_set_app_id(id);
        for (int i = 0; i < ngmodinit; i++) {
            uint32_t a[4] = {0, 0, 0, 0};
            guest_call(gcpu, gmodinit[i], 4, a);
        }

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        RT32AppDelegate *d = [RT32AppDelegate new];
        app.delegate = d;
        [app run];
    }
    return 0;
}
