/* alr-vk-tri.c — the GUEST Vulkan triangle app for the ALR ICD PRESENT (VK-M4) test.
 *
 * Run UNDER the ALR loader, with the ALR Vulkan ICD (libvulkan.so.1) first on the guest
 * library search path (/usr/lib/androlinux) and the VK ring env set (ALR_VK_ICD=1), this:
 *   1. vkCreateInstance + vkEnumeratePhysicalDevices + vkCreateDevice + vkGetDeviceQueue,
 *   2. vkCreateCommandPool + vkAllocateCommandBuffers,
 *   3. vkCreateShaderModule × 2 — uploading ITS OWN SPIR-V (alr_vk_tri_spirv.h) over the
 *      wire to the host, which vkCreateShaderModule's it on REAL Mali,
 *   4. vkCreateSwapchainKHR (AHB-backed) + vkGetSwapchainImagesKHR,
 *   5. vkAcquireNextImageKHR -> records a clear+triangle DRAW with ITS OWN shaders
 *      (alrVkCmdDrawTriangleModules) -> vkQueuePresentKHR,
 *   6. repeats a few frames, then tears down.
 *
 * The ONLY Vulkan implementation it binds is our libvulkan.so.1, which marshals to real
 * Mali over the ring AND routes the presented swapchain AHB to the in-app Wayland
 * compositor (a wl_surface on the SurfaceView). So a window with the guest's triangle
 * appears on screen — the VK-M4 milestone. The expected PASS line is:
 *
 *     ALR VK ICD TRIANGLE: PASS device=Mali-G615 MC2 frames=4 present=VK_SUCCESS
 *
 * (Visual proof = the triangle on the SurfaceView; this stdout line is the headless/
 * logcat-greppable signal.) Self-contained (vendored ABI header, no Vulkan SDK) so it
 * cross-builds with zig cc — exactly like alr-vk-enum.c.
 */
#include "alr_icd_vk_min.h"
#include "alr_vk_tri_spirv.h"  /* the GUEST's own triangle SPIR-V (vert + frag) */

#include <stdio.h>
#include <string.h>

#define TRI_W 512
#define TRI_H 512
#define TRI_FRAMES 4

int main(void) {
    VkApplicationInfo app;
    VkInstanceCreateInfo ici;
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys[8];
    VkPhysicalDeviceProperties props;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkResult r;
    uint32_t count = 0;
    int present_result = 0;
    int frames_presented = 0;

    /* ---- instance + physical device ---- */
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "alr-vk-tri";
    app.apiVersion = VK_API_VERSION_1_1;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS || inst == VK_NULL_HANDLE) {
        printf("ALR VK ICD TRIANGLE: FAIL stage=create-instance result=%d\n", (int)r);
        return 1;
    }
    r = vkEnumeratePhysicalDevices(inst, &count, NULL);
    if (r != VK_SUCCESS || count == 0) {
        printf("ALR VK ICD TRIANGLE: FAIL stage=enumerate count=%u result=%d "
               "(no host ring? expected off-device)\n", count, (int)r);
        vkDestroyInstance(inst, NULL);
        return 1;
    }
    if (count > 8) count = 8;
    vkEnumeratePhysicalDevices(inst, &count, phys);
    memset(&props, 0, sizeof(props));
    vkGetPhysicalDeviceProperties(phys[0], &props);
    printf("alr vk icd triangle device0 name=\"%s\"\n", props.deviceName);

    /* ---- logical device + graphics queue ---- */
    {
        VkDeviceQueueCreateInfo qci;
        VkDeviceCreateInfo dci;
        float prio = 1.0f;
        memset(&qci, 0, sizeof(qci));
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = 0;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        memset(&dci, 0, sizeof(dci));
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        r = vkCreateDevice(phys[0], &dci, NULL, &dev);
        if (r != VK_SUCCESS || dev == VK_NULL_HANDLE) {
            printf("ALR VK ICD TRIANGLE: FAIL stage=create-device result=%d\n", (int)r);
            vkDestroyInstance(inst, NULL);
            return 1;
        }
        vkGetDeviceQueue(dev, 0, 0, &queue);
        if (queue == VK_NULL_HANDLE) {
            printf("ALR VK ICD TRIANGLE: FAIL stage=get-queue\n");
            vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
            return 1;
        }
    }

    /* ---- command pool + buffer ---- */
    VkCommandPool pool = (VkCommandPool)0;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo pci;
        VkCommandBufferAllocateInfo cbai;
        memset(&pci, 0, sizeof(pci));
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = 0;
        r = vkCreateCommandPool(dev, &pci, NULL, &pool);
        if (r != VK_SUCCESS) {
            printf("ALR VK ICD TRIANGLE: FAIL stage=create-pool result=%d\n", (int)r);
            vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
            return 1;
        }
        memset(&cbai, 0, sizeof(cbai));
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        r = vkAllocateCommandBuffers(dev, &cbai, &cmd);
        if (r != VK_SUCCESS || cmd == VK_NULL_HANDLE) {
            printf("ALR VK ICD TRIANGLE: FAIL stage=alloc-cmd result=%d\n", (int)r);
            vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
            return 1;
        }
    }

    /* ---- GUEST SPIR-V over the wire: create the app's OWN vert+frag modules ---- */
    VkShaderModule vert = (VkShaderModule)0, frag = (VkShaderModule)0;
    {
        VkShaderModuleCreateInfo vsm, fsm;
        memset(&vsm, 0, sizeof(vsm));
        vsm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vsm.codeSize = sizeof(kAlrTriVertSpv);
        vsm.pCode = kAlrTriVertSpv;
        r = vkCreateShaderModule(dev, &vsm, NULL, &vert);
        if (r != VK_SUCCESS) {
            printf("ALR VK ICD TRIANGLE: FAIL stage=create-vert-shader result=%d\n", (int)r);
            vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
            return 1;
        }
        memset(&fsm, 0, sizeof(fsm));
        fsm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        fsm.codeSize = sizeof(kAlrTriFragSpv);
        fsm.pCode = kAlrTriFragSpv;
        r = vkCreateShaderModule(dev, &fsm, NULL, &frag);
        if (r != VK_SUCCESS) {
            printf("ALR VK ICD TRIANGLE: FAIL stage=create-frag-shader result=%d\n", (int)r);
            vkDestroyShaderModule(dev, vert, NULL);
            vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
            return 1;
        }
        printf("alr vk icd triangle guest SPIR-V uploaded: vert=%u bytes frag=%u bytes\n",
               (unsigned)sizeof(kAlrTriVertSpv), (unsigned)sizeof(kAlrTriFragSpv));
    }

    /* ---- AHB-backed swapchain ---- */
    VkSwapchainKHR swap = (VkSwapchainKHR)0;
    uint32_t img_count = 0;
    {
        VkSwapchainCreateInfoKHR sci;
        memset(&sci, 0, sizeof(sci));
        sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        sci.minImageCount = 2;
        sci.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
        sci.imageExtent.width = TRI_W;
        sci.imageExtent.height = TRI_H;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = 1;
        r = vkCreateSwapchainKHR(dev, &sci, NULL, &swap);
        if (r != VK_SUCCESS) {
            printf("ALR VK ICD TRIANGLE: FAIL stage=create-swapchain result=%d\n", (int)r);
            vkDestroyShaderModule(dev, frag, NULL); vkDestroyShaderModule(dev, vert, NULL);
            vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
            return 1;
        }
        vkGetSwapchainImagesKHR(dev, swap, &img_count, NULL);
        printf("alr vk icd triangle swapchain images=%u extent=%dx%d\n", img_count, TRI_W, TRI_H);
    }

    /* ---- present loop: acquire -> record guest-shader draw -> present ---- */
    for (int f = 0; f < TRI_FRAMES; ++f) {
        uint32_t image_index = 0;
        r = vkAcquireNextImageKHR(dev, swap, ~0ULL, (VkSemaphore)0, (VkFence)0, &image_index);
        if (r != VK_SUCCESS) {
            printf("alr vk icd triangle acquire frame=%d result=%d\n", f, (int)r);
            present_result = (int)r;
            break;
        }
        /* Record the clear + triangle DRAW using the guest's OWN shader modules into the
         * acquired swapchain image. (Coarse record — the renderpass/pipeline/draw are
         * built host-side on real Mali from these modules; see alr_icd_vk_min.h.) */
        alrVkCmdDrawTriangleModules(cmd, swap, image_index, vert, frag, TRI_W, TRI_H,
                                    /*bg=*/0.05f, 0.05f, 0.08f, 1.0f);
        VkPresentInfoKHR pi;
        VkSwapchainKHR swaps[1]; uint32_t idxs[1]; VkResult results[1];
        swaps[0] = swap; idxs[0] = image_index; results[0] = VK_SUCCESS;
        memset(&pi, 0, sizeof(pi));
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.swapchainCount = 1;
        pi.pSwapchains = swaps;
        pi.pImageIndices = idxs;
        pi.pResults = results;
        r = vkQueuePresentKHR(queue, &pi);
        present_result = (int)r;
        if (r != VK_SUCCESS) {
            printf("alr vk icd triangle present frame=%d result=%d\n", f, (int)r);
            break;
        }
        ++frames_presented;
    }

    /* ---- teardown ---- */
    vkDestroySwapchainKHR(dev, swap, NULL);
    vkDestroyShaderModule(dev, frag, NULL);
    vkDestroyShaderModule(dev, vert, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);

    /* PASS: a Mali device, the guest's own SPIR-V uploaded, and >=1 frame presented OK. */
    int pass = (strstr(props.deviceName, "Mali") != NULL) && (frames_presented >= 1) &&
               (present_result == (int)VK_SUCCESS);
    printf("ALR VK ICD TRIANGLE: %s device=%s frames=%d present=%s\n",
           pass ? "PASS" : "FAIL", props.deviceName, frames_presented,
           present_result == (int)VK_SUCCESS ? "VK_SUCCESS" : "fail");
    return pass ? 0 : 1;
}
