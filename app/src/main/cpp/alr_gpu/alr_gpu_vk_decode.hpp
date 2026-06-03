// ALR GPU Vulkan command-stream decoder — VK-M2 first step (HOST half).
//
// The HOST side of the Vulkan enumerate/props marshalling path. It drains a request
// op stream (the wire defined in alr_gpu_vk_proto.hpp) and replays it as REAL Vulkan
// on the device's vendor Mali libvulkan (the only hardware path on a non-root Mali —
// strategy §2/§3), then encodes a reply op stream the guest decodes to learn the
// device count + each device's props. This is the Vulkan twin of alr_gpu_decode.hpp's
// decode_batch() (the GLES host decoder).
//
// WHY enumerate/props FIRST (and not draw): it is the smallest CLOSED round-trip that
// exercises the whole backbone — request encode -> ring -> host decode -> real
// vkCreateInstance/vkEnumeratePhysicalDevices/vkGetPhysicalDeviceProperties on Mali ->
// reply encode -> ring -> guest decode — WITHOUT a render pipeline. It proves the
// boundary + the client-side virtual-handle model end to end, which the heavy VK-M2/M3
// command-buffer work then builds on. (Mirrors how the GLES track proved clear/scissor
// marshalling before the full shader/VBO/texture/draw path.)
//
// CLIENT-SIDE VIRTUAL HANDLES: the guest hands us virtual VkInstance / VkPhysicalDevice
// ids; VkDecodeState owns the virtual->real translation, exactly like alr::gpu::HostState
// does for GL objects. The guest never sees a real Vulkan handle.
//
// VK_USE_PLATFORM_ANDROID_KHR + <vulkan/vulkan.h> are only pulled when ALR_VK_DECODE_REAL
// is defined (the on-device build path, via runtime_report.cpp). The default build is a
// HEADERLESS WIRE codec — no <vulkan.h> needed — so the host self-test + the native wire
// test compile and run on macOS/Linux with NO Vulkan SDK, proving the marshalling round
// trip independently of any GPU. On device, defining ALR_VK_DECODE_REAL swaps the
// "produce props from real Mali" path in. Header-only + self-contained (alr_gpu/** rule).

#ifndef ALR_GPU_ALR_GPU_VK_DECODE_HPP
#define ALR_GPU_ALR_GPU_VK_DECODE_HPP

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "alr_gpu/alr_gpu_vk_proto.hpp"

#ifdef ALR_VK_DECODE_REAL
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>
#include <android/hardware_buffer.h>  // VK-M2 body: AHB-backed clear render target
#include <utility>                    // std::pair (real_pool/real_cmd maps)
#endif

namespace alr::gpu {

// ---- A bounds-checked little-endian cursor (same shape as alr::gpu::Reader in
// alr_gpu_decode.hpp; duplicated here to keep this header standalone — no GLES
// include). Reads the request stream AND, on the guest, the reply stream. ----
class VkReader {
public:
    VkReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool u8(uint8_t& v) { return take(&v, 1); }
    bool u16(uint16_t& v) { return take(&v, 2); }  // generated sub-opcode width
    bool u32(uint32_t& v) { return take(&v, 4); }
    bool u64(uint64_t& v) { return take(&v, 8); }
    bool i32(int32_t& v) { return take(&v, 4); }
    bool f32(float& v) { return take(&v, 4); }
    bool blob(const uint8_t*& data, uint32_t& len) {
        if (!u32(len)) return false;
        if (pos_ + len > n_) return false;
        data = p_ + pos_;
        pos_ += len;
        return true;
    }
    bool done() const { return pos_ >= n_; }
    size_t pos() const { return pos_; }

private:
    template <typename T>
    bool take(T* out, size_t bytes) {
        if (pos_ + bytes > n_) return false;
        std::memcpy(out, p_ + pos_, bytes);
        pos_ += bytes;
        return true;
    }
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
};

// A growable little-endian byte builder for the REPLY stream (the host writes,
// the guest reads). Same encoding as the C AlrVkEncoder but std::vector-backed.
class VkReplyEncoder {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }  // generated reply sub-opcode width
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void i32(int32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void blob(const void* p, uint32_t n) {
        u32(n);
        if (n) raw(p, n);
    }
    void str(const std::string& s) { blob(s.data(), static_cast<uint32_t>(s.size())); }
    const std::vector<uint8_t>& bytes() const { return buf_; }

private:
    void raw(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    std::vector<uint8_t> buf_;
};

// One physical device's properties, in the host's own struct (decoupled from
// <vulkan.h> so the wire test can construct/compare them without a Vulkan SDK).
struct VkPhysProps {
    uint32_t api_version = 0;
    uint32_t driver_version = 0;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t device_type = 0;  // AlrVkPhysDeviceType
    std::string device_name;
    bool is_software = false;
    struct QF {
        uint32_t flags = 0;
        uint32_t count = 0;
    };
    std::vector<QF> queue_families;
};

// One image-format query's result, in the host's own struct (decoupled from <vulkan.h>
// so the wire test can construct/compare it without a Vulkan SDK). Mirrors the fields of
// VkImageFormatProperties + the VkResult the real Mali driver returned (a negative
// vk_result means the format/usage tuple is unsupported — a valid answer).
struct VkImageFmtProps {
    int32_t  vk_result = 0;       // 0 == VK_SUCCESS; <0 == VK_ERROR_FORMAT_NOT_SUPPORTED etc.
    uint32_t max_extent_w = 0;
    uint32_t max_extent_h = 0;
    uint32_t max_extent_d = 0;
    uint32_t max_mip_levels = 0;
    uint32_t max_array_layers = 0;
    uint32_t sample_counts = 0;   // VkSampleCountFlags
    uint64_t max_resource_size = 0;
};

// The clear a CMD_BEGIN_CLEAR recorded into a virtual command buffer, plus the geometry
// of the offscreen target it renders into. Held until the matching QUEUE_SUBMIT replays
// it. Decoupled from <vulkan.h> so the wire test can drive it with no SDK.
//
// VK-M3 breadth: the SAME record carries a DRAW request (is_draw=true). For a draw, the
// f32 clear[] is the BACKGROUND the render pass clears to, and the submit additionally
// binds a graphics pipeline + vertex buffer and issues one vkCmdDraw of a triangle whose
// color comes from the host fragment shader (kAlrVkTriColor*). is_draw=false = bare clear.
struct VkClearRecord {
    bool recorded = false;
    bool is_draw = false;  // VK-M3: clear-then-draw-triangle vs. bare clear
    uint32_t width = 0;
    uint32_t height = 0;
    float clear[4] = {0, 0, 0, 0};  // RGBA, 0..1 (the DRAW's background when is_draw)

    // ---- VK-M4 (PRESENT rung): a clear+draw that uses the GUEST'S own shader modules
    // and renders into a swapchain image (vs. VK-M3's host-embedded SPIR-V + throwaway
    // AHB). When is_present_draw, QUEUE_PRESENT runs vk_real_draw_present (guest shaders
    // -> swapchain AHB -> compositor) instead of vk_real_draw_submit. ----
    bool is_present_draw = false;
    uint32_t vswapchain = 0;
    uint32_t image_index = 0;
    uint32_t vvert = 0;       // guest vertex shader-module virtual id
    uint32_t vfrag = 0;       // guest fragment shader-module virtual id
};

// ---------------------------------------------------------------------------
// VK-M4 present sink — how the host routes a finished swapchain AHB to the on-screen
// compositor WITHOUT this decode header depending on alr_wayland/**. The host servicer
// (alr_gpu_vk_host_service.hpp) installs a sink that forwards to
// alr::wayland::alr_wayland_submit_gpu_frame; a headless self-test leaves it null (the
// present still renders + reads back, just isn't displayed). The AHardwareBuffer* is
// carried as void* so this declaration is header-compilable with no Android headers.
//   ahb    : the finished swapchain image's AHardwareBuffer* (BORROWED for the call;
//            the sink acquires its own ref if it retains it — matches §5-C contract).
//   w / h  : pixel dimensions.  serial : monotonic present serial (pacing/debug).
// Returns true if the frame was accepted by a real sink (1 -> ALR_VK_REPLY_PRESENT
// presented=1). A null sink returns false (presented=0).
// ---------------------------------------------------------------------------
using VkPresentSink = bool (*)(void* ahb, int w, int h, uint64_t serial);
inline VkPresentSink& vk_present_sink() {
    static VkPresentSink sink = nullptr;
    return sink;
}
inline void set_vk_present_sink(VkPresentSink s) { vk_present_sink() = s; }

// The fixed triangle color the VK-M3 draw fragment shader emits (kAlrVkTri*Spv below).
// MUST equal the vec4 constant baked into the fragment SPIR-V — the wire test + the host
// readback assert the center pixel ~matches this (and that it DIFFERS from the clear
// background, proving a draw, not a clear). 0.95/0.10/0.80 -> ~242/26/204 in 8-bit UNORM.
inline constexpr float kAlrVkTriColorR = 0.95f;
inline constexpr float kAlrVkTriColorG = 0.10f;
inline constexpr float kAlrVkTriColorB = 0.80f;
inline constexpr float kAlrVkTriColorA = 1.00f;

// ---------------------------------------------------------------------------
// Handcrafted tiny SPIR-V for the VK-M3 draw-breadth triangle pipeline.
//
// HOW THESE WERE PRODUCED (reproducible with the NDK's bundled glslc — no extra deps):
//   $NDK/shader-tools/<host>/glslc --target-env=vulkan1.1 -O alr_tri.vert -o v.spv
//   $NDK/shader-tools/<host>/glslc --target-env=vulkan1.1 -O alr_tri.frag -o f.spv
//   # then emit each .spv as little-endian uint32 words (python: struct.unpack('<I',...)).
//
// GLSL SOURCES (kept here so the bytes are auditable / regenerable):
//   alr_tri.vert (#version 450):
//       layout(location = 0) in vec2 inPos;
//       void main() { gl_Position = vec4(inPos, 0.0, 1.0); }
//   alr_tri.frag (#version 450):
//       layout(location = 0) out vec4 outColor;
//       void main() { outColor = vec4(0.95, 0.10, 0.80, 1.0); }
//
// The frag color is a BAKED constant (no push constant / no descriptor set) so the
// VkPipelineLayout is empty — maximally portable on Mali — and the readback color is
// fixed = kAlrVkTriColor*. Vertex positions come from a bound vertex buffer (vec2 NDC),
// so the draw exercises vkCmdBindVertexBuffers + vkCmdDraw(3). SPIR-V version word is
// 0x00010300 (SPIR-V 1.3 / Vulkan 1.1), matching --target-env. Word[0] is the magic
// 0x07230203; a runtime guard below re-checks it before vkCreateShaderModule.
inline constexpr uint32_t kAlrVkTriVertSpv[] = {
    0x07230203u, 0x00010300u, 0x000d000au, 0x0000001bu, 0x00000000u, 0x00020011u,
    0x00000001u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu,
    0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0007000fu, 0x00000000u,
    0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000du, 0x00000012u, 0x00050048u,
    0x0000000bu, 0x00000000u, 0x0000000bu, 0x00000000u, 0x00050048u, 0x0000000bu,
    0x00000001u, 0x0000000bu, 0x00000001u, 0x00050048u, 0x0000000bu, 0x00000002u,
    0x0000000bu, 0x00000003u, 0x00050048u, 0x0000000bu, 0x00000003u, 0x0000000bu,
    0x00000004u, 0x00030047u, 0x0000000bu, 0x00000002u, 0x00040047u, 0x00000012u,
    0x0000001eu, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u,
    0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u,
    0x00000006u, 0x00000004u, 0x00040015u, 0x00000008u, 0x00000020u, 0x00000000u,
    0x0004002bu, 0x00000008u, 0x00000009u, 0x00000001u, 0x0004001cu, 0x0000000au,
    0x00000006u, 0x00000009u, 0x0006001eu, 0x0000000bu, 0x00000007u, 0x00000006u,
    0x0000000au, 0x0000000au, 0x00040020u, 0x0000000cu, 0x00000003u, 0x0000000bu,
    0x0004003bu, 0x0000000cu, 0x0000000du, 0x00000003u, 0x00040015u, 0x0000000eu,
    0x00000020u, 0x00000001u, 0x0004002bu, 0x0000000eu, 0x0000000fu, 0x00000000u,
    0x00040017u, 0x00000010u, 0x00000006u, 0x00000002u, 0x00040020u, 0x00000011u,
    0x00000001u, 0x00000010u, 0x0004003bu, 0x00000011u, 0x00000012u, 0x00000001u,
    0x0004002bu, 0x00000006u, 0x00000014u, 0x00000000u, 0x0004002bu, 0x00000006u,
    0x00000015u, 0x3f800000u, 0x00040020u, 0x00000019u, 0x00000003u, 0x00000007u,
    0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u,
    0x00000005u, 0x0004003du, 0x00000010u, 0x00000013u, 0x00000012u, 0x00050051u,
    0x00000006u, 0x00000016u, 0x00000013u, 0x00000000u, 0x00050051u, 0x00000006u,
    0x00000017u, 0x00000013u, 0x00000001u, 0x00070050u, 0x00000007u, 0x00000018u,
    0x00000016u, 0x00000017u, 0x00000014u, 0x00000015u, 0x00050041u, 0x00000019u,
    0x0000001au, 0x0000000du, 0x0000000fu, 0x0003003eu, 0x0000001au, 0x00000018u,
    0x000100fdu, 0x00010038u};
inline constexpr uint32_t kAlrVkTriFragSpv[] = {
    0x07230203u, 0x00010300u, 0x000d000au, 0x0000000fu, 0x00000000u, 0x00020011u,
    0x00000001u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu,
    0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0006000fu, 0x00000004u,
    0x00000004u, 0x6e69616du, 0x00000000u, 0x00000009u, 0x00030010u, 0x00000004u,
    0x00000007u, 0x00040047u, 0x00000009u, 0x0000001eu, 0x00000000u, 0x00020013u,
    0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u,
    0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040020u,
    0x00000008u, 0x00000003u, 0x00000007u, 0x0004003bu, 0x00000008u, 0x00000009u,
    0x00000003u, 0x0004002bu, 0x00000006u, 0x0000000au, 0x3f733333u, 0x0004002bu,
    0x00000006u, 0x0000000bu, 0x3dcccccdu, 0x0004002bu, 0x00000006u, 0x0000000cu,
    0x3f4ccccdu, 0x0004002bu, 0x00000006u, 0x0000000du, 0x3f800000u, 0x0007002cu,
    0x00000007u, 0x0000000eu, 0x0000000au, 0x0000000bu, 0x0000000cu, 0x0000000du,
    0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u,
    0x00000005u, 0x0003003eu, 0x00000009u, 0x0000000eu, 0x000100fdu, 0x00010038u};

// The triangle's NDC vertices (vec2 each), big enough to cover the framebuffer center
// so the center-pixel readback is always inside the triangle (a fan around origin).
struct AlrVkVert2 { float x, y; };
inline constexpr AlrVkVert2 kAlrVkTriVerts[3] = {
    {0.0f, -0.8f}, {-0.8f, 0.8f}, {0.8f, 0.8f}};

// Host decode state: virtual instance/physical-device/device/queue/pool/cmd ids -> the
// host resources behind them. (The real Vulkan handles live in the real_* maps only on
// the device build; the wire test populates props_/clears_ directly.) Mirrors HostState.
struct VkDecodeState {
    std::map<uint32_t, bool> instances;            // vinst -> created
    std::map<uint32_t, uint32_t> enum_base;        // vinst -> vphys_base assigned
    std::map<uint32_t, uint32_t> enum_count;       // vinst -> device count
    std::map<uint32_t, VkPhysProps> props;         // vphys -> resolved props
    std::map<uint32_t, bool> devices;              // vdev -> created
    std::map<uint32_t, bool> queues;               // vqueue -> bound
    std::map<uint32_t, bool> pools;                // vpool -> created
    std::map<uint32_t, bool> cmds;                 // vcmd -> allocated
    std::map<uint32_t, VkClearRecord> clears;      // vcmd -> pending clear record
    std::map<uint32_t, uint32_t> swapchains;       // vswapchain -> image_count (wire-mode)
    std::map<uint32_t, bool> shaders;              // vshader -> created (wire-mode)
    bool ok = true;
    int decoded = 0;  // request ops dispatched
    uint64_t present_serial = 0;  // VK-M4: monotonic serial handed to the present sink

#ifdef ALR_VK_DECODE_REAL
    std::map<uint32_t, VkInstance> real_inst;      // vinst -> real VkInstance
    std::map<uint32_t, VkPhysicalDevice> real_phys;// vphys -> real VkPhysicalDevice
    // VK-M2 body: real logical-device objects, keyed by their virtual ids.
    struct RealDevice {
        VkPhysicalDevice phys = VK_NULL_HANDLE;
        VkDevice dev = VK_NULL_HANDLE;
        uint32_t gfx_family = 0;
        // FULL DEVICE PASSTHROUGH: the queue families+counts this device was ACTUALLY
        // created with (family -> queue_count). For the coarse CREATE_DEVICE path this is
        // { gfx_family -> 1 }; for CREATE_DEVICE2 it mirrors the client's queue-create
        // list. GET_DEVICE_QUEUE2 consults this so it never asks the driver for a
        // (family,index) the device wasn't built with (the GetDeviceQueue null-deref).
        std::map<uint32_t, uint32_t> queue_counts;
    };
    std::map<uint32_t, RealDevice> real_dev;       // vdev -> logical device
    std::map<uint32_t, VkQueue> real_queue;        // vqueue -> queue
    std::map<uint32_t, std::pair<uint32_t, VkCommandPool>> real_pool;  // vpool -> (vdev, pool)
    std::map<uint32_t, std::pair<uint32_t, VkCommandBuffer>> real_cmd; // vcmd -> (vdev, cmd)

    // ---- VK-M4 (PRESENT rung): guest-supplied shader modules + AHB-backed swapchains.
    // A shader module is the guest's OWN SPIR-V vkCreateShaderModule'd on Mali. A
    // swapchain owns a persistent ring of AHB COLOR_ATTACHMENT images (the proven
    // round7 AHB target, kept alive across frames so a present can route an image's AHB
    // to the compositor and acquire/present can rotate). ----
    std::map<uint32_t, std::pair<uint32_t, VkShaderModule>> real_shader;  // vshader -> (vdev, mod)
    struct SwapImage {
        AHardwareBuffer* ahb = nullptr;   // the image's backing AHB (host owns one ref)
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct RealSwapchain {
        uint32_t vdev = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
        uint32_t next = 0;                // round-robin acquire cursor
        std::vector<SwapImage> images;    // image ring (each an AHB color attachment)
    };
    std::map<uint32_t, RealSwapchain> real_swapchain;  // vswapchain -> AHB image ring
#endif
};

// ---- software-name classifier (same heuristic as alr_gpu_vk.hpp's vk_name_software). ----
inline bool vk_decode_name_software(const std::string& name) {
    std::string n = name;
    for (auto& c : n) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return n.find("swiftshader") != std::string::npos || n.find("llvmpipe") != std::string::npos ||
           n.find("lavapipe") != std::string::npos || n.find("softpipe") != std::string::npos ||
           n.find("software") != std::string::npos;
}

// Append one ALR_VK_REPLY_PHYS_PROPS record for `vphys` from `p` to the reply.
inline void encode_phys_props_reply(VkReplyEncoder& re, uint32_t vphys, const VkPhysProps& p) {
    re.u8(static_cast<uint8_t>(ALR_VK_REPLY_PHYS_PROPS));
    re.u32(vphys);
    re.u32(p.api_version);
    re.u32(p.driver_version);
    re.u32(p.vendor_id);
    re.u32(p.device_id);
    re.u32(p.device_type);
    re.str(p.device_name);
    re.u32(static_cast<uint32_t>(p.queue_families.size()));
    for (const auto& qf : p.queue_families) {
        re.u32(qf.flags);
        re.u32(qf.count);
    }
    re.u8(p.is_software ? 1u : 0u);
}

#ifdef ALR_VK_DECODE_REAL
// Bring up a real VkInstance on the vendor Mali libvulkan and resolve props for the
// requested virtual device. Returns the VkResult of instance creation; fills `st`.
// (Only compiled on the device path — runtime_report.cpp defines ALR_VK_DECODE_REAL.)
inline VkResult vk_real_create_instance(VkDecodeState& st, uint32_t vinst, uint32_t app_api) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "ALR VK-M2";
    app.apiVersion = app_api ? app_api : VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, nullptr, &inst);
    if (r == VK_SUCCESS) {
        st.real_inst[vinst] = inst;
        st.instances[vinst] = true;
    }
    return r;
}

inline uint32_t vk_real_enumerate(VkDecodeState& st, uint32_t vinst, uint32_t vphys_base) {
    auto it = st.real_inst.find(vinst);
    if (it == st.real_inst.end()) return 0;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(it->second, &n, nullptr);
    if (n == 0) return 0;
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(it->second, &n, devs.data());
    for (uint32_t i = 0; i < n; ++i) st.real_phys[vphys_base + i] = devs[i];
    return n;
}

inline bool vk_real_props(VkDecodeState& st, uint32_t vphys, VkPhysProps& out) {
    auto it = st.real_phys.find(vphys);
    if (it == st.real_phys.end()) return false;
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(it->second, &p);
    out.api_version = p.apiVersion;
    out.driver_version = p.driverVersion;
    out.vendor_id = p.vendorID;
    out.device_id = p.deviceID;
    out.device_type = static_cast<uint32_t>(p.deviceType);
    out.device_name = p.deviceName;
    out.is_software = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) ||
                      vk_decode_name_software(out.device_name);
    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(it->second, &nqf, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nqf);
    vkGetPhysicalDeviceQueueFamilyProperties(it->second, &nqf, qf.data());
    for (uint32_t i = 0; i < nqf; ++i)
        out.queue_families.push_back({static_cast<uint32_t>(qf[i].queueFlags), qf[i].queueCount});
    return true;
}

inline void vk_real_destroy_instance(VkDecodeState& st, uint32_t vinst) {
    auto it = st.real_inst.find(vinst);
    if (it != st.real_inst.end()) {
        vkDestroyInstance(it->second, nullptr);
        st.real_inst.erase(it);
    }
    st.instances.erase(vinst);
}

// ANGLE-init rung: query the REAL Mali vkGetPhysicalDeviceImageFormatProperties for the
// physical device behind virtual `vphys` and fill `out`. Returns true if `vphys` was a
// known device (out.vk_result then carries Mali's verdict — VK_SUCCESS or, validly,
// VK_ERROR_FORMAT_NOT_SUPPORTED); false if the virtual id was unknown to the host.
inline bool vk_real_image_format_props(VkDecodeState& st, uint32_t vphys, uint32_t format,
                                       uint32_t type, uint32_t tiling, uint32_t usage,
                                       uint32_t flags, VkImageFmtProps& out) {
    auto it = st.real_phys.find(vphys);
    if (it == st.real_phys.end()) return false;
    VkImageFormatProperties props{};
    VkResult r = vkGetPhysicalDeviceImageFormatProperties(
        it->second, static_cast<VkFormat>(format), static_cast<VkImageType>(type),
        static_cast<VkImageTiling>(tiling), static_cast<VkImageUsageFlags>(usage),
        static_cast<VkImageCreateFlags>(flags), &props);
    out.vk_result = static_cast<int32_t>(r);
    if (r == VK_SUCCESS) {
        out.max_extent_w = props.maxExtent.width;
        out.max_extent_h = props.maxExtent.height;
        out.max_extent_d = props.maxExtent.depth;
        out.max_mip_levels = props.maxMipLevels;
        out.max_array_layers = props.maxArrayLayers;
        out.sample_counts = static_cast<uint32_t>(props.sampleCounts);
        out.max_resource_size = static_cast<uint64_t>(props.maxResourceSize);
    }
    return true;
}

// ---- VK-M2 body, real Mali path: device + queue + command-buffer + clear-submit. ----

// The device extensions the clear-submit path NEEDS enabled at vkCreateDevice time. The
// AHB color target is imported via VK_ANDROID_external_memory_android_hardware_buffer; its
// device-level entry points (vkGetAndroidHardwareBufferPropertiesANDROID,
// VkImportAndroidHardwareBufferInfoANDROID) are ONLY legal to use once the extension was
// enabled on the logical device. Creating the device with NO extensions (the original
// VK-M2 bug) made vkGetDeviceProcAddr return the AHB fn but the import / image-bind was
// undefined, so the eventual vkQueueSubmit of a command buffer pointed at that broken
// color attachment failed on Mali. We mirror the VK-M1 keystone (alr_gpu_vk.hpp), which
// enables the AHB extension + its non-core dep.
inline constexpr const char* kAlrVkAhbExt =
    "VK_ANDROID_external_memory_android_hardware_buffer";

// Is `want` present in the device's extension list?
inline bool vk_real_dev_ext_present(VkPhysicalDevice phys, const char* want) {
    uint32_t n = 0;
    if (vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr) != VK_SUCCESS || n == 0)
        return false;
    std::vector<VkExtensionProperties> ext(n);
    if (vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, ext.data()) != VK_SUCCESS)
        return false;
    for (const auto& e : ext)
        if (std::strcmp(e.extensionName, want) == 0) return true;
    return false;
}

// Find a graphics queue family on a real physical device. Returns false if none.
inline bool vk_real_gfx_family(VkPhysicalDevice phys, uint32_t& family_out) {
    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, nullptr);
    if (nqf == 0) return false;
    std::vector<VkQueueFamilyProperties> qf(nqf);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, qf.data());
    for (uint32_t i = 0; i < nqf; ++i) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family_out = i; return true; }
    }
    return false;
}

// Create a real logical VkDevice (one graphics queue) on the real physical device behind
// virtual `vphys`. Returns the VkResult; on success fills st.real_dev[vdev] and records
// the graphics family used in `gfx_family_out`.
inline VkResult vk_real_create_device(VkDecodeState& st, uint32_t vphys, uint32_t vdev,
                                      uint32_t& gfx_family_out) {
    auto it = st.real_phys.find(vphys);
    if (it == st.real_phys.end()) return VK_ERROR_INITIALIZATION_FAILED;
    VkPhysicalDevice phys = it->second;
    uint32_t family = 0;
    if (!vk_real_gfx_family(phys, family)) return VK_ERROR_INITIALIZATION_FAILED;
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    // Enable the AHB external-memory extension (+ its non-core dep if present) so the
    // clear-submit path may legally import the AHB color target. Without this the device
    // is valid but the AHB import is undefined, and the submit that consumes that color
    // attachment fails on Mali — the original VK-M2 clear-submit failure. Mirrors the
    // VK-M1 keystone (alr_gpu_vk.hpp). If the driver lacks the extension we create the
    // device anyway (the import will then fail loudly with ALR_VK_RENDER_TARGET_ALLOC,
    // which is more diagnosable than a silent submit failure).
    std::vector<const char*> dev_ext;
    if (vk_real_dev_ext_present(phys, kAlrVkAhbExt)) dev_ext.push_back(kAlrVkAhbExt);
    if (vk_real_dev_ext_present(phys, "VK_EXT_queue_family_foreign"))
        dev_ext.push_back("VK_EXT_queue_family_foreign");
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(dev_ext.size());
    dci.ppEnabledExtensionNames = dev_ext.empty() ? nullptr : dev_ext.data();
    VkDevice dev = VK_NULL_HANDLE;
    VkResult r = vkCreateDevice(phys, &dci, nullptr, &dev);
    if (r == VK_SUCCESS) {
        VkDecodeState::RealDevice rd;
        rd.phys = phys;
        rd.dev = dev;
        rd.gfx_family = family;
        rd.queue_counts[family] = 1;  // the single queue this coarse path created
        st.real_dev[vdev] = rd;
        gfx_family_out = family;
    }
    return r;
}

inline void vk_real_get_queue(VkDecodeState& st, uint32_t vdev, uint32_t queue_index,
                              uint32_t vqueue) {
    auto it = st.real_dev.find(vdev);
    if (it == st.real_dev.end()) return;
    VkQueue q = VK_NULL_HANDLE;
    vkGetDeviceQueue(it->second.dev, it->second.gfx_family, queue_index, &q);
    if (q != VK_NULL_HANDLE) st.real_queue[vqueue] = q;
}

// ---- FULL DEVICE PASSTHROUGH: the allowlist of pNext feature sTypes we forward to the
// real Mali vkCreateDevice. Each is a self-contained { sType, pNext, <bools/limits> }
// struct ANGLE may chain off VkDeviceCreateInfo.pNext (typically a single
// VkPhysicalDeviceFeatures2 plus a handful of core-promoted feature structs). We forward
// only KNOWN structs (so a malformed/unknown sType from the wire can never make the host
// driver walk a bogus pNext), reconstructing each into a fixed local buffer and relinking
// the chain. An unknown sType on the wire is DROPPED (the feature simply stays disabled —
// the conservative, conformant outcome), never blindly chained. ----
inline bool vk_passthrough_feature_stype_allowed(uint32_t s_type) {
    switch (s_type) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT:
            return true;
        default:
            return false;
    }
}

// FULL DEVICE PASSTHROUGH: create a real Mali VkDevice from the CLIENT's actual
// VkDeviceCreateInfo (queue families+counts, enabled device extensions, allowlisted pNext
// feature chain decoded from the wire). `qcis` is (family,count) pairs; `exts` the enabled
// device-extension names; `feat_bytes`/`feat_types` the allowlisted feature structs (each
// the WHOLE struct incl. its sType/pNext header, relinked here). On success fills
// st.real_dev[vdev] (incl. queue_counts) and reports the first graphics family used.
inline VkResult vk_real_create_device2(
    VkDecodeState& st, uint32_t vphys, uint32_t vdev,
    const std::vector<std::pair<uint32_t, uint32_t>>& qcis,
    const std::vector<std::string>& exts,
    const std::vector<std::vector<uint8_t>>& feat_bytes,
    const std::vector<uint32_t>& feat_types, uint32_t& gfx_family_out) {
    auto it = st.real_phys.find(vphys);
    if (it == st.real_phys.end()) {
        std::fprintf(stderr, "[alr-vk-host] create_device2 FAIL: vphys=%u not in real_phys "
                             "(map size=%zu)\n", vphys, st.real_phys.size());
        std::fflush(stderr);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkPhysicalDevice phys = it->second;

    // The device's available queue families (so we never request a count beyond what the
    // family supports, and so we can pick the graphics family to report back).
    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, nullptr);
    std::vector<VkQueueFamilyProperties> qfprops(nqf ? nqf : 1);
    if (nqf) vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, qfprops.data());

    // Build the queue-create list from the client's request, clamped to what each family
    // actually supports. If the client asked for nothing (defensive), fall back to one
    // queue on the first graphics family — the coarse path's behavior.
    static const float kPrios[64] = {
        1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1};
    std::vector<VkDeviceQueueCreateInfo> qci;
    std::map<uint32_t, uint32_t> created_counts;
    uint32_t first_gfx = UINT32_MAX;
    for (const auto& fc : qcis) {
        uint32_t fam = fc.first, want = fc.second ? fc.second : 1;
        if (nqf && fam >= nqf) continue;  // out-of-range family: skip (never request it)
        uint32_t avail = (nqf && fam < nqf) ? qfprops[fam].queueCount : want;
        uint32_t cnt = want < avail ? want : avail;
        if (cnt == 0) cnt = 1;
        if (cnt > 64) cnt = 64;
        VkDeviceQueueCreateInfo q{};
        q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        q.queueFamilyIndex = fam;
        q.queueCount = cnt;
        q.pQueuePriorities = kPrios;
        qci.push_back(q);
        created_counts[fam] = cnt;
        if (first_gfx == UINT32_MAX && (!nqf || (qfprops[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT)))
            first_gfx = fam;
    }
    if (qci.empty()) {
        uint32_t fam = 0;
        if (!vk_real_gfx_family(phys, fam)) return VK_ERROR_INITIALIZATION_FAILED;
        VkDeviceQueueCreateInfo q{};
        q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        q.queueFamilyIndex = fam;
        q.queueCount = 1;
        q.pQueuePriorities = kPrios;
        qci.push_back(q);
        created_counts[fam] = 1;
        first_gfx = fam;
    }
    if (first_gfx == UINT32_MAX) first_gfx = qci.front().queueFamilyIndex;

    // Intersect the client's enabled extensions with what Mali actually exposes (never
    // pass an extension the driver doesn't have — vkCreateDevice would reject the whole
    // create). ALSO ensure the AHB external-memory pair the host's own present path needs
    // is enabled even if the client didn't ask (our swapchain import depends on it), so the
    // passthrough device stays compatible with the AHB present rung.
    std::vector<const char*> dev_ext;
    auto add_ext_if_present = [&](const char* name) {
        if (vk_real_dev_ext_present(phys, name)) {
            for (const char* e : dev_ext) if (std::strcmp(e, name) == 0) return;
            dev_ext.push_back(name);
        }
    };
    // Keep the c_str storage alive for the duration of the create.
    std::vector<std::string> ext_store = exts;
    for (const auto& e : ext_store) {
        if (e.empty()) continue;
        // VK_KHR_swapchain is a real WSI extension Mali exposes, but our ICD's swapchain is
        // a host-side AHB rotation (no on-screen VkSurface), so enabling the driver's real
        // swapchain is unnecessary AND could pull in surface deps; the host present path
        // does NOT use the driver swapchain. Drop it (the guest ICD answers swapchain ops).
        if (e == "VK_KHR_swapchain") continue;
        if (vk_real_dev_ext_present(phys, e.c_str())) {
            bool dup = false;
            for (const char* p : dev_ext) if (e == p) { dup = true; break; }
            if (!dup) dev_ext.push_back(e.c_str());
        }
    }
    add_ext_if_present(kAlrVkAhbExt);
    add_ext_if_present("VK_EXT_queue_family_foreign");

    // Rebuild the allowlisted feature pNext chain from the wire bytes into stable local
    // storage, relinking each struct's pNext to the next. Every struct begins with
    // { VkStructureType sType; void* pNext; } so we can splice pNext after copying.
    // VkPhysicalDeviceFeatures2 (if present) is chained the same way; vkCreateDevice reads
    // .features from it. We do NOT also set dci.pEnabledFeatures (mutually exclusive with a
    // Features2 in pNext); if no Features2 came over the wire, pEnabledFeatures stays null
    // (all-core, the conservative default).
    std::vector<std::vector<uint8_t>> feat_store;  // owns the reconstructed structs
    feat_store.reserve(feat_bytes.size());
    struct Hdr { VkStructureType sType; void* pNext; };
    for (size_t i = 0; i < feat_bytes.size(); ++i) {
        if (i >= feat_types.size()) break;
        if (!vk_passthrough_feature_stype_allowed(feat_types[i])) continue;
        if (feat_bytes[i].size() < sizeof(Hdr)) continue;  // too small to be a real struct
        feat_store.push_back(feat_bytes[i]);
    }
    void* chain_head = nullptr;
    for (size_t i = feat_store.size(); i-- > 0;) {
        Hdr h{};
        std::memcpy(&h, feat_store[i].data(), sizeof(Hdr));
        h.pNext = chain_head;  // relink: this struct -> the previously-linked one
        std::memcpy(feat_store[i].data(), &h, sizeof(Hdr));
        chain_head = feat_store[i].data();
    }

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = chain_head;  // the allowlisted Features2 + feature-struct chain (may be null)
    dci.queueCreateInfoCount = static_cast<uint32_t>(qci.size());
    dci.pQueueCreateInfos = qci.data();
    dci.enabledExtensionCount = static_cast<uint32_t>(dev_ext.size());
    dci.ppEnabledExtensionNames = dev_ext.empty() ? nullptr : dev_ext.data();
    VkDevice dev = VK_NULL_HANDLE;
    VkResult r = vkCreateDevice(phys, &dci, nullptr, &dev);
    if (r != VK_SUCCESS) {
        // Diagnose a real-Mali create rejection (the forwarded ext/feature/queue set Mali
        // refused) so a device run pins WHY the full-passthrough device failed rather than
        // a bare VkResult on the wire. Quiet on success (no per-create spam).
        std::string el;
        for (const char* e : dev_ext) { el += e; el += ' '; }
        std::fprintf(stderr, "[alr-vk-host] create_device2 vphys=%u qci=%zu(fam0_cnt=%u) "
                             "ext=%zu[%s] feat=%zu -> VkResult=%d\n",
                     vphys, qci.size(),
                     created_counts.count(0) ? created_counts[0] : 0u,
                     dev_ext.size(), el.c_str(), feat_store.size(), (int)r);
        std::fflush(stderr);
    }
    if (r == VK_SUCCESS) {
        VkDecodeState::RealDevice rd;
        rd.phys = phys;
        rd.dev = dev;
        rd.gfx_family = first_gfx;
        rd.queue_counts = created_counts;
        st.real_dev[vdev] = rd;
        gfx_family_out = first_gfx;
    }
    return r;
}

// FULL DEVICE PASSTHROUGH: queue-family-aware get-device-queue. Binds vqueue to the real
// queue at (family,index) — but ONLY if the device was created with a queue there, so the
// driver's GetDeviceQueue can never null-deref on an un-created (family,index). If the
// requested family/index wasn't created, we fall back to the device's recorded gfx_family
// index 0 (a valid queue) so the client still gets a usable queue rather than a crash.
inline void vk_real_get_queue2(VkDecodeState& st, uint32_t vdev, uint32_t queue_family_index,
                               uint32_t queue_index, uint32_t vqueue) {
    auto it = st.real_dev.find(vdev);
    if (it == st.real_dev.end()) return;
    uint32_t fam = queue_family_index, idx = queue_index;
    auto qc = it->second.queue_counts.find(fam);
    if (qc == it->second.queue_counts.end() || idx >= qc->second) {
        // Not a queue this device was built with — use the known-good graphics queue 0.
        fam = it->second.gfx_family;
        idx = 0;
    }
    VkQueue q = VK_NULL_HANDLE;
    vkGetDeviceQueue(it->second.dev, fam, idx, &q);
    if (q != VK_NULL_HANDLE) st.real_queue[vqueue] = q;
}

inline VkResult vk_real_create_pool(VkDecodeState& st, uint32_t vdev, uint32_t vpool) {
    auto it = st.real_dev.find(vdev);
    if (it == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = it->second.gfx_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkResult r = vkCreateCommandPool(it->second.dev, &pci, nullptr, &pool);
    if (r == VK_SUCCESS) st.real_pool[vpool] = {vdev, pool};
    return r;
}

inline VkResult vk_real_alloc_cmd(VkDecodeState& st, uint32_t vdev, uint32_t vpool,
                                  uint32_t vcmd) {
    auto dit = st.real_dev.find(vdev);
    auto pit = st.real_pool.find(vpool);
    if (dit == st.real_dev.end() || pit == st.real_pool.end())
        return VK_ERROR_INITIALIZATION_FAILED;
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pit->second.second;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult r = vkAllocateCommandBuffers(dit->second.dev, &cbai, &cmd);
    if (r == VK_SUCCESS) st.real_cmd[vcmd] = {vdev, cmd};
    return r;
}

// Replay a CLEAR-only render into an AHB-backed COLOR_ATTACHMENT on the real Mali GPU,
// submitting `vcmd` on `vqueue`, then read the cleared center pixel back on the CPU. This
// is the device twin of alr_gpu_vk.hpp::run_vk_ahb_render_probe but driven by the virtual
// handles the guest shipped. Returns an AlrVkRenderResult; fills px[4] (RGBA 0..255).
// Self-contained (allocates+frees its own AHB/image/view/renderpass/framebuffer per call).
inline int vk_real_clear_submit(VkDecodeState& st, uint32_t vdev, uint32_t vqueue,
                                uint32_t vcmd, const VkClearRecord& rec, uint8_t px[4]) {
    px[0] = px[1] = px[2] = px[3] = 0;
    if (!rec.recorded) return ALR_VK_RENDER_NO_CLEAR_RECORDED;
    auto dit = st.real_dev.find(vdev);
    auto qit = st.real_queue.find(vqueue);
    auto cit = st.real_cmd.find(vcmd);
    if (dit == st.real_dev.end() || qit == st.real_queue.end() || cit == st.real_cmd.end())
        return ALR_VK_RENDER_NO_DEVICE;
    VkDevice dev = dit->second.dev;
    VkQueue queue = qit->second;
    VkCommandBuffer cmd = cit->second.second;
    const uint32_t w = rec.width ? rec.width : 64;
    const uint32_t h = rec.height ? rec.height : 64;

    // ---- AHB color target (GPU framebuffer + CPU readable) ----
    auto p_ahb_props = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
        vkGetDeviceProcAddr(dev, "vkGetAndroidHardwareBufferPropertiesANDROID"));
    if (!p_ahb_props) return ALR_VK_RENDER_TARGET_ALLOC;
    AHardwareBuffer_Desc d{};
    d.width = w;
    d.height = h;
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    AHardwareBuffer* ahb = nullptr;
    if (AHardwareBuffer_allocate(&d, &ahb) != 0 || ahb == nullptr)
        return ALR_VK_RENDER_TARGET_ALLOC;

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    int result = ALR_VK_RENDER_OK;
    auto cleanup = [&]() {
        if (fb) vkDestroyFramebuffer(dev, fb, nullptr);
        if (rp) vkDestroyRenderPass(dev, rp, nullptr);
        if (view) vkDestroyImageView(dev, view, nullptr);
        if (image) vkDestroyImage(dev, image, nullptr);
        if (mem) vkFreeMemory(dev, mem, nullptr);
        AHardwareBuffer_release(ahb);
    };

    VkAndroidHardwareBufferFormatPropertiesANDROID fmt_props{};
    fmt_props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferPropertiesANDROID ahb_props{};
    ahb_props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    ahb_props.pNext = &fmt_props;
    if (p_ahb_props(dev, ahb, &ahb_props) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_TARGET_ALLOC; }
    VkFormat color_fmt = (fmt_props.format != VK_FORMAT_UNDEFINED) ? fmt_props.format
                                                                   : VK_FORMAT_R8G8B8A8_UNORM;

    VkExternalMemoryImageCreateInfo ext_img{};
    ext_img.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo img_ci{};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.pNext = &ext_img;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = color_fmt;
    img_ci.extent = {w, h, 1};
    img_ci.mipLevels = 1;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &img_ci, nullptr, &image) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_TARGET_ALLOC; }

    uint32_t mem_type = 0;
    bool found_type = false;
    for (uint32_t i = 0; i < 32; ++i)
        if (ahb_props.memoryTypeBits & (1u << i)) { mem_type = i; found_type = true; break; }
    if (!found_type) { cleanup(); return ALR_VK_RENDER_TARGET_ALLOC; }
    VkImportAndroidHardwareBufferInfoANDROID import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    import_info.buffer = ahb;
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image;
    dedicated.pNext = &import_info;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &dedicated;
    mai.allocationSize = ahb_props.allocationSize;
    mai.memoryTypeIndex = mem_type;
    if (vkAllocateMemory(dev, &mai, nullptr, &mem) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_TARGET_ALLOC; }
    if (vkBindImageMemory(dev, image, mem, 0) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_TARGET_ALLOC; }

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = color_fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_TARGET_ALLOC; }

    VkAttachmentDescription att{};
    att.format = color_fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // The render pass resolves the attachment straight to GENERAL so the subsequent
    // queue-family-release barrier (-> EXTERNAL, for the CPU AHB read) has a defined source
    // layout. Leaving it in COLOR_ATTACHMENT_OPTIMAL and then CPU-reading the AHB on a
    // tiler (Mali) yields undefined contents — the readback half of the clear-submit gap.
    att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference att_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &att_ref;
    // Make the color write available + the GENERAL transition visible to anything that
    // reads the attachment after the pass (the queue-family-release barrier below, then
    // the host AHB lock). Without this dependency the store can race the readback.
    VkSubpassDependency dep{};
    dep.srcSubpass = 0;
    dep.dstSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = 0;
    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    if (vkCreateRenderPass(dev, &rpci, nullptr, &rp) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_RECORD; }

    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = rp;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &view;
    fbci.width = w;
    fbci.height = h;
    fbci.layers = 1;
    if (vkCreateFramebuffer(dev, &fbci, nullptr, &fb) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_RECORD; }

    // ---- record the CLEAR into the guest's command buffer ----
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_RECORD; }
    VkClearValue clear{};
    clear.color = {{rec.clear[0], rec.clear[1], rec.clear[2], rec.clear[3]}};
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = rp;
    rbi.framebuffer = fb;
    rbi.renderArea.extent = {w, h};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(cmd);
    // Release the AHB color image from the graphics queue to the external consumer (the
    // CPU that locks the AHB) so it sees fully-flushed pixels. On a tiler the framebuffer
    // lives in tile memory until this resolve/ownership transfer; the host-read destination
    // makes the write available to a host read of the AHB. (Image is already in GENERAL via
    // the render pass finalLayout.) VK_QUEUE_FAMILY_EXTERNAL is core in Vulkan 1.1, so this
    // is valid without the optional VK_EXT_queue_family_foreign extension.
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = dit->second.gfx_family;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_RECORD; }

    // ---- submit + fence-wait ----
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS) fence = VK_NULL_HANDLE;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) {
        if (fence) vkDestroyFence(dev, fence, nullptr);
        cleanup();
        return ALR_VK_RENDER_SUBMIT;
    }
    if (fence) {
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(dev, fence, nullptr);
    } else {
        vkQueueWaitIdle(queue);
    }

    // ---- read the AHB back and sample the center pixel ----
    AHardwareBuffer_Desc got{};
    AHardwareBuffer_describe(ahb, &got);
    void* cpu = nullptr;
    if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &cpu) == 0 &&
        cpu != nullptr) {
        const uint32_t stride_px = got.stride ? got.stride : w;
        const auto* base = static_cast<const unsigned char*>(cpu);
        const size_t off = (static_cast<size_t>(h / 2) * stride_px + (w / 2)) * 4;
        for (int i = 0; i < 4; ++i) px[i] = base[off + i];
        AHardwareBuffer_unlock(ahb, nullptr);
    } else {
        cleanup();
        return ALR_VK_RENDER_READBACK;
    }

    cleanup();
    return result;
}

// ---- VK-M3 draw-breadth, real Mali path: clear-then-DRAW one triangle. ----
//
// Extends vk_real_clear_submit beyond a bare clear: it builds a real GRAPHICS PIPELINE
// (vkCreateShaderModule × vert+frag handcrafted SPIR-V, vkCreatePipelineLayout (empty),
// vkCreateGraphicsPipelines with a vec2 vertex input), uploads the 3 NDC triangle verts
// into a HOST_VISIBLE VkBuffer (vkCreateBuffer + vkAllocateMemory + vkMapMemory), then
// records vkCmdBeginRenderPass(clear bg) / vkCmdBindPipeline / vkCmdBindVertexBuffers /
// vkCmdDraw(3) / vkCmdEndRenderPass into the guest's command buffer, submits + waits, and
// reads the AHB center pixel back. That pixel is the TRIANGLE color (kAlrVkTriColor*),
// distinct from the clear background — the proof a DRAW (not just a clear) reached Mali.
// Self-contained: allocates+frees its own AHB/image/view/renderpass/framebuffer/pipeline/
// vertex-buffer per call. Returns an AlrVkRenderResult; fills px[4] (RGBA 0..255).
inline int vk_real_draw_submit(VkDecodeState& st, uint32_t vdev, uint32_t vqueue,
                               uint32_t vcmd, const VkClearRecord& rec, uint8_t px[4]) {
    px[0] = px[1] = px[2] = px[3] = 0;
    if (!rec.recorded) return ALR_VK_RENDER_NO_CLEAR_RECORDED;
    auto dit = st.real_dev.find(vdev);
    auto qit = st.real_queue.find(vqueue);
    auto cit = st.real_cmd.find(vcmd);
    if (dit == st.real_dev.end() || qit == st.real_queue.end() || cit == st.real_cmd.end())
        return ALR_VK_RENDER_NO_DEVICE;
    VkDevice dev = dit->second.dev;
    VkPhysicalDevice phys = dit->second.phys;
    VkQueue queue = qit->second;
    VkCommandBuffer cmd = cit->second.second;
    const uint32_t w = rec.width ? rec.width : 64;
    const uint32_t h = rec.height ? rec.height : 64;

    // ---- AHB color target (GPU framebuffer + CPU readable) ----
    auto p_ahb_props = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
        vkGetDeviceProcAddr(dev, "vkGetAndroidHardwareBufferPropertiesANDROID"));
    if (!p_ahb_props) return ALR_VK_RENDER_TARGET_ALLOC;
    AHardwareBuffer_Desc d{};
    d.width = w;
    d.height = h;
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    AHardwareBuffer* ahb = nullptr;
    if (AHardwareBuffer_allocate(&d, &ahb) != 0 || ahb == nullptr)
        return ALR_VK_RENDER_TARGET_ALLOC;

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    // VK-M3 pipeline objects
    VkShaderModule vert_mod = VK_NULL_HANDLE, frag_mod = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer vbuf = VK_NULL_HANDLE;
    VkDeviceMemory vmem = VK_NULL_HANDLE;
    auto cleanup = [&]() {
        if (vbuf) vkDestroyBuffer(dev, vbuf, nullptr);
        if (vmem) vkFreeMemory(dev, vmem, nullptr);
        if (pipeline) vkDestroyPipeline(dev, pipeline, nullptr);
        if (pipe_layout) vkDestroyPipelineLayout(dev, pipe_layout, nullptr);
        if (frag_mod) vkDestroyShaderModule(dev, frag_mod, nullptr);
        if (vert_mod) vkDestroyShaderModule(dev, vert_mod, nullptr);
        if (fb) vkDestroyFramebuffer(dev, fb, nullptr);
        if (rp) vkDestroyRenderPass(dev, rp, nullptr);
        if (view) vkDestroyImageView(dev, view, nullptr);
        if (image) vkDestroyImage(dev, image, nullptr);
        if (mem) vkFreeMemory(dev, mem, nullptr);
        AHardwareBuffer_release(ahb);
    };

    VkAndroidHardwareBufferFormatPropertiesANDROID fmt_props{};
    fmt_props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferPropertiesANDROID ahb_props{};
    ahb_props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    ahb_props.pNext = &fmt_props;
    if (p_ahb_props(dev, ahb, &ahb_props) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_TARGET_ALLOC; }
    VkFormat color_fmt = (fmt_props.format != VK_FORMAT_UNDEFINED) ? fmt_props.format
                                                                   : VK_FORMAT_R8G8B8A8_UNORM;

    VkExternalMemoryImageCreateInfo ext_img{};
    ext_img.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo img_ci{};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.pNext = &ext_img;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = color_fmt;
    img_ci.extent = {w, h, 1};
    img_ci.mipLevels = 1;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &img_ci, nullptr, &image) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_TARGET_ALLOC; }

    uint32_t mem_type = 0;
    bool found_type = false;
    for (uint32_t i = 0; i < 32; ++i)
        if (ahb_props.memoryTypeBits & (1u << i)) { mem_type = i; found_type = true; break; }
    if (!found_type) { cleanup(); return ALR_VK_RENDER_TARGET_ALLOC; }
    VkImportAndroidHardwareBufferInfoANDROID import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    import_info.buffer = ahb;
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image;
    dedicated.pNext = &import_info;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &dedicated;
    mai.allocationSize = ahb_props.allocationSize;
    mai.memoryTypeIndex = mem_type;
    if (vkAllocateMemory(dev, &mai, nullptr, &mem) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_TARGET_ALLOC; }
    if (vkBindImageMemory(dev, image, mem, 0) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_TARGET_ALLOC; }

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = color_fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_TARGET_ALLOC; }

    VkAttachmentDescription att{};
    att.format = color_fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;  // see vk_real_clear_submit: GENERAL so the
                                                // queue-family-release + CPU read are defined
    VkAttachmentReference att_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &att_ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = 0;
    dep.dstSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = 0;
    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    if (vkCreateRenderPass(dev, &rpci, nullptr, &rp) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_RECORD; }

    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = rp;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &view;
    fbci.width = w;
    fbci.height = h;
    fbci.layers = 1;
    if (vkCreateFramebuffer(dev, &fbci, nullptr, &fb) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_RECORD; }

    // ---- VK-M3: shader modules (handcrafted SPIR-V), empty pipeline layout, pipeline ----
    if (kAlrVkTriVertSpv[0] != 0x07230203u || kAlrVkTriFragSpv[0] != 0x07230203u) {
        cleanup();
        return ALR_VK_RENDER_PIPELINE;  // embedded SPIR-V corrupt (magic mismatch)
    }
    VkShaderModuleCreateInfo vsm{};
    vsm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vsm.codeSize = sizeof(kAlrVkTriVertSpv);
    vsm.pCode = kAlrVkTriVertSpv;
    if (vkCreateShaderModule(dev, &vsm, nullptr, &vert_mod) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_PIPELINE; }
    VkShaderModuleCreateInfo fsm{};
    fsm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fsm.codeSize = sizeof(kAlrVkTriFragSpv);
    fsm.pCode = kAlrVkTriFragSpv;
    if (vkCreateShaderModule(dev, &fsm, nullptr, &frag_mod) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_PIPELINE; }

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;  // empty: no sets / no push
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &pipe_layout) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_PIPELINE; }

    // ---- vertex buffer (HOST_VISIBLE) holding the 3 NDC triangle verts ----
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = sizeof(kAlrVkTriVerts);
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bci, nullptr, &vbuf) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_PIPELINE; }
    VkMemoryRequirements vreq{};
    vkGetBufferMemoryRequirements(dev, vbuf, &vreq);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t vtype = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        const bool usable = (vreq.memoryTypeBits & (1u << i)) != 0;
        const bool host_visible =
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (usable && host_visible) { vtype = i; break; }
    }
    if (vtype == UINT32_MAX) { cleanup(); return ALR_VK_RENDER_PIPELINE; }
    VkMemoryAllocateInfo vai{};
    vai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    vai.allocationSize = vreq.size;
    vai.memoryTypeIndex = vtype;
    if (vkAllocateMemory(dev, &vai, nullptr, &vmem) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_PIPELINE; }
    if (vkBindBufferMemory(dev, vbuf, vmem, 0) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_PIPELINE; }
    void* vmap = nullptr;
    if (vkMapMemory(dev, vmem, 0, sizeof(kAlrVkTriVerts), 0, &vmap) != VK_SUCCESS || !vmap) {
        cleanup();
        return ALR_VK_RENDER_PIPELINE;
    }
    std::memcpy(vmap, kAlrVkTriVerts, sizeof(kAlrVkTriVerts));
    vkUnmapMemory(dev, vmem);  // HOST_COHERENT: no explicit flush needed

    // ---- graphics pipeline: vec2 vertex input -> vert -> frag (fixed color) ----
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vib{};
    vib.binding = 0;
    vib.stride = sizeof(AlrVkVert2);
    vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription via{};
    via.location = 0;
    via.binding = 0;
    via.format = VK_FORMAT_R32G32_SFLOAT;  // vec2 inPos
    via.offset = 0;
    VkPipelineVertexInputStateCreateInfo vis{};
    vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vis.vertexBindingDescriptionCount = 1;
    vis.pVertexBindingDescriptions = &vib;
    vis.vertexAttributeDescriptionCount = 1;
    vis.pVertexAttributeDescriptions = &via;

    VkPipelineInputAssemblyStateCreateInfo ias{};
    ias.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{};
    vp.width = static_cast<float>(w);
    vp.height = static_cast<float>(h);
    vp.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {w, h};
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;  // accept either winding so the center is always filled
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vis;
    gp.pInputAssemblyState = &ias;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.layout = pipe_layout;
    gp.renderPass = rp;
    gp.subpass = 0;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline) != VK_SUCCESS) {
        cleanup();
        return ALR_VK_RENDER_PIPELINE;
    }

    // ---- record: clear bg, bind pipeline + vertex buffer, draw 3, end ----
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_RECORD; }
    VkClearValue clear{};
    clear.color = {{rec.clear[0], rec.clear[1], rec.clear[2], rec.clear[3]}};
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = rp;
    rbi.framebuffer = fb;
    rbi.renderArea.extent = {w, h};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDeviceSize voff = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &voff);
    vkCmdDraw(cmd, 3, 1, 0, 0);  // the triangle
    vkCmdEndRenderPass(cmd);
    // Release the color image to the external (CPU) consumer (same as vk_real_clear_submit).
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = dit->second.gfx_family;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_RECORD; }

    // ---- submit + fence-wait ----
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS) fence = VK_NULL_HANDLE;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) {
        if (fence) vkDestroyFence(dev, fence, nullptr);
        cleanup();
        return ALR_VK_RENDER_SUBMIT;
    }
    if (fence) {
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(dev, fence, nullptr);
    } else {
        vkQueueWaitIdle(queue);
    }

    // ---- read the AHB center pixel back (should be the TRIANGLE color, not the bg) ----
    AHardwareBuffer_Desc got{};
    AHardwareBuffer_describe(ahb, &got);
    void* cpu = nullptr;
    if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &cpu) == 0 &&
        cpu != nullptr) {
        const uint32_t stride_px = got.stride ? got.stride : w;
        const auto* base = static_cast<const unsigned char*>(cpu);
        const size_t off = (static_cast<size_t>(h / 2) * stride_px + (w / 2)) * 4;
        for (int i = 0; i < 4; ++i) px[i] = base[off + i];
        AHardwareBuffer_unlock(ahb, nullptr);
    } else {
        cleanup();
        return ALR_VK_RENDER_READBACK;
    }

    cleanup();
    return ALR_VK_RENDER_OK;
}

// ===========================================================================
// VK-M4 (PRESENT rung) — guest-supplied SPIR-V shader modules + an AHB-backed
// swapchain whose images route to the in-app Wayland compositor.
// ===========================================================================

// Create the guest's OWN shader module from its SPIR-V blob on the real Mali device.
// `spirv`/`spirv_len` is the raw blob the guest shipped over the wire. Returns the
// VkResult; on success records st.real_shader[vshader]. We re-check the SPIR-V magic
// + 4-byte alignment up front so a corrupt/odd blob fails diagnosably rather than UB
// in the driver.
inline VkResult vk_real_create_shader_module(VkDecodeState& st, uint32_t vdev,
                                             uint32_t vshader, const uint8_t* spirv,
                                             uint32_t spirv_len) {
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    if (spirv == nullptr || spirv_len < 8 || (spirv_len % 4) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;  // SPIR-V is a u32 word stream
    uint32_t magic = 0;
    std::memcpy(&magic, spirv, 4);
    if (magic != 0x07230203u) return VK_ERROR_INITIALIZATION_FAILED;  // not SPIR-V
    // vkCreateShaderModule requires pCode 4-byte aligned; the wire blob may not be, so
    // copy into an aligned uint32 vector.
    std::vector<uint32_t> words(spirv_len / 4);
    std::memcpy(words.data(), spirv, spirv_len);
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = spirv_len;
    smci.pCode = words.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(dit->second.dev, &smci, nullptr, &mod);
    if (r == VK_SUCCESS) {
        st.real_shader[vshader] = {vdev, mod};
        st.shaders[vshader] = true;
    }
    return r;
}

inline void vk_real_destroy_shader_module(VkDecodeState& st, uint32_t vdev,
                                          uint32_t vshader) {
    auto sit = st.real_shader.find(vshader);
    if (sit == st.real_shader.end()) return;
    auto dit = st.real_dev.find(vdev);
    if (dit != st.real_dev.end() && sit->second.second != VK_NULL_HANDLE)
        vkDestroyShaderModule(dit->second.dev, sit->second.second, nullptr);
    st.real_shader.erase(sit);
    st.shaders.erase(vshader);
}

// Allocate ONE AHB-backed COLOR_ATTACHMENT image (the proven round7 target) into `out`,
// on device `dev`/`phys`. GPU_FRAMEBUFFER | GPU_SAMPLED_IMAGE so the compositor can also
// import it as a sampled external image; CPU_READ_OFTEN so a headless self-test reads
// the center pixel back. Returns true on success; fills out.{ahb,image,mem,view}.
// (No VkPhysicalDevice needed: AHB import derives its memory type from the AHB props'
// memoryTypeBits, not vkGetPhysicalDeviceMemoryProperties.)
inline bool vk_real_alloc_swap_image(VkDevice dev, uint32_t w,
                                     uint32_t h, VkFormat& color_fmt,
                                     VkDecodeState::SwapImage& out) {
    auto p_ahb_props = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
        vkGetDeviceProcAddr(dev, "vkGetAndroidHardwareBufferPropertiesANDROID"));
    if (!p_ahb_props) return false;
    AHardwareBuffer_Desc d{};
    d.width = w;
    d.height = h;
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    // GPU_FRAMEBUFFER (render into) + GPU_SAMPLED_IMAGE (compositor samples it as an
    // external-OES texture, the §5-C zero-copy path) + CPU_READ_OFTEN (headless readback).
    d.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
              AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    AHardwareBuffer* ahb = nullptr;
    if (AHardwareBuffer_allocate(&d, &ahb) != 0 || ahb == nullptr) return false;

    VkAndroidHardwareBufferFormatPropertiesANDROID fmt_props{};
    fmt_props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferPropertiesANDROID ahb_props{};
    ahb_props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    ahb_props.pNext = &fmt_props;
    if (p_ahb_props(dev, ahb, &ahb_props) != VK_SUCCESS) { AHardwareBuffer_release(ahb); return false; }
    color_fmt = (fmt_props.format != VK_FORMAT_UNDEFINED) ? fmt_props.format
                                                          : VK_FORMAT_R8G8B8A8_UNORM;

    VkExternalMemoryImageCreateInfo ext_img{};
    ext_img.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo img_ci{};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.pNext = &ext_img;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = color_fmt;
    img_ci.extent = {w, h, 1};
    img_ci.mipLevels = 1;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(dev, &img_ci, nullptr, &image) != VK_SUCCESS) {
        AHardwareBuffer_release(ahb);
        return false;
    }
    uint32_t mem_type = 0;
    bool found_type = false;
    for (uint32_t i = 0; i < 32; ++i)
        if (ahb_props.memoryTypeBits & (1u << i)) { mem_type = i; found_type = true; break; }
    if (!found_type) { vkDestroyImage(dev, image, nullptr); AHardwareBuffer_release(ahb); return false; }
    VkImportAndroidHardwareBufferInfoANDROID import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    import_info.buffer = ahb;
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image;
    dedicated.pNext = &import_info;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &dedicated;
    mai.allocationSize = ahb_props.allocationSize;
    mai.memoryTypeIndex = mem_type;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &mai, nullptr, &mem) != VK_SUCCESS ||
        vkBindImageMemory(dev, image, mem, 0) != VK_SUCCESS) {
        if (mem) vkFreeMemory(dev, mem, nullptr);
        vkDestroyImage(dev, image, nullptr);
        AHardwareBuffer_release(ahb);
        return false;
    }
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = color_fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS) {
        vkFreeMemory(dev, mem, nullptr);
        vkDestroyImage(dev, image, nullptr);
        AHardwareBuffer_release(ahb);
        return false;
    }
    out.ahb = ahb;
    out.image = image;
    out.mem = mem;
    out.view = view;
    return true;
}

inline void vk_real_free_swap_image(VkDevice dev, VkDecodeState::SwapImage& im) {
    if (im.view) vkDestroyImageView(dev, im.view, nullptr);
    if (im.image) vkDestroyImage(dev, im.image, nullptr);
    if (im.mem) vkFreeMemory(dev, im.mem, nullptr);
    if (im.ahb) AHardwareBuffer_release(im.ahb);
    im = VkDecodeState::SwapImage{};
}

// Create an AHB-backed swapchain: a persistent ring of `image_count` AHB color
// attachments (clamped 1..4). Returns the VkResult; fills st.real_swapchain[vswapchain]
// and `image_count_out` (what was actually allocated).
inline VkResult vk_real_create_swapchain(VkDecodeState& st, uint32_t vdev,
                                         uint32_t vswapchain, uint32_t width,
                                         uint32_t height, uint32_t image_count,
                                         uint32_t& image_count_out) {
    image_count_out = 0;
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    VkDevice dev = dit->second.dev;
    const uint32_t w = width ? width : 64;
    const uint32_t h = height ? height : 64;
    uint32_t n = image_count ? image_count : 2;
    if (n > 4) n = 4;  // bound the AHB ring (double/triple buffering is plenty)

    VkDecodeState::RealSwapchain sc;
    sc.vdev = vdev;
    sc.width = w;
    sc.height = h;
    sc.next = 0;
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    for (uint32_t i = 0; i < n; ++i) {
        VkDecodeState::SwapImage im;
        VkFormat got_fmt = fmt;
        if (!vk_real_alloc_swap_image(dev, w, h, got_fmt, im)) {
            for (auto& already : sc.images) vk_real_free_swap_image(dev, already);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        fmt = got_fmt;
        sc.images.push_back(im);
    }
    sc.format = fmt;
    st.real_swapchain[vswapchain] = std::move(sc);
    st.swapchains[vswapchain] = static_cast<uint32_t>(st.real_swapchain[vswapchain].images.size());
    image_count_out = static_cast<uint32_t>(st.real_swapchain[vswapchain].images.size());
    return VK_SUCCESS;
}

inline void vk_real_destroy_swapchain(VkDecodeState& st, uint32_t vswapchain) {
    auto it = st.real_swapchain.find(vswapchain);
    if (it == st.real_swapchain.end()) return;
    auto dit = st.real_dev.find(it->second.vdev);
    if (dit != st.real_dev.end()) {
        vkDeviceWaitIdle(dit->second.dev);
        for (auto& im : it->second.images) vk_real_free_swap_image(dit->second.dev, im);
    }
    st.real_swapchain.erase(it);
    st.swapchains.erase(vswapchain);
}

// Acquire the next image index from a swapchain (round-robin over its AHB ring).
// Returns true + sets `index_out` on success.
inline bool vk_real_acquire_next_image(VkDecodeState& st, uint32_t vswapchain,
                                       uint32_t& index_out) {
    auto it = st.real_swapchain.find(vswapchain);
    if (it == st.real_swapchain.end() || it->second.images.empty()) return false;
    index_out = it->second.next;
    it->second.next = (it->second.next + 1u) %
                      static_cast<uint32_t>(it->second.images.size());
    return true;
}

// Render a clear-bg + one triangle using the GUEST'S shader modules into the swapchain
// image `image_index`, submit + wait, route the image's AHB to the present sink, and
// read the center pixel back. This is vk_real_draw_submit promoted to (a) the guest's
// own SPIR-V (st.real_shader[rec.vvert/vfrag]) and (b) a persistent swapchain AHB (so
// the compositor can hold/import it). Returns an AlrVkRenderResult; fills px[4] +
// *presented_out (1 if the sink accepted the AHB). The render-pass/pipeline objects are
// per-call (cheap); only the AHB image ring persists in the swapchain.
inline int vk_real_draw_present(VkDecodeState& st, uint32_t vdev, uint32_t vqueue,
                                uint32_t vcmd, const VkClearRecord& rec, uint8_t px[4],
                                uint8_t* presented_out) {
    px[0] = px[1] = px[2] = px[3] = 0;
    if (presented_out) *presented_out = 0;
    if (!rec.recorded || !rec.is_present_draw) return ALR_VK_RENDER_NO_CLEAR_RECORDED;
    auto dit = st.real_dev.find(vdev);
    auto qit = st.real_queue.find(vqueue);
    auto cit = st.real_cmd.find(vcmd);
    if (dit == st.real_dev.end() || qit == st.real_queue.end() || cit == st.real_cmd.end())
        return ALR_VK_RENDER_NO_DEVICE;
    auto scit = st.real_swapchain.find(rec.vswapchain);
    if (scit == st.real_swapchain.end() ||
        rec.image_index >= scit->second.images.size())
        return ALR_VK_RENDER_NO_SWAPCHAIN;
    auto vsit = st.real_shader.find(rec.vvert);
    auto fsit = st.real_shader.find(rec.vfrag);
    if (vsit == st.real_shader.end() || fsit == st.real_shader.end())
        return ALR_VK_RENDER_NO_SHADER;

    VkDevice dev = dit->second.dev;
    VkPhysicalDevice phys = dit->second.phys;
    VkQueue queue = qit->second;
    VkCommandBuffer cmd = cit->second.second;
    VkDecodeState::RealSwapchain& sc = scit->second;
    VkDecodeState::SwapImage& target = sc.images[rec.image_index];
    const uint32_t w = sc.width;
    const uint32_t h = sc.height;
    const VkFormat color_fmt = sc.format;
    VkShaderModule vert_mod = vsit->second.second;
    VkShaderModule frag_mod = fsit->second.second;

    // Per-call render pass / framebuffer / pipeline / vertex buffer (the swapchain image
    // itself persists). Cleanup destroys only these per-call objects, NOT the AHB image.
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer vbuf = VK_NULL_HANDLE;
    VkDeviceMemory vmem = VK_NULL_HANDLE;
    auto cleanup = [&]() {
        if (vbuf) vkDestroyBuffer(dev, vbuf, nullptr);
        if (vmem) vkFreeMemory(dev, vmem, nullptr);
        if (pipeline) vkDestroyPipeline(dev, pipeline, nullptr);
        if (pipe_layout) vkDestroyPipelineLayout(dev, pipe_layout, nullptr);
        if (fb) vkDestroyFramebuffer(dev, fb, nullptr);
        if (rp) vkDestroyRenderPass(dev, rp, nullptr);
    };

    VkAttachmentDescription att{};
    att.format = color_fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;  // GENERAL: defined for the queue-family
                                                // release to the compositor + CPU read
    VkAttachmentReference att_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &att_ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = 0;
    dep.dstSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = 0;
    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    if (vkCreateRenderPass(dev, &rpci, nullptr, &rp) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_RECORD; }

    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = rp;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &target.view;
    fbci.width = w;
    fbci.height = h;
    fbci.layers = 1;
    if (vkCreateFramebuffer(dev, &fbci, nullptr, &fb) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_RECORD; }

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;  // empty layout
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &pipe_layout) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_PIPELINE; }

    // Vertex buffer (HOST_VISIBLE) with the 3 NDC triangle verts (bring-up contract:
    // the guest vert shader takes a vec2 at location 0; a later rung ships verts on wire).
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = sizeof(kAlrVkTriVerts);
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bci, nullptr, &vbuf) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_PIPELINE; }
    VkMemoryRequirements vreq{};
    vkGetBufferMemoryRequirements(dev, vbuf, &vreq);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t vtype = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        const bool usable = (vreq.memoryTypeBits & (1u << i)) != 0;
        const bool host_visible =
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (usable && host_visible) { vtype = i; break; }
    }
    if (vtype == UINT32_MAX) { cleanup(); return ALR_VK_RENDER_PIPELINE; }
    VkMemoryAllocateInfo vai{};
    vai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    vai.allocationSize = vreq.size;
    vai.memoryTypeIndex = vtype;
    if (vkAllocateMemory(dev, &vai, nullptr, &vmem) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_PIPELINE; }
    if (vkBindBufferMemory(dev, vbuf, vmem, 0) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_PIPELINE; }
    void* vmap = nullptr;
    if (vkMapMemory(dev, vmem, 0, sizeof(kAlrVkTriVerts), 0, &vmap) != VK_SUCCESS || !vmap) { cleanup(); return ALR_VK_RENDER_PIPELINE; }
    std::memcpy(vmap, kAlrVkTriVerts, sizeof(kAlrVkTriVerts));
    vkUnmapMemory(dev, vmem);

    // Graphics pipeline driven by the GUEST'S vert+frag modules.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vib{};
    vib.binding = 0;
    vib.stride = sizeof(AlrVkVert2);
    vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription via{};
    via.location = 0;
    via.binding = 0;
    via.format = VK_FORMAT_R32G32_SFLOAT;  // vec2 inPos at location 0
    via.offset = 0;
    VkPipelineVertexInputStateCreateInfo vis{};
    vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vis.vertexBindingDescriptionCount = 1;
    vis.pVertexBindingDescriptions = &vib;
    vis.vertexAttributeDescriptionCount = 1;
    vis.pVertexAttributeDescriptions = &via;

    VkPipelineInputAssemblyStateCreateInfo ias{};
    ias.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{};
    vp.width = static_cast<float>(w);
    vp.height = static_cast<float>(h);
    vp.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {w, h};
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vis;
    gp.pInputAssemblyState = &ias;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.layout = pipe_layout;
    gp.renderPass = rp;
    gp.subpass = 0;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline) != VK_SUCCESS) {
        cleanup();
        return ALR_VK_RENDER_PIPELINE;
    }

    // Record: clear bg, bind pipeline + verts, draw 3, release to the external consumer.
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_RECORD; }
    VkClearValue clear{};
    clear.color = {{rec.clear[0], rec.clear[1], rec.clear[2], rec.clear[3]}};
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = rp;
    rbi.framebuffer = fb;
    rbi.renderArea.extent = {w, h};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDeviceSize voff = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &voff);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    // Release the color image to the external consumer (the compositor's GPU import +
    // the CPU readback). Same barrier as the throwaway-AHB draw path.
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = dit->second.gfx_family;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier.image = target.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) { cleanup(); return ALR_VK_RENDER_RECORD; }

    // Submit + fence-wait.
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS) fence = VK_NULL_HANDLE;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) {
        if (fence) vkDestroyFence(dev, fence, nullptr);
        cleanup();
        return ALR_VK_RENDER_SUBMIT;
    }
    if (fence) {
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(dev, fence, nullptr);
    } else {
        vkQueueWaitIdle(queue);
    }

    // PRESENT: route the rendered AHB to the in-app compositor (zero-copy onto the
    // SurfaceView). The sink borrows the AHB and acquires its own ref if it retains it.
    if (vk_present_sink() != nullptr) {
        const uint64_t serial = st.present_serial++;
        const bool ok = vk_present_sink()(target.ahb, static_cast<int>(w),
                                          static_cast<int>(h), serial);
        if (presented_out) *presented_out = ok ? 1u : 0u;
    }

    // Read the presented image's center pixel back (the guest shader's color) so a
    // headless self-test (no display) still proves the guest SPIR-V ran on Mali.
    AHardwareBuffer_Desc got{};
    AHardwareBuffer_describe(target.ahb, &got);
    void* cpu = nullptr;
    if (AHardwareBuffer_lock(target.ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &cpu) == 0 &&
        cpu != nullptr) {
        const uint32_t stride_px = got.stride ? got.stride : w;
        const auto* base = static_cast<const unsigned char*>(cpu);
        const size_t off = (static_cast<size_t>(h / 2) * stride_px + (w / 2)) * 4;
        for (int i = 0; i < 4; ++i) px[i] = base[off + i];
        AHardwareBuffer_unlock(target.ahb, nullptr);
    } else {
        cleanup();
        return ALR_VK_RENDER_READBACK;
    }

    cleanup();
    return ALR_VK_RENDER_OK;
}

inline void vk_real_destroy_device(VkDecodeState& st, uint32_t vdev) {
    auto it = st.real_dev.find(vdev);
    if (it == st.real_dev.end()) return;
    VkDevice dev = it->second.dev;
    vkDeviceWaitIdle(dev);
    // command buffers are freed with their pool; destroy pools owned by this device.
    for (auto pit = st.real_pool.begin(); pit != st.real_pool.end();) {
        if (pit->second.first == vdev) {
            vkDestroyCommandPool(dev, pit->second.second, nullptr);
            pit = st.real_pool.erase(pit);
        } else {
            ++pit;
        }
    }
    for (auto cit = st.real_cmd.begin(); cit != st.real_cmd.end();) {
        if (cit->second.first == vdev) cit = st.real_cmd.erase(cit);
        else ++cit;
    }
    // VK-M4: free swapchains (their AHB images) + shader modules owned by this device
    // BEFORE vkDestroyDevice (their VkImage/VkShaderModule belong to `dev`).
    for (auto sit = st.real_swapchain.begin(); sit != st.real_swapchain.end();) {
        if (sit->second.vdev == vdev) {
            for (auto& im : sit->second.images) vk_real_free_swap_image(dev, im);
            st.swapchains.erase(sit->first);
            sit = st.real_swapchain.erase(sit);
        } else {
            ++sit;
        }
    }
    for (auto sh = st.real_shader.begin(); sh != st.real_shader.end();) {
        if (sh->second.first == vdev) {
            if (sh->second.second != VK_NULL_HANDLE)
                vkDestroyShaderModule(dev, sh->second.second, nullptr);
            st.shaders.erase(sh->first);
            sh = st.real_shader.erase(sh);
        } else {
            ++sh;
        }
    }
    vkDestroyDevice(dev, nullptr);
    st.real_dev.erase(it);
}
#endif  // ALR_VK_DECODE_REAL

// ---------------------------------------------------------------------------
// decode_vk_batch — drain one request batch; replay each op; APPEND its reply to
// `reply`. Returns false if the request stream was malformed. On the device build
// (ALR_VK_DECODE_REAL) it touches the real Mali driver; otherwise it resolves props
// from a host-supplied `provider` callback (so the wire test injects synthetic Mali
// props with no GPU). The reply stream is itself the VK reply wire (AlrVkReply ops),
// so the guest decodes it with the SAME VkReader.
//
// `provider`: when NOT building against real Vulkan, the caller supplies how to answer
// enumerate/props. It returns the device count for an instance and fills props per
// vphys. This is the seam that lets the marshalling path be wire-verified host-side
// (inject a synthetic "Mali-G615, VK 1.3, graphics queue" device) and Mali-verified
// device-side (the same code with ALR_VK_DECODE_REAL → vendor libvulkan).
// ---------------------------------------------------------------------------
struct VkProvider {
    // For an instance creation: return the VkResult-equivalent (0 == success).
    int (*create_instance)(void* ctx, uint32_t vinst, uint32_t app_api) = nullptr;
    // For enumerate: return device count; the host assigns vphys ids [vphys_base..).
    uint32_t (*enumerate)(void* ctx, uint32_t vinst, uint32_t vphys_base) = nullptr;
    // For props: fill `out` for vphys; return true if the id was known.
    bool (*props)(void* ctx, uint32_t vphys, VkPhysProps& out) = nullptr;
    // ANGLE-init rung: image-format query. Fill `out` (incl. out.vk_result = Mali's
    // verdict) for the (format,type,tiling,usage,flags) tuple on vphys; return true if the
    // id was known. Null = the host answers it directly via the real Mali path.
    bool (*image_format_props)(void* ctx, uint32_t vphys, uint32_t format, uint32_t type,
                               uint32_t tiling, uint32_t usage, uint32_t flags,
                               VkImageFmtProps& out) = nullptr;
    void (*destroy_instance)(void* ctx, uint32_t vinst) = nullptr;
    // ---- VK-M2 body seams (synthetic = wire mode; null callbacks are no-ops). ----
    // Create a logical device on vphys; return VkResult (0 == success), set gfx_family.
    int (*create_device)(void* ctx, uint32_t vphys, uint32_t vdev,
                         uint32_t* gfx_family_out) = nullptr;
    void (*get_queue)(void* ctx, uint32_t vdev, uint32_t queue_index,
                      uint32_t vqueue) = nullptr;
    // FULL DEVICE PASSTHROUGH seams (synthetic/wire mode; null = host real-Mali path).
    // create_device2 forwards the client's actual queue list / extensions / feature chain.
    int (*create_device2)(void* ctx, uint32_t vphys, uint32_t vdev,
                          const std::vector<std::pair<uint32_t, uint32_t>>& qcis,
                          const std::vector<std::string>& exts,
                          const std::vector<std::vector<uint8_t>>& feat_bytes,
                          const std::vector<uint32_t>& feat_types,
                          uint32_t* gfx_family_out) = nullptr;
    void (*get_queue2)(void* ctx, uint32_t vdev, uint32_t queue_family_index,
                       uint32_t queue_index, uint32_t vqueue) = nullptr;
    int (*create_pool)(void* ctx, uint32_t vdev, uint32_t vpool) = nullptr;
    int (*alloc_cmd)(void* ctx, uint32_t vdev, uint32_t vpool, uint32_t vcmd) = nullptr;
    // Replay the clear+submit; fill px[4] (RGBA 0..255); return AlrVkRenderResult.
    int (*clear_submit)(void* ctx, uint32_t vdev, uint32_t vqueue, uint32_t vcmd,
                        const VkClearRecord& rec, uint8_t px[4]) = nullptr;
    // VK-M3: replay the clear-then-DRAW-triangle + submit; fill px[4] (RGBA 0..255) with
    // the post-draw center pixel; return AlrVkRenderResult. Null = fall back to a bare
    // clear (so a provider that hasn't implemented draw still passes the clear path).
    int (*draw_submit)(void* ctx, uint32_t vdev, uint32_t vqueue, uint32_t vcmd,
                       const VkClearRecord& rec, uint8_t px[4]) = nullptr;
    void (*destroy_device)(void* ctx, uint32_t vdev) = nullptr;
    // ---- VK-M4 (PRESENT rung) seams (synthetic = wire mode; null callbacks no-op). ----
    // Create the guest's shader module from its SPIR-V; return VkResult (0 == success).
    int (*create_shader_module)(void* ctx, uint32_t vdev, uint32_t vshader,
                                const uint8_t* spirv, uint32_t spirv_len) = nullptr;
    void (*destroy_shader_module)(void* ctx, uint32_t vdev, uint32_t vshader) = nullptr;
    // Create an AHB-backed swapchain; return VkResult, set *image_count_out.
    int (*create_swapchain)(void* ctx, uint32_t vdev, uint32_t vswapchain, uint32_t width,
                            uint32_t height, uint32_t image_count,
                            uint32_t* image_count_out) = nullptr;
    void (*destroy_swapchain)(void* ctx, uint32_t vswapchain) = nullptr;
    // Acquire the next swapchain image; return true + set *index_out.
    bool (*acquire_next_image)(void* ctx, uint32_t vswapchain, uint32_t* index_out) = nullptr;
    // Replay the guest-shader clear+draw into the swapchain image + present; fill px[4]
    // (post-draw center pixel) + *presented (1 if routed to the sink); return
    // AlrVkRenderResult.
    int (*draw_present)(void* ctx, uint32_t vdev, uint32_t vqueue, uint32_t vcmd,
                        const VkClearRecord& rec, uint8_t px[4], uint8_t* presented) = nullptr;
    void* ctx = nullptr;
};

// ---------------------------------------------------------------------------
// GENERATED-OP SEAM. The 300.. opcode band is implemented by the codegen output
// (alr_gpu/generated/alr_gpu_vk_gen_decode.hpp::decode_vk_gen_op). To avoid a circular
// include (the generated decoder needs VkDecodeState/VkReader from THIS header), the
// generated header REGISTERS its dispatcher here via a function pointer. decode_vk_batch's
// default case calls it before fail-stopping, so an unknown op in the generated band is
// handled, and a truly-unknown op still fail-stops. `gen_provider` is an opaque pointer the
// caller may set (the wire test's synthetic Mali for the generated ops); on device it is
// null and the generated decoder uses the real-Mali path. Returns true if `op` was a
// generated op it handled (consuming its operands off `r`), false otherwise.
using VkGenDispatchFn = bool (*)(uint8_t op, VkReader& r, VkDecodeState& st,
                                 VkReplyEncoder& reply, const void* gen_provider);
inline VkGenDispatchFn& vk_gen_dispatch() {
    static VkGenDispatchFn fn = nullptr;
    return fn;
}
inline void set_vk_gen_dispatch(VkGenDispatchFn fn) { vk_gen_dispatch() = fn; }
// Optional opaque generated-provider pointer (wire test injects its synthetic Mali; device
// leaves it null). decode_vk_batch forwards it to the registered generated dispatcher.
inline const void*& vk_gen_provider_ptr() {
    static const void* p = nullptr;
    return p;
}
inline void set_vk_gen_provider(const void* p) { vk_gen_provider_ptr() = p; }

inline bool decode_vk_batch(const uint8_t* data, size_t len, VkDecodeState& st,
                            VkReplyEncoder& reply, const VkProvider* provider = nullptr) {
    VkReader r(data, len);
    bool running = true;
    while (running && st.ok) {
        uint8_t op = 0;
        if (!r.u8(op)) break;  // clean end of buffer
        switch (op) {
            case ALR_VK_OP_END:
                running = false;
                break;

            case ALR_VK_OP_CREATE_INSTANCE: {
                uint32_t vinst = 0, app_api = 0;
                if (!r.u32(vinst) || !r.u32(app_api)) { st.ok = false; break; }
                int res = -1;
#ifdef ALR_VK_DECODE_REAL
                if (!provider) {
                    res = static_cast<int>(vk_real_create_instance(st, vinst, app_api));
                }
#endif
                if (provider && provider->create_instance) {
                    res = provider->create_instance(provider->ctx, vinst, app_api);
                    if (res == 0) st.instances[vinst] = true;
                }
                reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_INSTANCE));
                reply.u32(vinst);
                reply.i32(res);
                st.decoded++;
                break;
            }

            case ALR_VK_OP_ENUMERATE_PHYSICAL_DEVICES: {
                uint32_t vinst = 0, vphys_base = 0;
                if (!r.u32(vinst) || !r.u32(vphys_base)) { st.ok = false; break; }
                uint32_t count = 0;
                int res = 0;
#ifdef ALR_VK_DECODE_REAL
                if (!provider) count = vk_real_enumerate(st, vinst, vphys_base);
#endif
                if (provider && provider->enumerate)
                    count = provider->enumerate(provider->ctx, vinst, vphys_base);
                st.enum_base[vinst] = vphys_base;
                st.enum_count[vinst] = count;
                reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_PHYS_COUNT));
                reply.u32(vinst);
                reply.u32(vphys_base);
                reply.u32(count);
                reply.i32(res);
                st.decoded++;
                break;
            }

            case ALR_VK_OP_GET_PHYSICAL_DEVICE_PROPERTIES: {
                uint32_t vinst = 0, vphys = 0;
                if (!r.u32(vinst) || !r.u32(vphys)) { st.ok = false; break; }
                VkPhysProps p{};
                bool got = false;
#ifdef ALR_VK_DECODE_REAL
                if (!provider) got = vk_real_props(st, vphys, p);
#endif
                if (provider && provider->props)
                    got = provider->props(provider->ctx, vphys, p);
                if (got) {
                    st.props[vphys] = p;
                    encode_phys_props_reply(reply, vphys, p);
                }
                // If the id was unknown the host emits no props record; the guest sees
                // the absence (a missing vphys in the reply) as "props unavailable".
                st.decoded++;
                break;
            }

            case ALR_VK_OP_GET_PHYS_IMAGE_FORMAT_PROPS: {
                uint32_t vinst = 0, vphys = 0, format = 0, type = 0, tiling = 0, usage = 0,
                         flags = 0;
                if (!r.u32(vinst) || !r.u32(vphys) || !r.u32(format) || !r.u32(type) ||
                    !r.u32(tiling) || !r.u32(usage) || !r.u32(flags)) {
                    st.ok = false;
                    break;
                }
                (void)vinst;
                VkImageFmtProps ifp{};
                // Default to "unknown vphys" => report FORMAT_NOT_SUPPORTED (a conformant,
                // non-crashing answer if the host can't resolve the device).
                ifp.vk_result = -11;  // VK_ERROR_FORMAT_NOT_SUPPORTED
                bool known = false;
#ifdef ALR_VK_DECODE_REAL
                if (!provider)
                    known = vk_real_image_format_props(st, vphys, format, type, tiling,
                                                       usage, flags, ifp);
#endif
                if (provider && provider->image_format_props)
                    known = provider->image_format_props(provider->ctx, vphys, format, type,
                                                         tiling, usage, flags, ifp);
                (void)known;
                reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_IMAGE_FORMAT_PROPS));
                reply.u32(vphys);
                reply.i32(ifp.vk_result);
                reply.u32(ifp.max_extent_w);
                reply.u32(ifp.max_extent_h);
                reply.u32(ifp.max_extent_d);
                reply.u32(ifp.max_mip_levels);
                reply.u32(ifp.max_array_layers);
                reply.u32(ifp.sample_counts);
                reply.u64(ifp.max_resource_size);
                st.decoded++;
                break;
            }

            case ALR_VK_OP_DESTROY_INSTANCE: {
                uint32_t vinst = 0;
                if (!r.u32(vinst)) { st.ok = false; break; }
#ifdef ALR_VK_DECODE_REAL
                if (!provider) vk_real_destroy_instance(st, vinst);
#endif
                if (provider && provider->destroy_instance)
                    provider->destroy_instance(provider->ctx, vinst);
                st.instances.erase(vinst);
                st.decoded++;
                break;
            }

            // ---- VK-M2 body ops ----
            case ALR_VK_OP_CREATE_DEVICE: {
                uint32_t vinst = 0, vphys = 0, vdev = 0;
                if (!r.u32(vinst) || !r.u32(vphys) || !r.u32(vdev)) { st.ok = false; break; }
                int res = -1;
                uint32_t gfx_family = 0;
#ifdef ALR_VK_DECODE_REAL
                if (!provider)
                    res = static_cast<int>(vk_real_create_device(st, vphys, vdev, gfx_family));
#endif
                if (provider && provider->create_device)
                    res = provider->create_device(provider->ctx, vphys, vdev, &gfx_family);
                if (res == 0) st.devices[vdev] = true;
                reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_DEVICE));
                reply.u32(vdev);
                reply.i32(res);
                reply.u32(gfx_family);
                st.decoded++;
                break;
            }

            case ALR_VK_OP_GET_DEVICE_QUEUE: {
                uint32_t vdev = 0, queue_index = 0, vqueue = 0;
                if (!r.u32(vdev) || !r.u32(queue_index) || !r.u32(vqueue)) { st.ok = false; break; }
#ifdef ALR_VK_DECODE_REAL
                if (!provider) vk_real_get_queue(st, vdev, queue_index, vqueue);
#endif
                if (provider && provider->get_queue)
                    provider->get_queue(provider->ctx, vdev, queue_index, vqueue);
                st.queues[vqueue] = true;  // client-side virtual id, no reply needed
                st.decoded++;
                break;
            }

            // ---- FULL DEVICE PASSTHROUGH ops ----
            case ALR_VK_OP_CREATE_DEVICE2: {
                uint32_t vinst = 0, vphys = 0, vdev = 0;
                if (!r.u32(vinst) || !r.u32(vphys) || !r.u32(vdev)) { st.ok = false; break; }
                // queue-create list
                uint32_t qci_count = 0;
                if (!r.u32(qci_count)) { st.ok = false; break; }
                if (qci_count > 64) { st.ok = false; break; }  // sane cap (matches kPrios)
                std::vector<std::pair<uint32_t, uint32_t>> qcis;
                qcis.reserve(qci_count);
                for (uint32_t i = 0; i < qci_count; ++i) {
                    uint32_t fam = 0, cnt = 0;
                    if (!r.u32(fam) || !r.u32(cnt)) { st.ok = false; break; }
                    qcis.emplace_back(fam, cnt);
                }
                if (!st.ok) break;
                // enabled device extensions
                uint32_t ext_count = 0;
                if (!r.u32(ext_count)) { st.ok = false; break; }
                if (ext_count > 256) { st.ok = false; break; }
                std::vector<std::string> exts;
                exts.reserve(ext_count);
                for (uint32_t i = 0; i < ext_count; ++i) {
                    const uint8_t* d = nullptr; uint32_t n = 0;
                    if (!r.blob(d, n)) { st.ok = false; break; }
                    if (n > 256) { st.ok = false; break; }  // VK_MAX_EXTENSION_NAME_SIZE bound
                    exts.emplace_back(reinterpret_cast<const char*>(d), n);
                }
                if (!st.ok) break;
                // allowlisted pNext feature structs
                uint32_t feat_count = 0;
                if (!r.u32(feat_count)) { st.ok = false; break; }
                if (feat_count > 64) { st.ok = false; break; }
                std::vector<uint32_t> feat_types;
                std::vector<std::vector<uint8_t>> feat_bytes;
                feat_types.reserve(feat_count);
                feat_bytes.reserve(feat_count);
                for (uint32_t i = 0; i < feat_count; ++i) {
                    uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                    if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; break; }
                    if (n > 1024) { st.ok = false; break; }  // a feature struct is small
                    feat_types.push_back(stype);
                    feat_bytes.emplace_back(d, d + n);
                }
                if (!st.ok) break;

                int res = -1;
                uint32_t gfx_family = 0;
#ifdef ALR_VK_DECODE_REAL
                if (!provider)
                    res = static_cast<int>(vk_real_create_device2(
                        st, vphys, vdev, qcis, exts, feat_bytes, feat_types, gfx_family));
#endif
                if (provider && provider->create_device2)
                    res = provider->create_device2(provider->ctx, vphys, vdev, qcis, exts,
                                                   feat_bytes, feat_types, &gfx_family);
                if (res == 0) st.devices[vdev] = true;
                reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_DEVICE));
                reply.u32(vdev);
                reply.i32(res);
                reply.u32(gfx_family);
                st.decoded++;
                break;
            }

            case ALR_VK_OP_GET_DEVICE_QUEUE2: {
                uint32_t vdev = 0, fam = 0, queue_index = 0, vqueue = 0;
                if (!r.u32(vdev) || !r.u32(fam) || !r.u32(queue_index) || !r.u32(vqueue)) {
                    st.ok = false; break;
                }
#ifdef ALR_VK_DECODE_REAL
                if (!provider) vk_real_get_queue2(st, vdev, fam, queue_index, vqueue);
#endif
                if (provider && provider->get_queue2)
                    provider->get_queue2(provider->ctx, vdev, fam, queue_index, vqueue);
                st.queues[vqueue] = true;  // client-side virtual id, no reply needed
                st.decoded++;
                break;
            }

            case ALR_VK_OP_CREATE_COMMAND_POOL: {
                uint32_t vdev = 0, vpool = 0;
                if (!r.u32(vdev) || !r.u32(vpool)) { st.ok = false; break; }
                int res = -1;
#ifdef ALR_VK_DECODE_REAL
                if (!provider) res = static_cast<int>(vk_real_create_pool(st, vdev, vpool));
#endif
                if (provider && provider->create_pool)
                    res = provider->create_pool(provider->ctx, vdev, vpool);
                if (res == 0) st.pools[vpool] = true;
                st.decoded++;
                break;
            }

            case ALR_VK_OP_ALLOCATE_COMMAND_BUFFERS: {
                uint32_t vdev = 0, vpool = 0, vcmd = 0;
                if (!r.u32(vdev) || !r.u32(vpool) || !r.u32(vcmd)) { st.ok = false; break; }
                int res = -1;
#ifdef ALR_VK_DECODE_REAL
                if (!provider) res = static_cast<int>(vk_real_alloc_cmd(st, vdev, vpool, vcmd));
#endif
                if (provider && provider->alloc_cmd)
                    res = provider->alloc_cmd(provider->ctx, vdev, vpool, vcmd);
                if (res == 0) st.cmds[vcmd] = true;
                st.decoded++;
                break;
            }

            case ALR_VK_OP_CMD_BEGIN_CLEAR: {
                uint32_t vdev = 0, vcmd = 0, w = 0, h = 0;
                float cr = 0, cg = 0, cb = 0, ca = 0;
                if (!r.u32(vdev) || !r.u32(vcmd) || !r.u32(w) || !r.u32(h) ||
                    !r.f32(cr) || !r.f32(cg) || !r.f32(cb) || !r.f32(ca)) {
                    st.ok = false;
                    break;
                }
                // The clear is RECORDED here (no GPU work yet); QUEUE_SUBMIT replays it.
                VkClearRecord rec;
                rec.recorded = true;
                rec.is_draw = false;
                rec.width = w;
                rec.height = h;
                rec.clear[0] = cr; rec.clear[1] = cg; rec.clear[2] = cb; rec.clear[3] = ca;
                st.clears[vcmd] = rec;
                st.decoded++;
                break;
            }

            case ALR_VK_OP_CMD_BEGIN_DRAW: {
                // VK-M3 breadth: same wire shape as CMD_BEGIN_CLEAR (the f32×4 is the
                // BACKGROUND); is_draw=true makes QUEUE_SUBMIT run the triangle pipeline.
                uint32_t vdev = 0, vcmd = 0, w = 0, h = 0;
                float br = 0, bg = 0, bb = 0, ba = 0;
                if (!r.u32(vdev) || !r.u32(vcmd) || !r.u32(w) || !r.u32(h) ||
                    !r.f32(br) || !r.f32(bg) || !r.f32(bb) || !r.f32(ba)) {
                    st.ok = false;
                    break;
                }
                VkClearRecord rec;
                rec.recorded = true;
                rec.is_draw = true;
                rec.width = w;
                rec.height = h;
                rec.clear[0] = br; rec.clear[1] = bg; rec.clear[2] = bb; rec.clear[3] = ba;
                st.clears[vcmd] = rec;
                st.decoded++;
                break;
            }

            // ---- VK-M4 (PRESENT rung) ops ----
            case ALR_VK_OP_CREATE_SHADER_MODULE: {
                uint32_t vdev = 0, vshader = 0, stage = 0;
                const uint8_t* spirv = nullptr; uint32_t spirv_len = 0;
                if (!r.u32(vdev) || !r.u32(vshader) || !r.u32(stage) ||
                    !r.blob(spirv, spirv_len)) { st.ok = false; break; }
                (void)stage;  // advisory; the pipeline stage binds the module
                int res = -1;
#ifdef ALR_VK_DECODE_REAL
                if (!provider)
                    res = static_cast<int>(
                        vk_real_create_shader_module(st, vdev, vshader, spirv, spirv_len));
#endif
                if (provider && provider->create_shader_module)
                    res = provider->create_shader_module(provider->ctx, vdev, vshader,
                                                         spirv, spirv_len);
                if (res == 0) st.shaders[vshader] = true;
                reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_SHADER));
                reply.u32(vshader);
                reply.i32(res);
                st.decoded++;
                break;
            }

            case ALR_VK_OP_DESTROY_SHADER_MODULE: {
                uint32_t vdev = 0, vshader = 0;
                if (!r.u32(vdev) || !r.u32(vshader)) { st.ok = false; break; }
#ifdef ALR_VK_DECODE_REAL
                if (!provider) vk_real_destroy_shader_module(st, vdev, vshader);
#endif
                if (provider && provider->destroy_shader_module)
                    provider->destroy_shader_module(provider->ctx, vdev, vshader);
                st.shaders.erase(vshader);
                st.decoded++;
                break;
            }

            case ALR_VK_OP_CREATE_SWAPCHAIN: {
                uint32_t vdev = 0, vsw = 0, w = 0, h = 0, cnt = 0;
                if (!r.u32(vdev) || !r.u32(vsw) || !r.u32(w) || !r.u32(h) || !r.u32(cnt)) {
                    st.ok = false; break;
                }
                int res = -1;
                uint32_t got_count = 0;
#ifdef ALR_VK_DECODE_REAL
                if (!provider)
                    res = static_cast<int>(
                        vk_real_create_swapchain(st, vdev, vsw, w, h, cnt, got_count));
#endif
                if (provider && provider->create_swapchain)
                    res = provider->create_swapchain(provider->ctx, vdev, vsw, w, h, cnt,
                                                     &got_count);
                if (res == 0) st.swapchains[vsw] = got_count;
                reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_SWAPCHAIN));
                reply.u32(vsw);
                reply.i32(res);
                reply.u32(got_count);
                st.decoded++;
                break;
            }

            case ALR_VK_OP_DESTROY_SWAPCHAIN: {
                uint32_t vdev = 0, vsw = 0;
                if (!r.u32(vdev) || !r.u32(vsw)) { st.ok = false; break; }
                (void)vdev;
#ifdef ALR_VK_DECODE_REAL
                if (!provider) vk_real_destroy_swapchain(st, vsw);
#endif
                if (provider && provider->destroy_swapchain)
                    provider->destroy_swapchain(provider->ctx, vsw);
                st.swapchains.erase(vsw);
                st.decoded++;
                break;
            }

            case ALR_VK_OP_ACQUIRE_NEXT_IMAGE: {
                uint32_t vdev = 0, vsw = 0;
                if (!r.u32(vdev) || !r.u32(vsw)) { st.ok = false; break; }
                (void)vdev;
                uint32_t index = 0;
                bool got = false;
#ifdef ALR_VK_DECODE_REAL
                if (!provider) got = vk_real_acquire_next_image(st, vsw, index);
#endif
                if (provider && provider->acquire_next_image)
                    got = provider->acquire_next_image(provider->ctx, vsw, &index);
                reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_ACQUIRE));
                reply.u32(vsw);
                reply.u32(index);
                reply.i32(got ? 0 : -1);
                st.decoded++;
                break;
            }

            case ALR_VK_OP_CMD_BEGIN_DRAW_MODULES: {
                uint32_t vdev = 0, vcmd = 0, vsw = 0, img = 0, vvert = 0, vfrag = 0,
                         w = 0, h = 0;
                float br = 0, bg = 0, bb = 0, ba = 0;
                if (!r.u32(vdev) || !r.u32(vcmd) || !r.u32(vsw) || !r.u32(img) ||
                    !r.u32(vvert) || !r.u32(vfrag) || !r.u32(w) || !r.u32(h) ||
                    !r.f32(br) || !r.f32(bg) || !r.f32(bb) || !r.f32(ba)) {
                    st.ok = false; break;
                }
                VkClearRecord rec;
                rec.recorded = true;
                rec.is_draw = true;
                rec.is_present_draw = true;
                rec.width = w;
                rec.height = h;
                rec.clear[0] = br; rec.clear[1] = bg; rec.clear[2] = bb; rec.clear[3] = ba;
                rec.vswapchain = vsw;
                rec.image_index = img;
                rec.vvert = vvert;
                rec.vfrag = vfrag;
                st.clears[vcmd] = rec;
                st.decoded++;
                break;
            }

            case ALR_VK_OP_QUEUE_PRESENT: {
                uint32_t vdev = 0, vqueue = 0, vcmd = 0, vsw = 0, img = 0;
                if (!r.u32(vdev) || !r.u32(vqueue) || !r.u32(vcmd) || !r.u32(vsw) ||
                    !r.u32(img)) { st.ok = false; break; }
                (void)vsw; (void)img;  // the recorded clear carries the swapchain/image
                int submit_res = 0;
                int render_res = ALR_VK_RENDER_NO_CLEAR_RECORDED;
                uint8_t px[4] = {0, 0, 0, 0};
                uint8_t presented = 0;
                auto cit = st.clears.find(vcmd);
                const VkClearRecord rec =
                    (cit != st.clears.end()) ? cit->second : VkClearRecord{};
#ifdef ALR_VK_DECODE_REAL
                if (!provider)
                    render_res = vk_real_draw_present(st, vdev, vqueue, vcmd, rec, px,
                                                      &presented);
#endif
                if (provider && provider->draw_present)
                    render_res = provider->draw_present(provider->ctx, vdev, vqueue, vcmd,
                                                        rec, px, &presented);
                if (render_res != ALR_VK_RENDER_OK &&
                    render_res != ALR_VK_RENDER_NO_CLEAR_RECORDED)
                    submit_res = -1;
                if (cit != st.clears.end()) st.clears.erase(cit);  // consume the record
                reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_PRESENT));
                reply.u32(vsw);
                reply.u32(img);
                reply.i32(submit_res);
                reply.i32(render_res);
                reply.u8(presented);
                reply.u8(px[0]);
                reply.u8(px[1]);
                reply.u8(px[2]);
                reply.u8(px[3]);
                st.decoded++;
                break;
            }

            case ALR_VK_OP_QUEUE_SUBMIT: {
                uint32_t vdev = 0, vqueue = 0, vcmd = 0;
                if (!r.u32(vdev) || !r.u32(vqueue) || !r.u32(vcmd)) { st.ok = false; break; }
                int submit_res = 0;
                int render_res = ALR_VK_RENDER_NO_CLEAR_RECORDED;
                uint8_t px[4] = {0, 0, 0, 0};
                auto cit = st.clears.find(vcmd);
                const VkClearRecord rec = (cit != st.clears.end()) ? cit->second : VkClearRecord{};
                // VK-M3: a record marked is_draw runs the triangle pipeline (clear bg +
                // draw); a bare clear runs the clear-only path. Both report through the
                // same ALR_VK_REPLY_SUBMIT (render_result + center px).
                const bool want_draw = rec.is_draw;
#ifdef ALR_VK_DECODE_REAL
                if (!provider)
                    render_res = want_draw ? vk_real_draw_submit(st, vdev, vqueue, vcmd, rec, px)
                                           : vk_real_clear_submit(st, vdev, vqueue, vcmd, rec, px);
#endif
                if (provider) {
                    if (want_draw && provider->draw_submit)
                        render_res = provider->draw_submit(provider->ctx, vdev, vqueue, vcmd, rec, px);
                    else if (provider->clear_submit)  // draw provider absent -> clear fallback
                        render_res = provider->clear_submit(provider->ctx, vdev, vqueue, vcmd, rec, px);
                }
                if (render_res != ALR_VK_RENDER_OK && render_res != ALR_VK_RENDER_NO_CLEAR_RECORDED)
                    submit_res = -1;  // a host stage failed; surface a non-success submit code
                if (cit != st.clears.end()) st.clears.erase(cit);  // consume the recorded clear
                reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_SUBMIT));
                reply.u32(vdev);
                reply.u32(vcmd);
                reply.i32(submit_res);
                reply.i32(render_res);
                reply.u8(px[0]);
                reply.u8(px[1]);
                reply.u8(px[2]);
                reply.u8(px[3]);
                st.decoded++;
                break;
            }

            case ALR_VK_OP_DESTROY_DEVICE: {
                uint32_t vdev = 0;
                if (!r.u32(vdev)) { st.ok = false; break; }
#ifdef ALR_VK_DECODE_REAL
                if (!provider) vk_real_destroy_device(st, vdev);
#endif
                if (provider && provider->destroy_device)
                    provider->destroy_device(provider->ctx, vdev);
                st.devices.erase(vdev);
                st.decoded++;
                break;
            }

            default: {
                // First give the generated 300.. band a chance (if its dispatcher is
                // registered). It reads the op's operands off the SAME reader and appends
                // its reply. If it handled the op, continue; otherwise fall through to the
                // fail-stop (same policy as the GLES decoder — never accept an off-contract
                // op). The hand-written 200..229 cases above always win for their numbers.
                VkGenDispatchFn gd = vk_gen_dispatch();
                if (gd && gd(op, r, st, reply, vk_gen_provider_ptr())) {
                    break;  // handled by the generated decoder (st.ok reflects its result)
                }
                st.ok = false;
                break;
            }
        }
    }
    reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_END));
    return st.ok;
}

// ---- Guest-side reply decode (also usable in the host self-test to verify the
// round-trip). Walks the reply op stream and fills out plain structs the caller
// can assert on. Returns false on malformed reply. ----
struct VkReplyInstance {
    uint32_t vinst = 0;
    int32_t result = 0;
};
struct VkReplyPhysCount {
    uint32_t vinst = 0;
    uint32_t vphys_base = 0;
    uint32_t count = 0;
    int32_t result = 0;
};
struct VkReplyDevice {
    uint32_t vdev = 0;
    int32_t result = 0;
    uint32_t gfx_family = 0;
};
struct VkReplySubmit {
    uint32_t vdev = 0;
    uint32_t vcmd = 0;
    int32_t submit_result = 0;
    int32_t render_result = 0;  // AlrVkRenderResult
    uint8_t px[4] = {0, 0, 0, 0};
};
// ---- VK-M4 (PRESENT rung) reply records ----
struct VkReplyShader {
    uint32_t vshader = 0;
    int32_t result = 0;
};
struct VkReplySwapchain {
    uint32_t vswapchain = 0;
    int32_t result = 0;
    uint32_t image_count = 0;
};
struct VkReplyAcquire {
    uint32_t vswapchain = 0;
    uint32_t image_index = 0;
    int32_t result = 0;
};
struct VkReplyPresent {
    uint32_t vswapchain = 0;
    uint32_t image_index = 0;
    int32_t submit_result = 0;
    int32_t render_result = 0;  // AlrVkRenderResult
    uint8_t presented = 0;      // 1 if routed to the compositor sink
    uint8_t px[4] = {0, 0, 0, 0};
};
// ---- ANGLE-init rung reply record ----
struct VkReplyImageFormatProps {
    uint32_t vphys = 0;
    int32_t result = 0;  // VkResult (0 == supported; <0 == FORMAT_NOT_SUPPORTED etc.)
    uint32_t max_extent_w = 0;
    uint32_t max_extent_h = 0;
    uint32_t max_extent_d = 0;
    uint32_t max_mip_levels = 0;
    uint32_t max_array_layers = 0;
    uint32_t sample_counts = 0;
    uint64_t max_resource_size = 0;
};
struct VkDecodedReply {
    std::vector<VkReplyInstance> instances;
    std::vector<VkReplyPhysCount> enumerations;
    std::map<uint32_t, VkPhysProps> props;  // vphys -> props
    std::vector<VkReplyDevice> devices;      // VK-M2 body
    std::vector<VkReplySubmit> submits;      // VK-M2 body
    std::vector<VkReplyShader> shaders;        // VK-M4
    std::vector<VkReplySwapchain> swapchains;  // VK-M4
    std::vector<VkReplyAcquire> acquires;      // VK-M4
    std::vector<VkReplyPresent> presents;      // VK-M4
    std::vector<VkReplyImageFormatProps> image_format_props;  // ANGLE-init rung
    bool ok = true;
};

inline bool decode_vk_reply(const uint8_t* data, size_t len, VkDecodedReply& out) {
    VkReader r(data, len);
    bool running = true;
    while (running) {
        uint8_t op = 0;
        if (!r.u8(op)) break;
        switch (op) {
            case ALR_VK_REPLY_END:
                running = false;
                break;
            case ALR_VK_REPLY_INSTANCE: {
                VkReplyInstance ri{};
                if (!r.u32(ri.vinst) || !r.i32(ri.result)) { out.ok = false; return false; }
                out.instances.push_back(ri);
                break;
            }
            case ALR_VK_REPLY_PHYS_COUNT: {
                VkReplyPhysCount pc{};
                if (!r.u32(pc.vinst) || !r.u32(pc.vphys_base) || !r.u32(pc.count) ||
                    !r.i32(pc.result)) {
                    out.ok = false;
                    return false;
                }
                out.enumerations.push_back(pc);
                break;
            }
            case ALR_VK_REPLY_PHYS_PROPS: {
                uint32_t vphys = 0;
                VkPhysProps p{};
                uint32_t qf_count = 0;
                const uint8_t* name = nullptr;
                uint32_t name_len = 0;
                uint8_t is_sw = 0;
                if (!r.u32(vphys) || !r.u32(p.api_version) || !r.u32(p.driver_version) ||
                    !r.u32(p.vendor_id) || !r.u32(p.device_id) || !r.u32(p.device_type) ||
                    !r.blob(name, name_len) || !r.u32(qf_count)) {
                    out.ok = false;
                    return false;
                }
                p.device_name.assign(reinterpret_cast<const char*>(name), name_len);
                for (uint32_t i = 0; i < qf_count; ++i) {
                    VkPhysProps::QF qf{};
                    if (!r.u32(qf.flags) || !r.u32(qf.count)) { out.ok = false; return false; }
                    p.queue_families.push_back(qf);
                }
                if (!r.u8(is_sw)) { out.ok = false; return false; }
                p.is_software = (is_sw != 0);
                out.props[vphys] = p;
                break;
            }
            case ALR_VK_REPLY_DEVICE: {
                VkReplyDevice rd{};
                if (!r.u32(rd.vdev) || !r.i32(rd.result) || !r.u32(rd.gfx_family)) {
                    out.ok = false;
                    return false;
                }
                out.devices.push_back(rd);
                break;
            }
            case ALR_VK_REPLY_SUBMIT: {
                VkReplySubmit rs{};
                if (!r.u32(rs.vdev) || !r.u32(rs.vcmd) || !r.i32(rs.submit_result) ||
                    !r.i32(rs.render_result) || !r.u8(rs.px[0]) || !r.u8(rs.px[1]) ||
                    !r.u8(rs.px[2]) || !r.u8(rs.px[3])) {
                    out.ok = false;
                    return false;
                }
                out.submits.push_back(rs);
                break;
            }
            case ALR_VK_REPLY_SHADER: {
                VkReplyShader sh{};
                if (!r.u32(sh.vshader) || !r.i32(sh.result)) { out.ok = false; return false; }
                out.shaders.push_back(sh);
                break;
            }
            case ALR_VK_REPLY_SWAPCHAIN: {
                VkReplySwapchain sw{};
                if (!r.u32(sw.vswapchain) || !r.i32(sw.result) || !r.u32(sw.image_count)) {
                    out.ok = false;
                    return false;
                }
                out.swapchains.push_back(sw);
                break;
            }
            case ALR_VK_REPLY_ACQUIRE: {
                VkReplyAcquire ac{};
                if (!r.u32(ac.vswapchain) || !r.u32(ac.image_index) || !r.i32(ac.result)) {
                    out.ok = false;
                    return false;
                }
                out.acquires.push_back(ac);
                break;
            }
            case ALR_VK_REPLY_PRESENT: {
                VkReplyPresent pr{};
                if (!r.u32(pr.vswapchain) || !r.u32(pr.image_index) ||
                    !r.i32(pr.submit_result) || !r.i32(pr.render_result) ||
                    !r.u8(pr.presented) || !r.u8(pr.px[0]) || !r.u8(pr.px[1]) ||
                    !r.u8(pr.px[2]) || !r.u8(pr.px[3])) {
                    out.ok = false;
                    return false;
                }
                out.presents.push_back(pr);
                break;
            }
            case ALR_VK_REPLY_IMAGE_FORMAT_PROPS: {
                VkReplyImageFormatProps ip{};
                if (!r.u32(ip.vphys) || !r.i32(ip.result) || !r.u32(ip.max_extent_w) ||
                    !r.u32(ip.max_extent_h) || !r.u32(ip.max_extent_d) ||
                    !r.u32(ip.max_mip_levels) || !r.u32(ip.max_array_layers) ||
                    !r.u32(ip.sample_counts) || !r.u64(ip.max_resource_size)) {
                    out.ok = false;
                    return false;
                }
                out.image_format_props.push_back(ip);
                break;
            }
            default:
                out.ok = false;
                return false;
        }
    }
    return out.ok;
}

}  // namespace alr::gpu

#endif  // ALR_GPU_ALR_GPU_VK_DECODE_HPP
