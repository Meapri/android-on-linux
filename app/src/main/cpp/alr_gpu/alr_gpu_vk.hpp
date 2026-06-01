// ALR GPU Vulkan executor keystone — VK-M1 (Phase 4 / Vulkan frontend track).
//
// The Vulkan twin of the GLES M4 work (alr_gpu_fbo.hpp / run_fbo_present_probe): a
// HOST-side probe that brings up the vendor Mali Vulkan driver in-process via the
// public NDK libvulkan, renders INTO an AHardwareBuffer-backed VkImage
// (COLOR_ATTACHMENT) on the real GPU, and reads the AHB back to pixel-verify. No
// guest, no ring — the de-risking keystone for the Vulkan headline (native-Vulkan
// games + all Wine/DXVK/VKD3D land on this backbone). See
// docs/research/gpu-guest-accel-strategy.md §7 (VK-M1) / §10.3 (converged order).
//
// WHY THIS IS THE RIGHT (AND ONLY) MALI PATH: on a non-root Mali device the vendor
// libvulkan reached in-process is the only hardware GPU path (strategy §2/§3); KMS/
// GBM/DRM/Panfrost/Turnip are all disqualified. VK_ANDROID_external_memory_android_
// hardware_buffer is CDD-mandatory on Vulkan-1.1+ (Mali-G615 is 1.3), and rendering
// INTO an AHB as an R8G8B8A8_UNORM COLOR_ATTACHMENT is spec-normative (no Ycbcr) —
// strategy §3.5. The AHB's dma-buf then drops into zwp_linux_dmabuf_v1 for the in-app
// compositor (WS-3), exactly mirroring the GLES AHB present (v114/v117).
//
// STATUS: written off-device (macOS has no Mali) — COMPILE-verified for aarch64 NDK,
// device-verification PENDING (DEVICE-REQ). Public NDK Vulkan + AHardwareBuffer only,
// W^X-safe, no vendor-private path. Header-only + self-contained (matching the rest of
// alr_gpu/**); a `nativeAlrGpuVk*` JNI entry is wired separately (kept out of this
// header so it doesn't collide with the concurrently-edited runtime_report.cpp).

#ifndef ALR_GPU_ALR_GPU_VK_HPP
#define ALR_GPU_ALR_GPU_VK_HPP

// The AHB Vulkan structs (VkImportAndroidHardwareBufferInfoANDROID, ...) are gated by
// VK_USE_PLATFORM_ANDROID_KHR. Enable it before <vulkan/vulkan.h> so this header is
// self-contained (the app build already defines it globally for the Android surface
// path; this guard makes a standalone include compile too).
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>
#include <android/hardware_buffer.h>
#include <cctype>  // tolower

#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace alr::gpu {

// ---- tiny self-contained helpers (this header does not depend on runtime_report.cpp) ----
inline const char* vk_res(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        default: return "VK_ERROR_<other>";
    }
}

inline bool vk_name_software(const char* name) {
    if (!name) return false;
    std::string n(name);
    for (auto& c : n) c = static_cast<char>(::tolower(c));
    return n.find("swiftshader") != std::string::npos || n.find("llvmpipe") != std::string::npos ||
           n.find("lavapipe") != std::string::npos || n.find("softpipe") != std::string::npos ||
           n.find("software") != std::string::npos;
}

inline bool vk_dev_ext_present(VkPhysicalDevice dev, const char* want) {
    uint32_t n = 0;
    if (vkEnumerateDeviceExtensionProperties(dev, nullptr, &n, nullptr) != VK_SUCCESS || n == 0)
        return false;
    std::vector<VkExtensionProperties> ext(n);
    if (vkEnumerateDeviceExtensionProperties(dev, nullptr, &n, ext.data()) != VK_SUCCESS) return false;
    for (const auto& e : ext)
        if (std::strcmp(e.extensionName, want) == 0) return true;
    return false;
}

// ===========================================================================
// run_vk_ahb_render_probe — VK-M1 keystone. Returns a report whose gating first line
// is "ALR VK AHB RENDER: PASS" or "ALR VK AHB RENDER: FAIL" (MainActivity greps it,
// like the GLES probes). PASS requires: a non-software Mali device, the AHB imported
// as a COLOR_ATTACHMENT VkImage, a render pass that CLEARs it green, and the AHB read
// back showing green at the center pixel.
// ===========================================================================
inline std::string run_vk_ahb_render_probe(int w = 64, int h = 64) {
    std::ostringstream out;
    auto fail = [&](const std::string& why) -> std::string {
        out << "ALR VK AHB RENDER: FAIL\nalr vk error=" << why;
        return out.str();
    };

    // ---- instance (Vulkan 1.1: AHB external-memory deps are core at 1.1+) ----
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "ALR VK-M1";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, nullptr, &inst);
    if (r != VK_SUCCESS) return fail(std::string("create-instance ") + vk_res(r));

    // ---- pick a hardware device with a graphics queue (reject software rasterizers) ----
    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(inst, &ndev, nullptr);
    if (ndev == 0) { vkDestroyInstance(inst, nullptr); return fail("no-physical-devices"); }
    std::vector<VkPhysicalDevice> devs(ndev);
    vkEnumeratePhysicalDevices(inst, &ndev, devs.data());

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    uint32_t gfx_qf = 0;
    bool software = true;
    const char* kAhbExt = "VK_ANDROID_external_memory_android_hardware_buffer";
    for (auto cand : devs) {
        if (!vk_dev_ext_present(cand, kAhbExt)) continue;  // need AHB import
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(cand, &p);
        uint32_t nqf = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(cand, &nqf, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nqf);
        vkGetPhysicalDeviceQueueFamilyProperties(cand, &nqf, qf.data());
        for (uint32_t i = 0; i < nqf; ++i) {
            if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                phys = cand; props = p; gfx_qf = i;
                software = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) ||
                           vk_name_software(p.deviceName);
                break;
            }
        }
        if (phys != VK_NULL_HANDLE) break;
    }
    if (phys == VK_NULL_HANDLE) { vkDestroyInstance(inst, nullptr); return fail("no-ahb-graphics-device"); }
    out << "alr vk renderer=" << props.deviceName;
    out << "\nalr vk software renderer=" << (software ? "true" : "false");

    // ---- logical device: enable AHB external-memory import (+ its non-core dep) ----
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = gfx_qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    std::vector<const char*> dev_ext = {kAhbExt};
    if (vk_dev_ext_present(phys, "VK_EXT_queue_family_foreign"))
        dev_ext.push_back("VK_EXT_queue_family_foreign");
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(dev_ext.size());
    dci.ppEnabledExtensionNames = dev_ext.data();
    VkDevice dev = VK_NULL_HANDLE;
    r = vkCreateDevice(phys, &dci, nullptr, &dev);
    if (r != VK_SUCCESS) { vkDestroyInstance(inst, nullptr); return fail(std::string("create-device ") + vk_res(r)); }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, gfx_qf, 0, &queue);

    auto p_ahb_props = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
        vkGetDeviceProcAddr(dev, "vkGetAndroidHardwareBufferPropertiesANDROID"));
    if (!p_ahb_props) {
        vkDestroyDevice(dev, nullptr); vkDestroyInstance(inst, nullptr);
        return fail("no-vkGetAndroidHardwareBufferPropertiesANDROID");
    }

    // ---- allocate an AHB usable as a GPU color target + CPU-readable for verify ----
    AHardwareBuffer_Desc d{};
    d.width = static_cast<uint32_t>(w);
    d.height = static_cast<uint32_t>(h);
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    AHardwareBuffer* ahb = nullptr;
    if (AHardwareBuffer_allocate(&d, &ahb) != 0 || ahb == nullptr) {
        vkDestroyDevice(dev, nullptr); vkDestroyInstance(inst, nullptr);
        return fail("ahb-allocate");
    }

    // RAII-ish cleanup for the device-side objects we create below.
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    auto cleanup_all = [&]() {
        if (pool) vkDestroyCommandPool(dev, pool, nullptr);
        if (fb) vkDestroyFramebuffer(dev, fb, nullptr);
        if (rp) vkDestroyRenderPass(dev, rp, nullptr);
        if (view) vkDestroyImageView(dev, view, nullptr);
        if (image) vkDestroyImage(dev, image, nullptr);
        if (mem) vkFreeMemory(dev, mem, nullptr);
        AHardwareBuffer_release(ahb);
        vkDestroyDevice(dev, nullptr);
        vkDestroyInstance(inst, nullptr);
    };

    // ---- AHB properties (memory type bits + the Vulkan format it maps to) ----
    VkAndroidHardwareBufferFormatPropertiesANDROID fmt_props{};
    fmt_props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferPropertiesANDROID ahb_props{};
    ahb_props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    ahb_props.pNext = &fmt_props;
    r = p_ahb_props(dev, ahb, &ahb_props);
    if (r != VK_SUCCESS) { cleanup_all(); return fail(std::string("ahb-properties ") + vk_res(r)); }
    // R8G8B8A8 AHB maps to a concrete VkFormat (not external) — strategy §3.5.
    VkFormat color_fmt = (fmt_props.format != VK_FORMAT_UNDEFINED) ? fmt_props.format
                                                                   : VK_FORMAT_R8G8B8A8_UNORM;

    // ---- VkImage backed by the AHB (COLOR_ATTACHMENT) ----
    VkExternalMemoryImageCreateInfo ext_img{};
    ext_img.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo img_ci{};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.pNext = &ext_img;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = color_fmt;
    img_ci.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    img_ci.mipLevels = 1;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = vkCreateImage(dev, &img_ci, nullptr, &image);
    if (r != VK_SUCCESS) { cleanup_all(); return fail(std::string("create-image ") + vk_res(r)); }

    // ---- import the AHB as the image's backing memory (dedicated) ----
    uint32_t mem_type = 0;
    bool found_type = false;
    for (uint32_t i = 0; i < 32; ++i) {
        if (ahb_props.memoryTypeBits & (1u << i)) { mem_type = i; found_type = true; break; }
    }
    if (!found_type) { cleanup_all(); return fail("no-importable-memory-type"); }
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
    r = vkAllocateMemory(dev, &mai, nullptr, &mem);
    if (r != VK_SUCCESS) { cleanup_all(); return fail(std::string("import-memory ") + vk_res(r)); }
    r = vkBindImageMemory(dev, image, mem, 0);
    if (r != VK_SUCCESS) { cleanup_all(); return fail(std::string("bind-image-memory ") + vk_res(r)); }

    // ---- image view + render pass (CLEAR -> STORE) + framebuffer ----
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = color_fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    r = vkCreateImageView(dev, &vci, nullptr, &view);
    if (r != VK_SUCCESS) { cleanup_all(); return fail(std::string("create-view ") + vk_res(r)); }

    VkAttachmentDescription att{};
    att.format = color_fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference att_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &att_ref;
    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    r = vkCreateRenderPass(dev, &rpci, nullptr, &rp);
    if (r != VK_SUCCESS) { cleanup_all(); return fail(std::string("create-renderpass ") + vk_res(r)); }

    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = rp;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &view;
    fbci.width = static_cast<uint32_t>(w);
    fbci.height = static_cast<uint32_t>(h);
    fbci.layers = 1;
    r = vkCreateFramebuffer(dev, &fbci, nullptr, &fb);
    if (r != VK_SUCCESS) { cleanup_all(); return fail(std::string("create-framebuffer ") + vk_res(r)); }

    // ---- record + submit: begin render pass (clear green), end ----
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = gfx_qf;
    r = vkCreateCommandPool(dev, &pci, nullptr, &pool);
    if (r != VK_SUCCESS) { cleanup_all(); return fail(std::string("create-cmdpool ") + vk_res(r)); }
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    r = vkAllocateCommandBuffers(dev, &cbai, &cmd);
    if (r != VK_SUCCESS) { cleanup_all(); return fail(std::string("alloc-cmdbuf ") + vk_res(r)); }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkClearValue clear{};
    clear.color = {{0.0f, 1.0f, 0.0f, 1.0f}};  // green
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = rp;
    rbi.framebuffer = fb;
    rbi.renderArea.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(cmd);  // CLEAR is the whole render (the keystone proves AHB-as-target)
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) { cleanup_all(); return fail(std::string("queue-submit ") + vk_res(r)); }
    vkQueueWaitIdle(queue);

    // ---- read the AHB back on the CPU and verify the GPU clear landed (green) ----
    AHardwareBuffer_Desc got{};
    AHardwareBuffer_describe(ahb, &got);  // got.stride is in PIXELS for R8G8B8A8
    void* cpu = nullptr;
    bool center_green = false;
    unsigned char px[4] = {0, 0, 0, 0};
    if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &cpu) == 0 &&
        cpu != nullptr) {
        const uint32_t stride_px = got.stride ? got.stride : static_cast<uint32_t>(w);
        const auto* base = static_cast<const unsigned char*>(cpu);
        const size_t off = (static_cast<size_t>(h / 2) * stride_px + (w / 2)) * 4;
        for (int i = 0; i < 4; ++i) px[i] = base[off + i];
        center_green = px[1] > 200 && px[0] < 80 && px[2] < 80;
        AHardwareBuffer_unlock(ahb, nullptr);
    } else {
        cleanup_all();
        return fail("ahb-lock-readback");
    }

    const bool pass = !software && center_green;
    out << "\nalr vk format=" << static_cast<int>(color_fmt) << " (R8G8B8A8_UNORM=" << VK_FORMAT_R8G8B8A8_UNORM << ")";
    out << "\nalr vk ahb stride_px=" << got.stride;
    out << "\nalr vk center px=" << static_cast<int>(px[0]) << "," << static_cast<int>(px[1]) << ","
        << static_cast<int>(px[2]) << "," << static_cast<int>(px[3]) << " (expect ~0,255,0,255 green clear)";
    out << "\nalr vk path=render-into-AHB (COLOR_ATTACHMENT, VK_ANDROID_external_memory_AHB), zero-copy presentable";
    cleanup_all();

    // Gating first line LAST so the detail lines above are already buffered.
    std::ostringstream final_out;
    final_out << "ALR VK AHB RENDER: " << (pass ? "PASS" : "FAIL") << out.str();
    return final_out.str();
}

}  // namespace alr::gpu

#endif  // ALR_GPU_ALR_GPU_VK_HPP
