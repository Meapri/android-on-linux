// ALR GPU Vulkan host servicer — the app-process half of the guest Vulkan ICD path.
//
// This is the Vulkan twin of alr_gpu_host_service.hpp's GpuExecutorService, but for
// the guest Vulkan ICD (alr_gpu/guest_icd/, SONAME libvulkan.so.1) instead of the
// GLES shim. Where the GLES executor owns a Mali GLES2 context and replays draw ops
// into an AHB-FBO, this servicer owns NOTHING persistent: it drains a REQUEST ring of
// Vulkan op batches (alr_gpu_vk_proto.hpp), replays each batch on the REAL vendor Mali
// libvulkan via decode_vk_batch (alr_gpu_vk_decode.hpp, ALR_VK_DECODE_REAL — the
// DEVICE-PROVEN enumerate/props/device/queue path), and writes the AlrVkReply stream
// back on a REPLY ring the guest ICD consumes. That reply (device count + the
// "Mali-G615 MC2" props) is the data the ENUM rung must return — the GLES path has no
// such reply stream, which is the one structural difference (a SECOND ring).
//
// TRANSPORT / HANDSHAKE (matches the ICD's alr_icd_runtime.h):
//   * REQUEST ring (guest ICD = producer, this servicer = consumer): the ICD appends a
//     request batch, bumps the request ring's req_seq, and blocks on its reply_seq.
//   * This servicer's thread waits for req_seq to advance, drains the full request
//     batch, decode_vk_batch's it on real Mali into a VkReplyEncoder, appends those
//     reply bytes to the REPLY ring, then bumps the REQUEST ring's reply_seq to release
//     the ICD. (reply_seq on the request ring is the completion signal; the reply ring
//     carries the payload — exactly the two-ring shape run_vk_marshal_mali_probe proves
//     in-process, now spanning the app-process host and the forked guest.)
//
// A PERSISTENT VkDecodeState lives across batches (like the GLES HostState) so virtual
// instance/physical-device/device/queue ids the guest created in one batch survive into
// the next (the ICD may enumerate in one submit and create a device in a later one).
//
// Header-only + self-contained (public NDK Vulkan + POSIX only), matching alr_gpu/**;
// runtime_report.cpp #includes it (after defining ALR_VK_DECODE_REAL) and adds a JNI
// self-test + the loader attach call.

#ifndef ALR_GPU_ALR_GPU_VK_HOST_SERVICE_HPP
#define ALR_GPU_ALR_GPU_VK_HOST_SERVICE_HPP

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "alr_gpu/alr_gpu_ring.hpp"        // ring_init, RingProducer/RingConsumer
#include "alr_gpu/alr_gpu_vk_decode.hpp"   // decode_vk_batch, VkDecodeState, VkReplyEncoder
#include "alr_gpu/alr_gpu_vk_proto.hpp"    // AlrVkEncoder + wire

namespace alr::gpu {

// ===========================================================================
// VkRingServicer — owns the CONSUMER side of the REQUEST ring and the PRODUCER side
// of the REPLY ring on its own thread, replaying each request batch on real Mali
// libvulkan and writing the reply back. The guest ICD is the counterpart producer/
// consumer (alr_gpu/guest_icd/).
// ===========================================================================
class VkRingServicer {
public:
    // `req_region` (guest->host requests) and `rep_region` (host->guest replies) must
    // both already be ring_init'd. `doorbell_fd` is OPTIONAL (an inherited eventfd the
    // guest ICD signals on submit; -1 = spin/poll). The servicer reads/drains it but
    // does NOT own it.
    VkRingServicer(void* req_region, void* rep_region, int doorbell_fd = -1)
        : req_region_(req_region), rep_region_(rep_region), doorbell_fd_(doorbell_fd) {}

    ~VkRingServicer() { stop(); }
    VkRingServicer(const VkRingServicer&) = delete;
    VkRingServicer& operator=(const VkRingServicer&) = delete;

    bool start() {
        if (thread_.joinable()) return true;
        if (!ring_valid(req_region_) || !ring_valid(rep_region_)) {
            error_ = "vk-ring-invalid";
            return false;
        }
        stop_.store(false, std::memory_order_release);
        thread_ = std::thread([this] { thread_main(); });
        return true;
    }

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (ring_valid(req_region_)) {
            static_cast<RingHeader*>(req_region_)->closed.store(1, std::memory_order_release);
        }
        if (thread_.joinable()) thread_.join();
    }

    uint32_t batches_serviced() const {
        return batches_serviced_.load(std::memory_order_acquire);
    }
    const std::string& error() const { return error_; }

private:
    void thread_main() {
        RingConsumer req_cons(req_region_);
        RingProducer rep_prod(rep_region_);
        auto* req_h = static_cast<RingHeader*>(req_region_);

        std::vector<uint8_t> scratch;      // reused request snapshot
        std::vector<uint8_t> batch_bytes;  // accumulated request bytes for current batch
        uint32_t replied_seq = 0;          // last req_seq we have acked

        // PERSISTENT decode state across batches (virtual->real maps survive — the ICD
        // may enumerate in one submit and create a device in a later one). On device
        // (ALR_VK_DECODE_REAL) this touches the real Mali driver.
        VkDecodeState st;

        for (;;) {
            // Drain all currently-available request bytes into the batch accumulator.
            drain_into(req_cons, scratch, batch_bytes);

            const uint32_t req = req_h->req_seq.load(std::memory_order_acquire);
            if (req != replied_seq) {
                // Defensive re-drain (catch bytes that landed between the check and the
                // req bump observation), then the batch is complete.
                drain_into(req_cons, scratch, batch_bytes);

                // Replay the request batch on real Mali libvulkan; build the reply.
                VkReplyEncoder reply;
                if (!batch_bytes.empty()) {
                    // provider=nullptr => the ALR_VK_DECODE_REAL path (vendor libvulkan).
                    decode_vk_batch(batch_bytes.data(), batch_bytes.size(), st, reply,
                                    /*provider=*/nullptr);
                } else {
                    // Empty batch: still emit a clean reply terminator so the guest's
                    // drain returns promptly.
                    reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_END));
                }

                // Push the reply onto the REPLY ring (chunked with back-pressure retry,
                // though the ENUM reply is tiny). The guest drains it after we bump the
                // request ring's reply_seq below.
                push_reply(rep_prod, reply.bytes());

                batch_bytes.clear();
                batches_serviced_.fetch_add(1, std::memory_order_acq_rel);

                // Release the guest: reply_seq := req_seq on the REQUEST ring (the ICD's
                // alr_icd_roundtrip spins on exactly this).
                replied_seq = req;
                req_h->reply_seq.store(req, std::memory_order_release);
                continue;
            }

            if (stop_.load(std::memory_order_acquire)) {
                const uint32_t final_req = req_h->req_seq.load(std::memory_order_acquire);
                if (final_req == replied_seq && req_cons.available() == 0) break;
            }
            idle_wait();
        }
    }

    static void drain_into(RingConsumer& cons, std::vector<uint8_t>& scratch,
                           std::vector<uint8_t>& out) {
        uint64_t avail = cons.available();
        while (avail > 0) {
            if (scratch.size() < avail) scratch.resize(static_cast<size_t>(avail));
            const uint32_t got = cons.snapshot(scratch.data(),
                                               static_cast<uint32_t>(scratch.size()));
            if (got == 0) break;
            out.insert(out.end(), scratch.begin(), scratch.begin() + got);
            cons.advance(got);
            avail = cons.available();
        }
    }

    static void push_reply(RingProducer& prod, const std::vector<uint8_t>& bytes) {
        size_t off = 0;
        const auto* base = bytes.data();
        const size_t total = bytes.size();
        // Bounded retry so a (pathological) full reply ring can't wedge the thread
        // forever; the ENUM reply is far smaller than the ring, so this is one append.
        int guard = 1 << 20;
        while (off < total && guard-- > 0) {
            const uint32_t chunk = static_cast<uint32_t>(total - off);
            if (prod.append(base + off, chunk)) {
                off += chunk;
            } else {
                std::this_thread::yield();
            }
        }
    }

    void idle_wait() {
        if (doorbell_fd_ < 0) {
            std::this_thread::yield();
            return;
        }
        struct pollfd pfd = {doorbell_fd_, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, /*timeout_ms=*/4);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            uint64_t tok = 0;
            const ssize_t n = ::read(doorbell_fd_, &tok, sizeof(tok));
            (void)n;
        }
    }

    void* req_region_ = nullptr;
    void* rep_region_ = nullptr;
    int doorbell_fd_ = -1;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> batches_serviced_{0};
    std::string error_;
};

// ===========================================================================
// VkRing — the env/fd bundle the guest ICD needs (the Vulkan twin of GpuRing). Filled
// by alr_loader_attach_vk_ring(); WS-1 inherits the fds across fork and advertises them
// via vk_ring_guest_env().
// ===========================================================================
struct VkRing {
    int req_fd = -1;          // inheritable memfd of the ring_init'd REQUEST region
    uint32_t req_bytes = 0;   // request data-region size (power of two)
    int rep_fd = -1;          // inheritable memfd of the REPLY region
    uint32_t rep_bytes = 0;   // reply data-region size
    int doorbell_fd = -1;     // inheritable eventfd the guest signals on submit (-1 = none)
};

namespace vkdetail {

inline int make_memfd(const char* name) {
    long fd = ::syscall(__NR_memfd_create, name, 0u);
    return fd < 0 ? -1 : static_cast<int>(fd);
}

// Allocate a memfd, size it, map it MAP_SHARED in the parent, ring_init() it. Returns
// the fd (>=0) and fills *out_region / *out_region_sz; -1 on failure.
inline int make_ring(uint32_t ring_bytes, void** out_region, size_t* out_region_sz) {
    if (ring_bytes == 0 || (ring_bytes & (ring_bytes - 1)) != 0) return -1;
    const size_t region_sz = ring_region_size(ring_bytes);
    const int fd = make_memfd("alr_vk_ring");
    if (fd < 0) return -1;
    if (::ftruncate(fd, static_cast<off_t>(region_sz)) != 0) { ::close(fd); return -1; }
    void* region = ::mmap(nullptr, region_sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (region == MAP_FAILED) { ::close(fd); return -1; }
    if (!ring_init(region, ring_bytes)) { ::munmap(region, region_sz); ::close(fd); return -1; }
    *out_region = region;
    *out_region_sz = region_sz;
    return fd;
}

// Process-global holder for the attached VK rings + servicer (the app/host process
// persists across the loader fork, so the servicer keeps draining while the guest ICD
// produces). Mirrors alr_gpu_ring_hook.hpp's GpuRingHolder.
struct VkRingHolder {
    std::mutex mu;
    std::unique_ptr<VkRingServicer> svc;
    void* req_region = nullptr;
    size_t req_region_sz = 0;
    void* rep_region = nullptr;
    size_t rep_region_sz = 0;
    int req_fd = -1;
    int rep_fd = -1;
    int doorbell_fd = -1;
    bool attached = false;
};

inline VkRingHolder& holder() {
    static VkRingHolder h;
    return h;
}

}  // namespace vkdetail

// ---------------------------------------------------------------------------
// vk_ring_guest_env — the env strings WS-1 appends to its guest_env vector so the guest
// ICD (alr_icd_env.h) finds the rings. SINGLE SOURCE OF TRUTH for the key names (must
// equal alr_gpu/guest_icd/alr_icd_env.h).
// ---------------------------------------------------------------------------
inline std::vector<std::string> vk_ring_guest_env(const VkRing& r) {
    std::vector<std::string> env;
    if (r.req_fd < 0 || r.req_bytes == 0 || r.rep_fd < 0 || r.rep_bytes == 0) return env;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "ALR_VK_RING_FD=%d", r.req_fd);
    env.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "ALR_VK_RING_BYTES=%u", r.req_bytes);
    env.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "ALR_VK_REPLY_FD=%d", r.rep_fd);
    env.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "ALR_VK_REPLY_BYTES=%u", r.rep_bytes);
    env.emplace_back(buf);
    if (r.doorbell_fd >= 0) {
        std::snprintf(buf, sizeof(buf), "ALR_VK_RING_DOORBELL_FD=%d", r.doorbell_fd);
        env.emplace_back(buf);
    }
    return env;
}

// ---------------------------------------------------------------------------
// alr_loader_attach_vk_ring — WS-1 calls this ONCE before fork (when the guest opts
// into the Vulkan ICD, e.g. ALR_VK_ICD=1). Creates the request+reply rings + an
// optional doorbell, starts the servicer thread (which owns NDK libvulkan), and fills
// `out`. Returns true if the VK ring pipeline is live (WS-1 then advertises
// vk_ring_guest_env(out) + inherits the fds). Idempotent: a second call without detach
// returns the existing rings.
//
// `ring_bytes` is the per-ring data size (power of two); 64 KiB is ample for the ENUM
// rung (a request is tens of bytes, a reply ~90).
// ---------------------------------------------------------------------------
inline bool alr_loader_attach_vk_ring(VkRing& out, uint32_t ring_bytes = (1u << 16)) {
    auto& h = vkdetail::holder();
    std::lock_guard<std::mutex> lk(h.mu);
    if (h.attached) {
        out.req_fd = h.req_fd;
        out.req_bytes = static_cast<uint32_t>(
            ring_valid(h.req_region) ? static_cast<RingHeader*>(h.req_region)->ring_bytes : 0u);
        out.rep_fd = h.rep_fd;
        out.rep_bytes = static_cast<uint32_t>(
            ring_valid(h.rep_region) ? static_cast<RingHeader*>(h.rep_region)->ring_bytes : 0u);
        out.doorbell_fd = h.doorbell_fd;
        return h.svc && h.svc->error().empty();
    }

    void* req_region = nullptr; size_t req_sz = 0;
    void* rep_region = nullptr; size_t rep_sz = 0;
    const int req_fd = vkdetail::make_ring(ring_bytes, &req_region, &req_sz);
    if (req_fd < 0) return false;
    const int rep_fd = vkdetail::make_ring(ring_bytes, &rep_region, &rep_sz);
    if (rep_fd < 0) {
        ::munmap(req_region, req_sz);
        ::close(req_fd);
        return false;
    }
    const int bell_fd = ::eventfd(0, 0);  // optional; -1 acceptable

    auto svc = std::make_unique<VkRingServicer>(req_region, rep_region, bell_fd);
    if (!svc->start()) {
        ::munmap(req_region, req_sz);
        ::munmap(rep_region, rep_sz);
        ::close(req_fd);
        ::close(rep_fd);
        if (bell_fd >= 0) ::close(bell_fd);
        return false;
    }

    h.svc = std::move(svc);
    h.req_region = req_region; h.req_region_sz = req_sz;
    h.rep_region = rep_region; h.rep_region_sz = rep_sz;
    h.req_fd = req_fd; h.rep_fd = rep_fd; h.doorbell_fd = bell_fd;
    h.attached = true;

    out.req_fd = req_fd;
    out.req_bytes = ring_bytes;
    out.rep_fd = rep_fd;
    out.rep_bytes = ring_bytes;
    out.doorbell_fd = bell_fd;
    return true;
}

// Stop the servicer + release the parent's VK ring resources. Idempotent.
inline void alr_loader_detach_vk_ring() {
    auto& h = vkdetail::holder();
    std::lock_guard<std::mutex> lk(h.mu);
    if (!h.attached) return;
    if (h.svc) { h.svc->stop(); h.svc.reset(); }
    if (h.req_region) { ::munmap(h.req_region, h.req_region_sz); h.req_region = nullptr; }
    if (h.rep_region) { ::munmap(h.rep_region, h.rep_region_sz); h.rep_region = nullptr; }
    if (h.req_fd >= 0) { ::close(h.req_fd); h.req_fd = -1; }
    if (h.rep_fd >= 0) { ::close(h.rep_fd); h.rep_fd = -1; }
    if (h.doorbell_fd >= 0) { ::close(h.doorbell_fd); h.doorbell_fd = -1; }
    h.attached = false;
}

// ===========================================================================
// run_vk_icd_servicer_probe — HOST-side self-test of the servicer + dual-ring transport
// WITHOUT a guest process. It stands in for the guest ICD with a host-side producer that
// speaks the EXACT same wire the ICD emits (create-instance / enumerate / props), pushes
// it through the request ring, lets the servicer thread replay it on real Mali + write
// the reply ring, then decodes the reply and asserts the Mali device surfaced. This is
// the Vulkan analogue of run_live_integration_probe (in-process two-thread), and is the
// host BUILD/DEVICE gate for the servicer half. The forked-guest ICD path (libvulkan.so.1
// in the rootfs) is the separate device test (alr-vk-enum). Gating first line:
// "ALR VK ICD SERVICE: PASS/FAIL".
// ===========================================================================
inline std::string run_vk_icd_servicer_probe() {
    std::ostringstream out;
    constexpr uint32_t kRingBytes = 1u << 16;  // 64 KiB each
    constexpr uint32_t kVinst = 7, kVphysBase = 100;

    std::vector<uint8_t> req_region(ring_region_size(kRingBytes), 0u);
    std::vector<uint8_t> rep_region(ring_region_size(kRingBytes), 0u);
    if (!ring_init(req_region.data(), kRingBytes) || !ring_init(rep_region.data(), kRingBytes)) {
        out << "ALR VK ICD SERVICE: FAIL\nalr vk icd service error=ring-init";
        return out.str();
    }

    // Start the servicer on the heap rings (no fds, no fork — both "ends" are this
    // process; the servicer drains the request ring + replays on real Mali libvulkan).
    VkRingServicer svc(req_region.data(), rep_region.data(), /*doorbell=*/-1);
    if (!svc.start()) {
        out << "ALR VK ICD SERVICE: FAIL\nalr vk icd service error=servicer-start:" << svc.error();
        return out.str();
    }

    // --- ICD-equivalent producer: emit the SAME request batch the ICD's
    // vkEnumeratePhysicalDevices emits (create instance + enumerate + props), push it,
    // bump req_seq, block on reply_seq — then drain the reply ring. ---
    RingProducer req_prod(req_region.data());
    RingConsumer rep_cons(rep_region.data());

    uint8_t reqbuf[64];
    AlrVkEncoder e;
    alr_vk_enc_init(&e, reqbuf, sizeof(reqbuf));
    alr_vk_enc_create_instance(&e, kVinst, (1u << 22) | (1u << 12));  // API 1.1
    alr_vk_enc_enumerate_phys(&e, kVinst, kVphysBase);
    alr_vk_enc_get_phys_props(&e, kVinst, kVphysBase);
    alr_vk_enc_u8(&e, static_cast<uint8_t>(ALR_VK_OP_END));

    bool pushed = req_prod.append(reqbuf, static_cast<uint32_t>(e.len));
    // Bump req_seq + block until the servicer bumps reply_seq (this IS the ICD handshake).
    req_prod.flush_and_wait(1u << 24);

    // Drain the reply ring.
    std::vector<uint8_t> rep_snap(rep_cons.available());
    const uint32_t rep_got = rep_cons.snapshot(rep_snap.data(),
                                               static_cast<uint32_t>(rep_snap.size()));
    rep_cons.advance(rep_got);

    svc.stop();

    // Decode the reply + assert the Mali device surfaced through the servicer.
    VkDecodedReply decoded;
    const bool reply_ok = decode_vk_reply(rep_snap.data(), rep_got, decoded);
    const bool inst_ok = decoded.instances.size() == 1 &&
                         decoded.instances[0].vinst == kVinst &&
                         decoded.instances[0].result == 0;
    const bool enum_ok = decoded.enumerations.size() == 1 &&
                         decoded.enumerations[0].count >= 1;
    auto it = decoded.props.find(kVphysBase);
    const bool props_present = it != decoded.props.end();
    const bool props_ok = props_present && !it->second.device_name.empty() &&
                          !it->second.is_software;

    const bool pass = pushed && reply_ok && inst_ok && enum_ok && props_ok &&
                      svc.error().empty();

    out << "ALR VK ICD SERVICE: " << (pass ? "PASS" : "FAIL");
    out << "\nalr vk icd service model=host servicer thread (drain req ring -> "
           "decode_vk_batch on real Mali libvulkan -> write reply ring), driven by an "
           "ICD-equivalent producer";
    out << "\nalr vk icd service batches=" << svc.batches_serviced();
    out << "\nalr vk icd service request pushed=" << (pushed ? "yes" : "no")
        << " reply bytes=" << rep_got;
    out << "\nalr vk icd service instance result=" << (inst_ok ? "VK_SUCCESS" : "fail");
    if (enum_ok)
        out << "\nalr vk icd service device count=" << decoded.enumerations[0].count;
    if (props_present) {
        out << "\nalr vk icd service renderer=" << it->second.device_name;
        out << "\nalr vk icd service api=" << (it->second.api_version >> 22) << "."
            << ((it->second.api_version >> 12) & 0x3ff);
        out << "\nalr vk icd service software renderer="
            << (it->second.is_software ? "true" : "false");
    } else {
        out << "\nalr vk icd service props=absent";
    }
    out << "\nalr vk icd service note=this is the HOST servicer half (the same one the "
           "forked guest libvulkan.so.1 ICD drives over inherited memfd rings); the "
           "guest-ICD end-to-end is the alr-vk-enum device test";
    out << "\nalr vk icd service error=" << (svc.error().empty() ? "(none)" : svc.error());
    return out.str();
}

// ===========================================================================
// run_vk_icd_present_probe — HOST-side self-test of the VK-M4 PRESENT rung: the guest's
// OWN SPIR-V over the wire + an AHB-backed swapchain whose present routes to the
// compositor sink. WITHOUT a guest process, it stands in for the guest ICD with a
// host-side producer that emits the EXACT VK-M4 batch a guest Vulkan triangle app emits:
//   create instance -> enumerate -> create device -> get queue -> create pool ->
//   alloc cmd -> CREATE_SHADER_MODULE(vert SPIR-V) -> CREATE_SHADER_MODULE(frag SPIR-V)
//   -> CREATE_SWAPCHAIN -> ACQUIRE_NEXT_IMAGE -> CMD_BEGIN_DRAW_MODULES -> QUEUE_PRESENT,
// pushes it through the request ring, lets the servicer replay it on real Mali + write
// the reply ring, then decodes the reply and asserts: shaders created, swapchain has >=1
// image, acquire returned an index, and the presented center pixel is the GUEST shader's
// color (kAlrVkTriColor*), distinct from the clear background — i.e. the guest's SPIR-V
// ran on Mali AND its result was handed to the present sink. The SPIR-V it ships is the
// proven triangle SPIR-V (kAlrVkTri*Spv) carried OVER THE WIRE (not host-embedded in the
// pipeline) — that is the milestone: guest-supplied shaders. A counting present sink
// asserts the AHB was routed. Gating first line: "ALR VK ICD PRESENT: PASS/FAIL".
// ===========================================================================
inline std::string run_vk_icd_present_probe() {
    std::ostringstream out;
    constexpr uint32_t kRingBytes = 1u << 16;  // 64 KiB each (SPIR-V blobs ~1 KiB fit)
    constexpr uint32_t kVinst = 9, kVphysBase = 100, kVdev = 1000, kVqueue = 2000;
    constexpr uint32_t kVpool = 1500, kVcmd = 1600, kVvert = 1700, kVfrag = 1701,
                       kVswap = 1800;
    constexpr uint32_t kW = 128, kH = 128;

    // A counting present sink so the probe asserts the AHB was actually routed (the
    // device build's runtime sink forwards to the compositor; here we just count).
    static std::atomic<int> g_probe_present_count{0};
    g_probe_present_count.store(0, std::memory_order_release);
    const VkPresentSink prev_sink = vk_present_sink();
    set_vk_present_sink([](void* ahb, int w, int h, uint64_t serial) -> bool {
        (void)ahb; (void)w; (void)h; (void)serial;
        g_probe_present_count.fetch_add(1, std::memory_order_acq_rel);
        return true;  // "accepted" for the self-test
    });

    std::vector<uint8_t> req_region(ring_region_size(kRingBytes), 0u);
    std::vector<uint8_t> rep_region(ring_region_size(kRingBytes), 0u);
    if (!ring_init(req_region.data(), kRingBytes) || !ring_init(rep_region.data(), kRingBytes)) {
        set_vk_present_sink(prev_sink);
        out << "ALR VK ICD PRESENT: FAIL\nalr vk icd present error=ring-init";
        return out.str();
    }

    VkRingServicer svc(req_region.data(), rep_region.data(), /*doorbell=*/-1);
    if (!svc.start()) {
        set_vk_present_sink(prev_sink);
        out << "ALR VK ICD PRESENT: FAIL\nalr vk icd present error=servicer-start:" << svc.error();
        return out.str();
    }

    RingProducer req_prod(req_region.data());
    RingConsumer rep_cons(rep_region.data());

    // ---- Batch 1: bring the device up + UPLOAD THE GUEST SPIR-V + create swapchain +
    // acquire (acquire returns an image index we need before recording). ----
    // The SPIR-V shipped is the proven triangle SPIR-V carried OVER THE WIRE — exactly
    // what a guest app's vkCreateShaderModule would marshal. (Reusing the validated blob
    // keeps the self-test deterministic; the wire path is identical for any guest SPIR-V.)
    std::vector<uint8_t> b1(4096);
    AlrVkEncoder e;
    alr_vk_enc_init(&e, b1.data(), static_cast<uint32_t>(b1.size()));
    alr_vk_enc_create_instance(&e, kVinst, (1u << 22) | (1u << 12));  // API 1.1
    alr_vk_enc_enumerate_phys(&e, kVinst, kVphysBase);
    alr_vk_enc_create_device(&e, kVinst, kVphysBase, kVdev);
    alr_vk_enc_get_device_queue(&e, kVdev, 0, kVqueue);
    alr_vk_enc_create_command_pool(&e, kVdev, kVpool);
    alr_vk_enc_allocate_command_buffers(&e, kVdev, kVpool, kVcmd);
    alr_vk_enc_create_shader_module(&e, kVdev, kVvert, /*stage VERTEX*/ 0x00000001u,
                                    kAlrVkTriVertSpv, sizeof(kAlrVkTriVertSpv));
    alr_vk_enc_create_shader_module(&e, kVdev, kVfrag, /*stage FRAGMENT*/ 0x00000010u,
                                    kAlrVkTriFragSpv, sizeof(kAlrVkTriFragSpv));
    alr_vk_enc_create_swapchain(&e, kVdev, kVswap, kW, kH, /*image_count=*/2);
    alr_vk_enc_acquire_next_image(&e, kVdev, kVswap);
    alr_vk_enc_u8(&e, static_cast<uint8_t>(ALR_VK_OP_END));
    const bool pushed1 = !e.overflow && req_prod.append(b1.data(), static_cast<uint32_t>(e.len));
    req_prod.flush_and_wait(1u << 24);

    std::vector<uint8_t> rep1(rep_cons.available());
    const uint32_t got1 = rep_cons.snapshot(rep1.data(), static_cast<uint32_t>(rep1.size()));
    rep_cons.advance(got1);
    VkDecodedReply d1;
    const bool reply1_ok = decode_vk_reply(rep1.data(), got1, d1);

    const bool shaders_ok = d1.shaders.size() == 2 && d1.shaders[0].result == 0 &&
                            d1.shaders[1].result == 0;
    const bool swap_ok = d1.swapchains.size() == 1 && d1.swapchains[0].result == 0 &&
                         d1.swapchains[0].image_count >= 1;
    const bool acquire_ok = d1.acquires.size() == 1 && d1.acquires[0].result == 0;
    const uint32_t img_index = acquire_ok ? d1.acquires[0].image_index : 0;

    // ---- Batch 2: record the guest-shader draw into the acquired image + present. ----
    std::vector<uint8_t> b2(256);
    AlrVkEncoder e2;
    alr_vk_enc_init(&e2, b2.data(), static_cast<uint32_t>(b2.size()));
    alr_vk_enc_cmd_begin_draw_modules(&e2, kVdev, kVcmd, kVswap, img_index, kVvert, kVfrag,
                                      kW, kH, /*bg=*/0.0f, 0.0f, 0.0f, 1.0f);
    alr_vk_enc_queue_present(&e2, kVdev, kVqueue, kVcmd, kVswap, img_index);
    alr_vk_enc_u8(&e2, static_cast<uint8_t>(ALR_VK_OP_END));
    const bool pushed2 = !e2.overflow && req_prod.append(b2.data(), static_cast<uint32_t>(e2.len));
    req_prod.flush_and_wait(1u << 24);

    std::vector<uint8_t> rep2(rep_cons.available());
    const uint32_t got2 = rep_cons.snapshot(rep2.data(), static_cast<uint32_t>(rep2.size()));
    rep_cons.advance(got2);
    VkDecodedReply d2;
    const bool reply2_ok = decode_vk_reply(rep2.data(), got2, d2);

    const bool present_present = d2.presents.size() == 1;
    const VkReplyPresent pr = present_present ? d2.presents[0] : VkReplyPresent{};
    const bool render_ok = present_present && pr.render_result == 0 && pr.submit_result == 0;
    // The guest fragment shader emits kAlrVkTriColor* (~242/26/204); the center pixel must
    // be ~that (not the 0,0,0 background) — proof the GUEST's SPIR-V drew on Mali.
    auto near8 = [](uint8_t got, float want01) {
        const int want = static_cast<int>(want01 * 255.0f + 0.5f);
        const int diff = static_cast<int>(got) - want;
        return (diff < 0 ? -diff : diff) <= 12;  // tiler/UNORM rounding slack
    };
    const bool color_ok = present_present &&
                          near8(pr.px[0], kAlrVkTriColorR) &&
                          near8(pr.px[1], kAlrVkTriColorG) &&
                          near8(pr.px[2], kAlrVkTriColorB);
    const bool routed_ok = pr.presented != 0 &&
                           g_probe_present_count.load(std::memory_order_acquire) >= 1;

    svc.stop();
    set_vk_present_sink(prev_sink);

    const bool pass = pushed1 && pushed2 && reply1_ok && reply2_ok && shaders_ok &&
                      swap_ok && acquire_ok && present_present && render_ok && color_ok &&
                      routed_ok && svc.error().empty();

    out << "ALR VK ICD PRESENT: " << (pass ? "PASS" : "FAIL");
    out << "\nalr vk icd present model=guest SPIR-V over the wire (CREATE_SHADER_MODULE) "
           "+ AHB-backed swapchain (CREATE_SWAPCHAIN/ACQUIRE) + QUEUE_PRESENT routes the "
           "rendered AHB to the compositor sink; replayed on real Mali libvulkan";
    out << "\nalr vk icd present batches=" << svc.batches_serviced();
    out << "\nalr vk icd present shaders created="
        << (shaders_ok ? "2 (vert+frag, guest SPIR-V)" : "FAILED");
    if (swap_ok)
        out << "\nalr vk icd present swapchain images=" << d1.swapchains[0].image_count;
    else
        out << "\nalr vk icd present swapchain=FAILED";
    out << "\nalr vk icd present acquired image index=" << img_index
        << (acquire_ok ? "" : " (acquire FAILED)");
    if (present_present) {
        out << "\nalr vk icd present render_result=" << pr.render_result
            << " submit_result=" << pr.submit_result;
        out << "\nalr vk icd present center pixel=" << static_cast<int>(pr.px[0]) << ","
            << static_cast<int>(pr.px[1]) << "," << static_cast<int>(pr.px[2]) << ","
            << static_cast<int>(pr.px[3])
            << " (expect guest-frag ~" << static_cast<int>(kAlrVkTriColorR * 255) << ","
            << static_cast<int>(kAlrVkTriColorG * 255) << ","
            << static_cast<int>(kAlrVkTriColorB * 255) << ")";
        out << "\nalr vk icd present color match=" << (color_ok ? "yes" : "NO");
        out << "\nalr vk icd present routed to sink=" << (pr.presented ? "yes" : "no")
            << " sink calls=" << g_probe_present_count.load(std::memory_order_acquire);
    } else {
        out << "\nalr vk icd present reply=absent";
    }
    out << "\nalr vk icd present note=guest's OWN SPIR-V drove the Mali pipeline (not the "
           "host-embedded shader); the on-device end-to-end is alr-vk-tri (a guest Vulkan "
           "triangle app that creates instance/device/swapchain, records its own shaders, "
           "and presents -> triangle on the SurfaceView)";
    out << "\nalr vk icd present error=" << (svc.error().empty() ? "(none)" : svc.error());
    return out.str();
}

}  // namespace alr::gpu

#endif  // ALR_GPU_ALR_GPU_VK_HOST_SERVICE_HPP
