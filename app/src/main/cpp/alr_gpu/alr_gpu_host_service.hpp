// ALR GPU host-side executor service (Phase 4 / GPU-native Linux app track, M5 backbone).
//
// This is the REUSABLE host backbone that turns the proven M1/M2/M4 pieces — the
// command-stream decoder (alr_gpu_decode.hpp), the SPSC command ring
// (alr_gpu_ring.hpp), and the AHB-backed FBO render target (alr_gpu_fbo.hpp) —
// into a live, continuously-running guest -> GPU -> AHB -> screen pipeline.
//
// `GpuExecutorService` owns the CONSUMER side of the ring on its own dedicated
// thread. That thread (and ONLY that thread) creates and uses the EGL context +
// AHB-FBO, because a GL context is thread-affine: it must be created, made
// current, used, and destroyed all on the same thread. The guest/producer side
// touches only the ring (no GL). Each frame the producer flushes (eglSwapBuffers-
// equivalent), the consumer decodes the accumulated op batch into the AHB-FBO,
// glFinish()es, invokes the caller's present callback with the rendered
// AhbRenderTarget (so the caller may hand that same AHB to the WaylandPresenter
// zero-copy, or read it back), then releases the producer via the ring's
// reply_seq handshake.
//
// `run_live_integration_probe()` is the in-process, two-thread device self-test:
// a real producer thread pushes the M1 triangle op stream through the ring for N
// frames while the executor renders each into an AHB on Mali and a present
// callback glReadPixels-verifies the result — proving the full multi-frame live
// handshake end to end, with no fork and no shared-memory fd (a plain malloc'd
// ring both threads see). The gating first line is `ALR GPU LIVE INTEGRATION:
// PASS/FAIL`, which MainActivity greps.
//
// Header-only and self-contained (public EGL/GLES2 APIs only) so it can be
// #included from runtime_report.cpp with a one-line add, keeping all GPU work in
// the alr_gpu TU while the concurrent PC-gate session owns that .cpp file.

#ifndef ALR_GPU_ALR_GPU_HOST_SERVICE_HPP
#define ALR_GPU_ALR_GPU_HOST_SERVICE_HPP

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "alr_gpu/alr_gpu_decode.hpp"
#include "alr_gpu/alr_gpu_fbo.hpp"
#include "alr_gpu/alr_gpu_probe.hpp"
#include "alr_gpu/alr_gpu_ring.hpp"

namespace alr::gpu {

// ===========================================================================
// GpuExecutorService — the reusable host executor backbone.
//
// Owns the ring CONSUMER and a dedicated GL thread. The caller hands it a ring
// region it has already ring_init'd (so the producer — guest or self-test — can
// be wired independently), the framebuffer dimensions, and a present callback
// invoked once per completed frame ON the consumer thread, after glFinish, so the
// callback may safely issue GL / glReadPixels on the same live context.
//
// OPTIONAL on-screen present (M5): if a non-null `window` (an ANativeWindow) is
// supplied, the GL thread ALSO creates a second EGL context bound to a window
// surface for that ANativeWindow, plus a samplerExternalOES "blit" program. After
// each frame's decode-into-AHB + glFinish, it makes the WINDOW context current,
// imports the just-rendered AHB as a GL_TEXTURE_EXTERNAL_OES texture, draws a
// full-screen quad sampling it, and eglSwapBuffers — putting the rendered frame on
// the screen zero-copy. When `window` is nullptr (the default) NONE of that exists
// and the service behaves EXACTLY as the pbuffer-only original (so
// run_live_integration_probe is byte-for-byte unchanged).
//
// Why two contexts, both on the one GL thread: Mali returns BLACK if you sample an
// AHB via external-OES in the SAME EGL context that just rendered into it as an
// FBO color attachment (tile-resolve / same-AHB read-after-write hazard, observed
// at v117; v114 proved a DIFFERENT context samples it fine). So the decode-into-AHB
// runs in the pbuffer context and the external-OES present runs in the window
// context. A GL context is thread-affine, so both live on this single thread and we
// eglMakeCurrent between them once per frame. They share an EGL share group so the
// AHB-import path is identical; only the render-target sampling crosses the context
// boundary, which is what defeats the hazard.
// ===========================================================================
class GpuExecutorService {
public:
    using PresentFn = std::function<void(const AhbRenderTarget&)>;

    // `ring_region` must already be ring_init'd (size == ring_region_size of the
    // chosen ring_bytes). `region_bytes` is the TOTAL region size (header + data),
    // kept for symmetry / future bounds use. `present` may be empty. `window` is
    // OPTIONAL: when non-null the GL thread additionally presents each rendered AHB
    // to that ANativeWindow (see class comment); when nullptr (default) the service
    // is pbuffer-only and identical to the original — preserving every existing call
    // site (e.g. run_live_integration_probe) unchanged.
    //
    // `doorbell_fd` is OPTIONAL: an inherited eventfd the producer (the real guest
    // shim) signals on flush. When >= 0 the idle wait blocks briefly on it so a guest
    // frame wakes the consumer promptly instead of spin-yielding a core (zero-overhead
    // goal); when -1 (default — both in-process probes pass -1) the idle wait is the
    // original spin-yield, so those probes stay byte-for-byte unchanged. The executor
    // only reads/drains the fd; it does NOT own or close it (the creator owns it).
    GpuExecutorService(void* ring_region, size_t region_bytes, int fb_w, int fb_h,
                       PresentFn present, ANativeWindow* window = nullptr,
                       int doorbell_fd = -1)
        : region_(ring_region),
          fb_w_(fb_w),
          fb_h_(fb_h),
          present_(std::move(present)),
          window_(window),
          doorbell_fd_(doorbell_fd) {
        // region_bytes is accepted for API symmetry (the data ring size lives in the
        // RingHeader the caller already ring_init'd); the consumer derives all bounds
        // from the header, so we don't store it.
        (void)region_bytes;
    }

    ~GpuExecutorService() { stop(); }

    GpuExecutorService(const GpuExecutorService&) = delete;
    GpuExecutorService& operator=(const GpuExecutorService&) = delete;

    // Spawn the consumer/GL thread. Returns true once the thread has signalled it
    // is ready (EGL + AHB-FBO created). Returns false (with error() populated) if
    // GL/EGL setup failed on the thread or the ring is invalid. Idempotent-safe:
    // calling start() twice without stop() is a no-op returning the prior result.
    bool start() {
        if (thread_.joinable()) return !setup_failed_.load(std::memory_order_acquire);
        if (region_ == nullptr || !ring_valid(region_)) {
            error_ = "ring-invalid";
            setup_failed_.store(true, std::memory_order_release);
            return false;
        }
        stop_.store(false, std::memory_order_release);
        ready_.store(false, std::memory_order_release);
        setup_failed_.store(false, std::memory_order_release);
        thread_ = std::thread([this] { thread_main(); });
        // Wait for the thread to finish EGL/AHB setup (success or failure).
        while (!ready_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return !setup_failed_.load(std::memory_order_acquire);
    }

    // Signal the thread to stop, unblock any waiting producer (mark the ring
    // closed so flush_and_wait returns), and join. GL teardown happens INSIDE the
    // thread before it exits (context is thread-affine). Safe to call repeatedly.
    void stop() {
        stop_.store(true, std::memory_order_release);
        if (region_ != nullptr && ring_valid(region_)) {
            // Release a producer blocked in flush_and_wait().
            static_cast<RingHeader*>(region_)->closed.store(1, std::memory_order_release);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    uint32_t frames_presented() const {
        return frames_presented_.load(std::memory_order_acquire);
    }
    const std::string& error() const { return error_; }
    const std::string& renderer_string() const { return renderer_; }
    bool software() const { return software_.load(std::memory_order_acquire); }
    // True once the optional window context + present program were created (only
    // meaningful when a non-null window was passed). Read after start() returns.
    bool window_ready() const { return window_ready_.load(std::memory_order_acquire); }

private:
    // ---- Optional on-screen present: window EGL context + external-OES blit. ----
    // Declared before the methods that take it by reference. GL handles live in the
    // WINDOW context (shared group), so a single full-screen triangle program + the
    // AHB-import proc is all the per-frame present needs.
    struct WinPresent {
        EGLSurface surf = EGL_NO_SURFACE;
        EGLContext ctx = EGL_NO_CONTEXT;
        GLuint program = 0;     // samplerExternalOES full-screen blit program
        GLint a_pos = -1;       // vec2 attribute (NDC position)
        GLint u_tex = -1;       // samplerExternalOES uniform
        GLuint ext_tex = 0;     // GL_TEXTURE_EXTERNAL_OES the AHB is imported into
        PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_image_target = nullptr;
    };

    // ---- The consumer/GL thread. ALL EGL/GLES happens here. ----
    void thread_main() {
        // 1) EGL display + GLES2 context (surfaceless via a 1x1 pbuffer, same style
        //    as run_fbo_present_probe / run_draw_probe). The render target is the
        //    AHB-FBO, so the draw surface only needs to make a context current.
        EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (dpy == EGL_NO_DISPLAY || eglInitialize(dpy, nullptr, nullptr) != EGL_TRUE) {
            fail_setup("egl-init " + egl_err_hex_local());
            return;
        }
        const EGLint cfg_attribs[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE,
        };
        EGLConfig cfg = nullptr;
        EGLint nc = 0;
        if (eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &nc) != EGL_TRUE || nc < 1) {
            fail_setup("choose-config " + egl_err_hex_local());
            eglTerminate(dpy);
            return;
        }
        const EGLint pb[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
        const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
        if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
            eglMakeCurrent(dpy, surf, surf, ctx) != EGL_TRUE) {
            fail_setup("make-current " + egl_err_hex_local());
            if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
            if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
            eglTerminate(dpy);
            return;
        }

        // OPTIONAL on-screen present: a SECOND EGL context bound to a window surface
        // for `window_`, sharing this pbuffer context's object space. Sampling the
        // AHB here (not in the pbuffer ctx) sidesteps the Mali same-context black-AHB
        // hazard. If anything fails we tear the window pieces down and fall back to
        // pbuffer-only (still a valid headless run), recording the reason in error_.
        WinPresent win;  // all zero / EGL_NO_* when no window or on setup failure
        if (window_ != nullptr) {
            // A window-renderable config (window surfaces need EGL_WINDOW_BIT).
            const EGLint win_cfg_attribs[] = {
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                EGL_NONE,
            };
            EGLConfig wcfg = nullptr;
            EGLint wn = 0;
            if (eglChooseConfig(dpy, win_cfg_attribs, &wcfg, 1, &wn) == EGL_TRUE && wn >= 1) {
                win.surf = eglCreateWindowSurface(dpy, wcfg, window_, nullptr);
                // Share with the pbuffer context so the AHB-import GL objects live in
                // one namespace; only the render-target SAMPLING crosses the boundary.
                win.ctx = eglCreateContext(dpy, wcfg, ctx, ctx_attribs);
            }
            // The AHB->external-OES import proc (does not need a current context).
            win.p_image_target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
                eglGetProcAddress("glEGLImageTargetTexture2DOES"));
            if (win.surf == EGL_NO_SURFACE || win.ctx == EGL_NO_CONTEXT) {
                error_ = "window-egl " + egl_err_hex_local();
            } else if (win.p_image_target == nullptr) {
                error_ = "no-glEGLImageTargetTexture2DOES";
            } else if (eglMakeCurrent(dpy, win.surf, win.surf, win.ctx) != EGL_TRUE ||
                       !win_present_setup(win)) {
                if (error_.empty()) error_ = "window-present-setup " + egl_err_hex_local();
                win_present_teardown(dpy, win);  // window ctx is current here
            } else {
                // Window present is fully wired; do not enter the teardown path below.
                window_ready_.store(true, std::memory_order_release);
            }
            // If setup did not fully succeed, drop any partial window state so the run
            // proceeds headless (pbuffer-only) — still a valid headless render.
            if (!window_ready_.load(std::memory_order_acquire)) {
                if (win.ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, win.ctx);
                if (win.surf != EGL_NO_SURFACE) eglDestroySurface(dpy, win.surf);
                win = WinPresent{};
            }
            // Return to the pbuffer (render) context for AHB-FBO creation below.
            eglMakeCurrent(dpy, surf, surf, ctx);
        }

        // Capture renderer identity / software-ness once, now that a context is current.
        {
            std::string vendor, rr;
            if (const auto* v = reinterpret_cast<const char*>(glGetString(GL_VENDOR))) vendor = v;
            if (const auto* g = reinterpret_cast<const char*>(glGetString(GL_RENDERER))) rr = g;
            renderer_ = rr;
            software_.store(renderer_software_local(vendor, rr), std::memory_order_release);
        }

        // 2) AHB-backed render target (allocate + import + FBO), and verify complete.
        //    ahb_target_create attaches only a color attachment; attach a DEPTH
        //    renderbuffer too so depth-tested geometry (the spinning cube) occludes
        //    correctly. The triangle stream never enables GL_DEPTH_TEST, so this is
        //    inert for the existing probe — only the FBO completeness check sees it.
        AhbRenderTarget rt;
        GLuint depth_rb = 0;
        bool rt_ok = ahb_target_create(rt, dpy, fb_w_, fb_h_);
        if (rt_ok) {
            glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
            glGenRenderbuffers(1, &depth_rb);
            glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, fb_w_, fb_h_);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                      GL_RENDERBUFFER, depth_rb);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            rt_ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
        if (!rt_ok) {
            error_ = "ahb-fbo-incomplete";
            setup_failed_.store(true, std::memory_order_release);
            if (depth_rb) glDeleteRenderbuffers(1, &depth_rb);
            ahb_target_destroy(rt);
            win_present_teardown(dpy, win);
            if (win.ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, win.ctx);
            if (win.surf != EGL_NO_SURFACE) eglDestroySurface(dpy, win.surf);
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(dpy, ctx);
            eglDestroySurface(dpy, surf);
            eglTerminate(dpy);
            ready_.store(true, std::memory_order_release);  // ready-with-error
            return;
        }

        // Setup OK — release start().
        ready_.store(true, std::memory_order_release);

        // 3) Consume loop: drain ring bytes into a per-frame accumulator; on each
        //    producer flush (req_seq advances past what we've replied to) decode the
        //    frame into the AHB-FBO, glFinish, present (callback + optional on-screen
        //    window blit), and post_reply.
        run_consume_loop(dpy, surf, ctx, rt, win);

        // 4) GL teardown — on this thread, before it exits.
        if (depth_rb) glDeleteRenderbuffers(1, &depth_rb);
        ahb_target_destroy(rt);
        win_present_teardown(dpy, win);
        if (win.ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, win.ctx);
        if (win.surf != EGL_NO_SURFACE) eglDestroySurface(dpy, win.surf);
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(dpy, ctx);
        eglDestroySurface(dpy, surf);
        eglTerminate(dpy);
    }

    // The per-frame consume/handshake loop. See header comment §frame model.
    // `dpy/render_surf/render_ctx` are the pbuffer (render) context; `win` is the
    // optional window-present context (win.ctx == EGL_NO_CONTEXT => headless).
    void run_consume_loop(EGLDisplay dpy, EGLSurface render_surf, EGLContext render_ctx,
                          AhbRenderTarget& rt, WinPresent& win) {
        RingConsumer cons(region_);
        auto* h = static_cast<RingHeader*>(region_);

        std::vector<uint8_t> scratch;       // reused snapshot buffer (per drain)
        std::vector<uint8_t> frame_bytes;   // accumulated op bytes for current frame
        uint32_t replied_seq = 0;           // last req_seq we have post_reply'd to

        for (;;) {
            // Drain all currently-available ring bytes into the frame accumulator.
            uint64_t avail = cons.available();
            while (avail > 0) {
                if (scratch.size() < avail) scratch.resize(static_cast<size_t>(avail));
                const uint32_t got =
                    cons.snapshot(scratch.data(), static_cast<uint32_t>(scratch.size()));
                if (got == 0) break;
                frame_bytes.insert(frame_bytes.end(), scratch.begin(), scratch.begin() + got);
                cons.advance(got);
                avail = cons.available();
            }

            // Has the producer flushed a frame we haven't replied to yet? req_seq is
            // bumped (acq_rel) AFTER the producer's append() published head (release),
            // so once we observe req_seq advanced we are guaranteed to have seen — and
            // the loop above to have drained — all of that frame's bytes.
            const uint32_t req = h->req_seq.load(std::memory_order_acquire);
            if (req != replied_seq) {
                // Defensive: drain anything that became visible between the check above
                // and observing the req bump, so the frame is complete before decode.
                avail = cons.available();
                while (avail > 0) {
                    if (scratch.size() < avail) scratch.resize(static_cast<size_t>(avail));
                    const uint32_t got =
                        cons.snapshot(scratch.data(), static_cast<uint32_t>(scratch.size()));
                    if (got == 0) break;
                    frame_bytes.insert(frame_bytes.end(), scratch.begin(),
                                       scratch.begin() + got);
                    cons.advance(got);
                    avail = cons.available();
                }

                // Decode this frame into the AHB-FBO with a FRESH HostState so the
                // guest's per-frame virtual IDs (vVS=1, vFS=2, vPROG=1, ... — constants
                // from build_triangle_stream) resolve identically every frame.
                glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
                HostState st;
                if (!frame_bytes.empty()) {
                    decode_batch(frame_bytes.data(), frame_bytes.size(), st);
                }
                glFinish();  // ensure the draw has landed in the AHB before present.

                if (present_) {
                    present_(rt);  // callback runs on THIS (GL) thread, RENDER context.
                }

                // On-screen present (cross-context): switch to the window context and
                // sample the just-rendered AHB via external-OES into the window
                // surface. Sampling in the OTHER context dodges Mali's same-context
                // black-AHB hazard. Headless when win.ctx == EGL_NO_CONTEXT.
                if (win.ctx != EGL_NO_CONTEXT) {
                    if (eglMakeCurrent(dpy, win.surf, win.surf, win.ctx) == EGL_TRUE) {
                        present_to_window(dpy, rt, win);
                    }
                    // Restore the render context for the next frame's decode.
                    eglMakeCurrent(dpy, render_surf, render_surf, render_ctx);
                }

                frames_presented_.fetch_add(1, std::memory_order_acq_rel);
                frame_bytes.clear();

                // Release the producer: reply_seq := req_seq (matches flush_and_wait).
                replied_seq = req;
                cons.post_reply();
                continue;  // immediately look for the next frame's bytes.
            }

            // Nothing pending. Exit if asked to stop and the producer is closed and
            // there is no outstanding work; otherwise spin-yield (no sleeps).
            if (stop_.load(std::memory_order_acquire)) {
                // Make sure we didn't race past a final flush before stopping.
                const uint32_t final_req = h->req_seq.load(std::memory_order_acquire);
                if (final_req == replied_seq && cons.available() == 0) {
                    break;
                }
                // else: a late frame arrived; loop around to service it.
            }
            idle_wait();
        }
    }

    // Idle wait between frames. With a doorbell (real guest) block briefly on the
    // eventfd so a producer flush wakes us promptly instead of burning a core; the
    // short timeout backstops any missed wakeup (the ring head/req_seq is the source
    // of truth — the doorbell is only a latency/CPU optimization). Without a doorbell
    // (doorbell_fd_ < 0, both in-process probes) this is the original spin-yield.
    void idle_wait() {
        if (doorbell_fd_ < 0) {
            std::this_thread::yield();
            return;
        }
        struct pollfd pfd = {doorbell_fd_, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, /*timeout_ms=*/4);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            // Drain the eventfd counter (readable => count > 0, so this won't block).
            uint64_t tok = 0;
            const ssize_t n = ::read(doorbell_fd_, &tok, sizeof(tok));
            (void)n;
        }
    }

    void fail_setup(std::string why) {
        error_ = std::move(why);
        setup_failed_.store(true, std::memory_order_release);
        ready_.store(true, std::memory_order_release);  // unblock start() (with error).
    }

    // Build the samplerExternalOES blit program + the persistent external-OES
    // texture object, in the CURRENTLY-CURRENT (window) context. Returns false on
    // any GL failure. A full-screen triangle (3 verts) covers the viewport; UVs are
    // derived from clip position so no separate attribute/VBO is needed. We DO flip V
    // (uv.y = 1 - ...) because the AHB-FBO was rendered bottom-up (GL convention) and
    // the window surface is also bottom-up, but the AHB content read back as a 2D
    // texture is top-left origin — matching the WaylandPresenter external-OES path
    // which V-flips. The cube demo is symmetric in Y at frame boundaries, so this is
    // belt-and-suspenders; a wrong flip would still present, just upside down.
    bool win_present_setup(WinPresent& win) {
        const char* vsrc =
            "attribute vec2 aPos; varying vec2 vUv;"
            "void main(){ vUv = vec2(aPos.x*0.5+0.5, 1.0-(aPos.y*0.5+0.5));"
            " gl_Position = vec4(aPos, 0.0, 1.0); }";
        const char* fsrc =
            "#extension GL_OES_EGL_image_external : require\n"
            "precision mediump float; varying vec2 vUv;"
            "uniform samplerExternalOES uTex;"
            "void main(){ gl_FragColor = texture2D(uTex, vUv); }";
        GLuint vs = run_compile(GL_VERTEX_SHADER, vsrc);
        GLuint fs = run_compile(GL_FRAGMENT_SHADER, fsrc);
        if (vs == 0 || fs == 0) {
            if (vs) glDeleteShader(vs);
            if (fs) glDeleteShader(fs);
            return false;
        }
        win.program = glCreateProgram();
        glAttachShader(win.program, vs);
        glAttachShader(win.program, fs);
        glBindAttribLocation(win.program, 0, "aPos");
        glLinkProgram(win.program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint linked = 0;
        glGetProgramiv(win.program, GL_LINK_STATUS, &linked);
        if (!linked) {
            glDeleteProgram(win.program);
            win.program = 0;
            return false;
        }
        win.a_pos = glGetAttribLocation(win.program, "aPos");
        win.u_tex = glGetUniformLocation(win.program, "uTex");
        glGenTextures(1, &win.ext_tex);
        return win.ext_tex != 0;
    }

    void win_present_teardown(EGLDisplay /*dpy*/, WinPresent& win) {
        // win.program / win.ext_tex were created in win.ctx, which shares an EGL
        // share group with the pbuffer (render) context, so they are valid to delete
        // from EITHER context — the caller need not have the window context current.
        // (eglDestroyContext on win.ctx would also free them, so this is belt-and-
        // suspenders for the success path; harmless if some other context is current.)
        if (win.ext_tex) { glDeleteTextures(1, &win.ext_tex); win.ext_tex = 0; }
        if (win.program) { glDeleteProgram(win.program); win.program = 0; }
    }

    // Import the rendered AHB as external-OES and blit a full-screen triangle to the
    // window surface, then swap. MUST be called with the window context current.
    void present_to_window(EGLDisplay dpy, AhbRenderTarget& rt, WinPresent& win) {
        // Import the AHB (already an EGLImageKHR in rt.image) into the external-OES
        // texture. Re-binding the same EGLImage each frame is cheap and avoids any
        // stale-content question on a persistent texture.
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, win.ext_tex);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        win.p_image_target(GL_TEXTURE_EXTERNAL_OES,
                           static_cast<GLeglImageOES>(rt.image));

        const int win_w = std::max(1, ANativeWindow_getWidth(window_));
        const int win_h = std::max(1, ANativeWindow_getHeight(window_));
        glBindFramebuffer(GL_FRAMEBUFFER, 0);  // the window surface (default FBO)
        glViewport(0, 0, win_w, win_h);
        glDisable(GL_DEPTH_TEST);  // the blit is a flat full-screen pass
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(win.program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, win.ext_tex);
        glUniform1i(win.u_tex, 0);
        // Full-screen triangle (covers the viewport; clipped to the quad region).
        const GLfloat tri[] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
        glEnableVertexAttribArray(static_cast<GLuint>(win.a_pos));
        glVertexAttribPointer(static_cast<GLuint>(win.a_pos), 2, GL_FLOAT, GL_FALSE, 0, tri);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        eglSwapBuffers(dpy, win.surf);
    }

    void* region_ = nullptr;
    int fb_w_ = 0;
    int fb_h_ = 0;
    PresentFn present_;
    ANativeWindow* window_ = nullptr;  // optional on-screen target (nullptr = headless)
    int doorbell_fd_ = -1;             // optional guest-flush wakeup eventfd (NOT owned; -1 = spin)

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> setup_failed_{false};
    std::atomic<bool> software_{false};
    std::atomic<bool> window_ready_{false};  // window ctx + present program created OK
    std::atomic<uint32_t> frames_presented_{0};
    std::string error_;       // written on the thread before ready_/at teardown; read after.
    std::string renderer_;    // captured once on the thread before ready_=true.
};

// ===========================================================================
// run_live_integration_probe — in-process, two-thread device self-test.
//
// A plain malloc'd ring (no fd, no fork) that both a producer thread and the
// executor's consumer thread see. The producer pushes the M1 triangle op stream
// for N frames; the executor renders each into an AHB on Mali; a present callback
// glReadPixels-verifies the result with the SAME tolerances as
// run_fbo_present_probe. Proves the full multi-frame live handshake end to end.
// ===========================================================================
inline std::string run_live_integration_probe() {
    std::ostringstream out;
    constexpr int W = 64, H = 64;
    constexpr uint32_t kFrames = 8;
    constexpr uint32_t kRingBytes = 1u << 16;  // 64 KiB data ring (a frame is ~1.6 KiB)

    // ---- Allocate + init the shared ring region (heap; both threads see it). ----
    std::vector<uint8_t> region(ring_region_size(kRingBytes), 0u);
    if (!ring_init(region.data(), kRingBytes)) {
        out << "ALR GPU LIVE INTEGRATION: FAIL\nalr gpu live error=ring-init";
        return out.str();
    }

    // ---- Build the per-frame op stream once (reused every frame). ----
    const std::vector<uint8_t> stream = build_triangle_stream(W, H);

    // ---- Present-callback state (written ONLY on the consumer thread; read after
    //      join(), so no synchronization needed beyond the join barrier). ----
    std::atomic<uint32_t> frames_verified{0};
    unsigned char last_center[4] = {0, 0, 0, 0};
    unsigned char last_corner[4] = {0, 0, 0, 0};

    GpuExecutorService::PresentFn present =
        [&](const AhbRenderTarget& rt) {
            // The callback runs on the GL thread right after glFinish; read straight
            // from the AHB-backed FBO (the most direct proof the decoded guest draw
            // landed in the AHB), using run_fbo_present_probe's exact thresholds.
            unsigned char center[4] = {0, 0, 0, 0};
            unsigned char corner[4] = {0, 0, 0, 0};
            glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
            glReadPixels(rt.w / 2, rt.h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
            glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
            const bool center_green = center[1] > 150 && center[0] < 100 && center[2] < 100;
            const bool corner_blue = corner[2] > 80 && corner[1] < 90 && corner[0] < 90;
            for (int i = 0; i < 4; ++i) {
                last_center[i] = center[i];
                last_corner[i] = corner[i];
            }
            if (center_green && corner_blue) {
                frames_verified.fetch_add(1, std::memory_order_acq_rel);
            }
        };

    // ---- Start the executor (creates EGL + AHB-FBO on its own thread). ----
    GpuExecutorService svc(region.data(), region.size(), W, H, std::move(present));
    const bool started = svc.start();
    if (!started) {
        out << "ALR GPU LIVE INTEGRATION: FAIL\nalr gpu live error=executor-start:"
            << svc.error();
        svc.stop();
        return out.str();
    }

    // ---- Producer thread: push the stream + flush_and_wait, N times. This is the
    //      live per-frame handshake (req_seq bump + block on reply_seq). ----
    std::atomic<uint32_t> frames_sent{0};
    std::atomic<bool> producer_ok{true};
    std::thread producer([&] {
        RingProducer prod(region.data());
        if (!prod.valid()) {
            producer_ok.store(false, std::memory_order_release);
            return;
        }
        for (uint32_t f = 0; f < kFrames; ++f) {
            // Append the whole frame stream. It fits in the 64 KiB ring (a frame is
            // ~1.6 KiB and the consumer drains continuously), but push in chunks with
            // back-pressure retry to be robust regardless of ring size.
            size_t off = 0;
            const auto* base = stream.data();
            const size_t total = stream.size();
            while (off < total) {
                const uint32_t chunk =
                    static_cast<uint32_t>(std::min<size_t>(total - off, kRingBytes / 2));
                if (prod.append(base + off, chunk)) {
                    off += chunk;
                } else {
                    // Ring full — let the consumer drain. (No GL on this thread.)
                    std::this_thread::yield();
                }
            }
            // The one sync point per frame: bump req_seq, block until the executor
            // post_reply's (reply_seq catches up). Generous spin so we never give up
            // before the GPU finishes a 64x64 draw.
            prod.flush_and_wait(1u << 24);
            frames_sent.fetch_add(1, std::memory_order_acq_rel);
        }
        prod.close();
    });

    producer.join();

    // ---- All frames sent; stop the executor (drains any final frame, then joins). ----
    svc.stop();

    // ---- Build the report. ----
    const uint32_t presented = svc.frames_presented();
    const uint32_t verified = frames_verified.load(std::memory_order_acquire);
    const uint32_t sent = frames_sent.load(std::memory_order_acquire);
    const bool sw = svc.software();
    const bool no_err = svc.error().empty();
    const bool prod_ok = producer_ok.load(std::memory_order_acquire);

    const bool pass = (presented == kFrames) && (verified == kFrames) && !sw && no_err &&
                      prod_ok && (sent == kFrames);

    out << "ALR GPU LIVE INTEGRATION: " << (pass ? "PASS" : "FAIL");
    out << "\nalr gpu live model=two-thread in-process ring (producer thread + GL "
           "executor thread), per-frame req_seq/reply_seq handshake";
    out << "\nalr gpu live frames_requested=" << kFrames;
    out << "\nalr gpu live frames_sent=" << sent;
    out << "\nalr gpu live frames_presented=" << presented;
    out << "\nalr gpu live frames_verified=" << verified;
    out << "\nalr gpu live last center=" << static_cast<int>(last_center[0]) << ","
        << static_cast<int>(last_center[1]) << "," << static_cast<int>(last_center[2])
        << " corner=" << static_cast<int>(last_corner[0]) << ","
        << static_cast<int>(last_corner[1]) << "," << static_cast<int>(last_corner[2])
        << " (expect center ~0,220,0 textured-green, corner ~26,26,102 clear-blue)";
    out << "\nalr gpu live renderer=" << svc.renderer_string();
    out << "\nalr gpu live software renderer=" << (sw ? "true" : "false");
    out << "\nalr gpu live producer ok=" << (prod_ok ? "true" : "false");
    out << "\nalr gpu live error=" << (no_err ? "(none)" : svc.error());
    return out.str();
}

}  // namespace alr::gpu

#endif  // ALR_GPU_ALR_GPU_HOST_SERVICE_HPP
