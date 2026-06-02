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
#endif

namespace alr::gpu {

// ---- A bounds-checked little-endian cursor (same shape as alr::gpu::Reader in
// alr_gpu_decode.hpp; duplicated here to keep this header standalone — no GLES
// include). Reads the request stream AND, on the guest, the reply stream. ----
class VkReader {
public:
    VkReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool u8(uint8_t& v) { return take(&v, 1); }
    bool u32(uint32_t& v) { return take(&v, 4); }
    bool i32(int32_t& v) { return take(&v, 4); }
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
    void u32(uint32_t v) { raw(&v, 4); }
    void i32(int32_t v) { raw(&v, 4); }
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

// Host decode state: virtual instance/physical-device ids -> the props the host
// resolved for them. (The real VkInstance/VkPhysicalDevice live behind these only on
// the device build; the wire test populates props_ directly.) Mirrors HostState.
struct VkDecodeState {
    std::map<uint32_t, bool> instances;            // vinst -> created
    std::map<uint32_t, uint32_t> enum_base;        // vinst -> vphys_base assigned
    std::map<uint32_t, uint32_t> enum_count;       // vinst -> device count
    std::map<uint32_t, VkPhysProps> props;         // vphys -> resolved props
    bool ok = true;
    int decoded = 0;  // request ops dispatched

#ifdef ALR_VK_DECODE_REAL
    std::map<uint32_t, VkInstance> real_inst;      // vinst -> real VkInstance
    std::map<uint32_t, VkPhysicalDevice> real_phys;// vphys -> real VkPhysicalDevice
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
    void (*destroy_instance)(void* ctx, uint32_t vinst) = nullptr;
    void* ctx = nullptr;
};

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

            default:
                // Unknown opcode: fail-stop (same policy as the GLES decoder) — never
                // emit an op not in alr_gpu_vk_proto.hpp.
                st.ok = false;
                break;
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
struct VkDecodedReply {
    std::vector<VkReplyInstance> instances;
    std::vector<VkReplyPhysCount> enumerations;
    std::map<uint32_t, VkPhysProps> props;  // vphys -> props
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
            default:
                out.ok = false;
                return false;
        }
    }
    return out.ok;
}

}  // namespace alr::gpu

#endif  // ALR_GPU_ALR_GPU_VK_DECODE_HPP
