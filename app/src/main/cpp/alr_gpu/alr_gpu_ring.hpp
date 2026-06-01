// ALR GPU command ring (Phase 4 / GPU-native Linux app track, M2 transport).
//
// A single-producer/single-consumer (SPSC) byte ring in a shared memory region.
// The GUEST (producer) appends encoded GL op batches; the HOST GPU thread
// (consumer) drains them and replays via alr::gpu::decode_batch() on the real
// Mali context. The ring lives in a parent-created mmap that the in-process
// loader's fork()ed guest inherits (COW + fd inheritance) — the same sharing the
// proven build_gpu_boundary_probe ring uses, so M2 needs NO new IPC primitive.
//
// This header is the transport DATA STRUCTURE + SPSC logic only; it is
// allocator-agnostic (the caller hands it a pointer to a shared region of size
// `ring_bytes + sizeof(Header)`), so it works equally on an anonymous MAP_SHARED
// region (host self-test, same process) or a memfd/ashmem region shared across
// the loader fork. Wiring it to a real guest + host GPU thread is M2's device
// step and lives in runtime_report.cpp (owned by the concurrent PC-gate session)
// — deferred. Header-only and self-contained so it stays in the alr_gpu TU.
//
// Frame model: the producer batches ops fire-and-forget; the ONE sync point is
// eglSwapBuffers (encoded as a SWAP op), where the producer flushes and blocks on
// reply_seq until the host has drained + presented. See /tmp/gpu-native-app/DESIGN.md §3.

#ifndef ALR_GPU_ALR_GPU_RING_HPP
#define ALR_GPU_ALR_GPU_RING_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace alr::gpu {

// Max bytes (incl. NUL) for each renderer/vendor/version identity string carried in
// the header. The host writes its REAL glGetString() values here once at startup; the
// guest shim reads them so a GLES guest's glGetString(GL_RENDERER/VENDOR/VERSION)
// reports the actual host GPU (e.g. "Mali-G615") rather than a synthetic placeholder.
// MUST match alr_gpu_ring_c.h's ALR_RING_IDENT_MAX (the guest reads this same field).
inline constexpr uint32_t kRingIdentMax = 64u;

// Shared-region header. Lives at offset 0 of the mapping; the data ring follows.
// All cursors are MONOTONIC absolute byte counts (never wrapped); the ring index
// is cursor % ring_bytes. 64-bit cursors never realistically overflow.
//
// The first 48 bytes (magic .. pad2) are the ORIGINAL hot-path layout and MUST stay
// byte-identical to alr_gpu_ring_c.h's AlrRingHeader. The identity block below is an
// APPEND-ONLY extension: it lives between pad2 and the data ring (which starts at
// sizeof(RingHeader) on BOTH sides, so growing the struct identically on both keeps
// the data ring placement consistent). The guest only reads it after identity_ready
// != 0, so an old host that never sets it leaves the guest on its synthetic fallback.
struct RingHeader {
    static constexpr uint32_t kMagic = 0x47524C41u;  // 'ALRG' little-endian
    static constexpr uint32_t kVersion = 1u;

    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t ring_bytes = 0;   // size of the data region (power of two)
    uint32_t pad = 0;
    std::atomic<uint64_t> head{0};       // producer write cursor (guest advances)
    std::atomic<uint64_t> tail{0};       // consumer read cursor (host advances)
    std::atomic<uint32_t> reply_seq{0};  // host bumps when a SYNC/swap reply is ready
    std::atomic<uint32_t> req_seq{0};    // producer bumps when it flushes a sync request
    std::atomic<uint32_t> closed{0};     // 1 = producer gone / teardown
    std::atomic<uint32_t> pad2{0};
    // ---- identity block (off 48): host->guest glGetString passthrough ----
    std::atomic<uint32_t> identity_ready{0};  // host sets 1 (release) AFTER the strings are filled
    uint32_t identity_pad = 0;                // keep the char arrays 8-byte aligned
    char renderer[kRingIdentMax] = {0};       // host glGetString(GL_RENDERER), NUL-terminated
    char vendor[kRingIdentMax] = {0};         // host glGetString(GL_VENDOR)
    char gl_version[kRingIdentMax] = {0};     // host glGetString(GL_VERSION)
};

// Layout lock: the hot-path fields must keep their original offsets (the C producer
// in alr_gpu_ring_c.h mirrors them byte-for-byte), and the identity block must sit at
// the documented offsets so the C side reads the same bytes the host wrote.
static_assert(offsetof(RingHeader, head) == 16, "head@16");
static_assert(offsetof(RingHeader, tail) == 24, "tail@24");
static_assert(offsetof(RingHeader, reply_seq) == 32, "reply_seq@32");
static_assert(offsetof(RingHeader, req_seq) == 36, "req_seq@36");
static_assert(offsetof(RingHeader, closed) == 40, "closed@40");
static_assert(offsetof(RingHeader, pad2) == 44, "pad2@44");
static_assert(offsetof(RingHeader, identity_ready) == 48, "identity_ready@48");
static_assert(offsetof(RingHeader, renderer) == 56, "renderer@56");
static_assert(offsetof(RingHeader, vendor) == 56 + kRingIdentMax, "vendor follows renderer");
static_assert(offsetof(RingHeader, gl_version) == 56 + 2 * kRingIdentMax, "gl_version follows vendor");
static_assert(sizeof(RingHeader) == 56 + 3 * kRingIdentMax, "RingHeader == 248 bytes");

// Fill the header's identity strings (host glGetString values) and publish them with a
// release store so a guest that observes identity_ready != 0 (acquire) sees fully-written
// strings. Idempotent and safe to call from the host's GL thread once a context is current.
// Each string is truncated to kRingIdentMax-1 chars + NUL. A null/empty arg leaves that
// field empty (the guest then keeps its synthetic fallback for that one).
inline void ring_set_identity(void* region, const char* renderer,
                              const char* vendor, const char* version) {
    if (region == nullptr) return;
    auto* h = static_cast<RingHeader*>(region);
    auto copy = [](char* dst, const char* src) {
        if (src == nullptr) { dst[0] = '\0'; return; }
        std::size_t n = 0;
        for (; n < kRingIdentMax - 1 && src[n] != '\0'; ++n) dst[n] = src[n];
        dst[n] = '\0';
    };
    copy(h->renderer, renderer);
    copy(h->vendor, vendor);
    copy(h->gl_version, version);
    // Publish AFTER the strings are written so the guest's acquire-load of identity_ready
    // implies the bytes are visible.
    h->identity_ready.store(1u, std::memory_order_release);
}

// Total bytes a caller must allocate (and zero) for a ring of `ring_bytes` data.
inline size_t ring_region_size(uint32_t ring_bytes) {
    return sizeof(RingHeader) + ring_bytes;
}

// Initialize a freshly-allocated (zeroed) shared region as an ALR GPU ring.
// `ring_bytes` MUST be a power of two. Call once, from the side that creates the
// region (the parent, before fork).
inline bool ring_init(void* region, uint32_t ring_bytes) {
    if (region == nullptr || ring_bytes == 0 || (ring_bytes & (ring_bytes - 1)) != 0) {
        return false;
    }
    auto* h = static_cast<RingHeader*>(region);
    h->magic = RingHeader::kMagic;
    h->version = RingHeader::kVersion;
    h->ring_bytes = ring_bytes;
    h->pad = 0;
    h->head.store(0, std::memory_order_relaxed);
    h->tail.store(0, std::memory_order_relaxed);
    h->reply_seq.store(0, std::memory_order_relaxed);
    h->req_seq.store(0, std::memory_order_relaxed);
    h->closed.store(0, std::memory_order_relaxed);
    h->pad2.store(0, std::memory_order_relaxed);
    // Identity not yet known (the host fills it from glGetString once a GL context is
    // current via ring_set_identity); the guest stays on its synthetic fallback until then.
    h->identity_ready.store(0, std::memory_order_relaxed);
    h->identity_pad = 0;
    h->renderer[0] = '\0';
    h->vendor[0] = '\0';
    h->gl_version[0] = '\0';
    return true;
}

inline bool ring_valid(const void* region) {
    if (!region) return false;
    const auto* h = static_cast<const RingHeader*>(region);
    return h->magic == RingHeader::kMagic && h->version == RingHeader::kVersion &&
           h->ring_bytes != 0 && (h->ring_bytes & (h->ring_bytes - 1)) == 0;
}

// ---- Producer (guest side): append bytes, flush, wait for a sync reply. ----
class RingProducer {
public:
    explicit RingProducer(void* region)
        : h_(static_cast<RingHeader*>(region)),
          data_(static_cast<uint8_t*>(region) + sizeof(RingHeader)),
          mask_(h_ ? h_->ring_bytes - 1 : 0) {}

    bool valid() const { return h_ != nullptr && ring_valid(h_); }

    // Free space available to the producer (ring_bytes - in-flight). One byte is
    // intentionally never used so head==tail unambiguously means "empty".
    uint64_t free_bytes() const {
        const uint64_t head = h_->head.load(std::memory_order_relaxed);
        const uint64_t tail = h_->tail.load(std::memory_order_acquire);
        const uint64_t used = head - tail;
        return (h_->ring_bytes - 1) - used;
    }

    // Append `len` bytes. Returns false if they don't currently fit (caller should
    // flush + wait for the consumer to drain, then retry — back-pressure). Does NOT
    // block; blocking belongs to the sync points (flush_and_wait).
    bool append(const void* src, uint32_t len) {
        if (!valid() || len == 0) return len == 0;
        if (free_bytes() < len) return false;
        uint64_t head = h_->head.load(std::memory_order_relaxed);
        const uint32_t off = static_cast<uint32_t>(head & mask_);
        const uint32_t first = (off + len <= h_->ring_bytes) ? len : (h_->ring_bytes - off);
        std::memcpy(data_ + off, src, first);
        if (first < len) {
            std::memcpy(data_, static_cast<const uint8_t*>(src) + first, len - first);
        }
        // Publish the new head with release so the consumer sees the bytes first.
        h_->head.store(head + len, std::memory_order_release);
        return true;
    }

    // Signal the consumer that a sync request (e.g. eglSwapBuffers) is pending and
    // wait until it bumps reply_seq past our request. `spin_then_yield` keeps it
    // header-only (the real build wires an eventfd; this works for the host
    // self-test and as a fallback). Returns the reply_seq observed.
    uint32_t flush_and_wait(unsigned spin = 1u << 20) {
        const uint32_t want = h_->req_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
        for (unsigned i = 0; i < spin; ++i) {
            if (h_->reply_seq.load(std::memory_order_acquire) >= want) break;
            if (h_->closed.load(std::memory_order_acquire)) break;
        }
        return h_->reply_seq.load(std::memory_order_acquire);
    }

    void close() { if (h_) h_->closed.store(1, std::memory_order_release); }

private:
    RingHeader* h_;
    uint8_t* data_;
    uint32_t mask_;
};

// ---- Consumer (host side): peek available bytes, copy a contiguous snapshot,
// advance tail, post a reply. The host decoder runs decode_batch() on the
// snapshot (it needs a contiguous buffer; the ring may wrap, so we linearize). ----
class RingConsumer {
public:
    explicit RingConsumer(void* region)
        : h_(static_cast<RingHeader*>(region)),
          data_(static_cast<uint8_t*>(region) + sizeof(RingHeader)),
          mask_(h_ ? h_->ring_bytes - 1 : 0) {}

    bool valid() const { return h_ != nullptr && ring_valid(h_); }

    uint64_t available() const {
        const uint64_t head = h_->head.load(std::memory_order_acquire);
        const uint64_t tail = h_->tail.load(std::memory_order_relaxed);
        return head - tail;
    }

    // Copy up to `out_cap` available bytes into `out` (linearizing a wrap), WITHOUT
    // advancing tail. Returns the number copied. The caller decodes them, then calls
    // advance(n) once it has consumed n bytes.
    uint32_t snapshot(uint8_t* out, uint32_t out_cap) {
        const uint64_t avail = available();
        const uint32_t n = static_cast<uint32_t>(avail < out_cap ? avail : out_cap);
        if (n == 0) return 0;
        const uint64_t tail = h_->tail.load(std::memory_order_relaxed);
        const uint32_t off = static_cast<uint32_t>(tail & mask_);
        const uint32_t first = (off + n <= h_->ring_bytes) ? n : (h_->ring_bytes - off);
        std::memcpy(out, data_ + off, first);
        if (first < n) std::memcpy(out + first, data_, n - first);
        return n;
    }

    void advance(uint32_t n) {
        const uint64_t tail = h_->tail.load(std::memory_order_relaxed);
        h_->tail.store(tail + n, std::memory_order_release);
    }

    // Post a sync reply (host finished a swap/SYNC). Matches producer's req_seq.
    void post_reply() {
        const uint32_t req = h_->req_seq.load(std::memory_order_acquire);
        h_->reply_seq.store(req, std::memory_order_release);
    }

    bool producer_closed() const { return h_->closed.load(std::memory_order_acquire) != 0; }

private:
    RingHeader* h_;
    uint8_t* data_;
    uint32_t mask_;
};

}  // namespace alr::gpu

#endif  // ALR_GPU_ALR_GPU_RING_HPP
