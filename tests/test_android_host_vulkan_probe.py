from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
CMAKE = ROOT / "app/src/main/cpp/CMakeLists.txt"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
GRAPHICS_SPEC = ROOT / "docs/android-graphics-bridge-spec.md"
GPU_ARCH = ROOT / "docs/architecture/gpu-display-bridge.md"


def test_native_loader_links_android_vulkan_and_reports_capabilities():
    assert "vulkan" in CMAKE.read_text(encoding="utf-8")
    text = CPP.read_text(encoding="utf-8")
    assert "#define VK_USE_PLATFORM_ANDROID_KHR" in text
    assert "#include <vulkan/vulkan.h>" in text
    assert "vkCreateInstance" in text
    assert "vkEnumeratePhysicalDevices" in text
    assert "VK_KHR_ANDROID_SURFACE_EXTENSION_NAME" in text
    assert "host vulkan hardware candidate=" in text
    assert "host vulkan software renderer=" in text
    assert "vkCreateAndroidSurfaceKHR" in text
    assert "vkGetPhysicalDeviceSurfaceSupportKHR" in text
    assert "vkGetPhysicalDeviceSurfaceFormatsKHR" in text
    assert "host vulkan surface hardware candidate=" in text
    assert "vkCreateSwapchainKHR" in text
    assert "vkCmdBeginRenderPass" in text
    assert "vkQueuePresentKHR" in text
    assert "host vulkan surface renderer=android-vulkan-swapchain-clear-present" in text
    assert "guest vulkan clear present hardware render=" in text


def test_main_activity_reports_vulkan_probe_before_verbose_report():
    text = MAIN.read_text(encoding="utf-8")
    assert "nativeHostVulkanProbe" in text
    assert "nativeProbeVulkanSurface" in text
    assert "nativeRenderVulkanSurfaceFrames" in text
    assert "ANDROID HOST VULKAN PROBE EXECUTION:" in text
    assert "ANDROID HOST VULKAN SURFACE PROBE EXECUTION: PENDING_SURFACE_CALLBACK" in text
    assert "ANDROID HOST VULKAN SURFACE EXECUTION: PENDING_SURFACE_CALLBACK" in text
    assert "host vulkan device=" in text
    assert "host vulkan android surface extension=" in text
    assert "Android host Vulkan probe:" in text
    assert "Android host Vulkan surface probe" in text
    assert "Android host Vulkan surface renderer" in text
    assert text.index("ANDROID HOST VULKAN PROBE EXECUTION:") < text.index("--- verbose report ---")


def test_docs_distinguish_vulkan_probe_from_surface_clear_claim():
    graphics = GRAPHICS_SPEC.read_text(encoding="utf-8")
    gpu_arch = GPU_ARCH.read_text(encoding="utf-8")
    assert "Host Vulkan capability probe" in graphics
    assert "ANDROID HOST VULKAN PROBE EXECUTION: PASS" in graphics
    assert "ANDROID HOST VULKAN SURFACE PROBE EXECUTION: PASS" in graphics
    assert "ANDROID HOST VULKAN SURFACE EXECUTION: PASS" in graphics
    assert "Vulkan swapchain clear/present renderer" in graphics
    assert "Vulkan host probe creates an instance" in gpu_arch
    assert "checks Android surface extension support" in gpu_arch
    assert "Vulkan surface probe checks queue-family present support" in gpu_arch
    assert "Vulkan surface renderer creates an Android swapchain" in gpu_arch
