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
// ===========================================================================
class GpuExecutorService {
public:
    using PresentFn = std::function<void(const AhbRenderTarget&)>;

    // `ring_region` must already be ring_init'd (size == ring_region_size of the
    // chosen ring_bytes). `region_bytes` is the TOTAL region size (header + data),
    // kept for symmetry / future bounds use. `present` may be empty.
    GpuExecutorService(void* ring_region, size_t region_bytes, int fb_w, int fb_h,
                       PresentFn present)
        : region_(ring_region),
          fb_w_(fb_w),
          fb_h_(fb_h),
          present_(std::move(present)) {
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

private:
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

        // Capture renderer identity / software-ness once, now that a context is current.
        {
            std::string vendor, rr;
            if (const auto* v = reinterpret_cast<const char*>(glGetString(GL_VENDOR))) vendor = v;
            if (const auto* g = reinterpret_cast<const char*>(glGetString(GL_RENDERER))) rr = g;
            renderer_ = rr;
            software_.store(renderer_software_local(vendor, rr), std::memory_order_release);
        }

        // 2) AHB-backed render target (allocate + import + FBO), and verify complete.
        AhbRenderTarget rt;
        bool rt_ok = ahb_target_create(rt, dpy, fb_w_, fb_h_);
        if (rt_ok) {
            glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
            rt_ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
        if (!rt_ok) {
            error_ = "ahb-fbo-incomplete";
            setup_failed_.store(true, std::memory_order_release);
            ahb_target_destroy(rt);
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
        //    frame into the AHB-FBO, glFinish, present, and post_reply.
        run_consume_loop(rt);

        // 4) GL teardown — on this thread, before it exits.
        ahb_target_destroy(rt);
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(dpy, ctx);
        eglDestroySurface(dpy, surf);
        eglTerminate(dpy);
    }

    // The per-frame consume/handshake loop. See header comment §frame model.
    void run_consume_loop(AhbRenderTarget& rt) {
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
                    present_(rt);  // callback runs on THIS (GL) thread; GL is safe.
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
            std::this_thread::yield();
        }
    }

    void fail_setup(std::string why) {
        error_ = std::move(why);
        setup_failed_.store(true, std::memory_order_release);
        ready_.store(true, std::memory_order_release);  // unblock start() (with error).
    }

    void* region_ = nullptr;
    int fb_w_ = 0;
    int fb_h_ = 0;
    PresentFn present_;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> setup_failed_{false};
    std::atomic<bool> software_{false};
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
