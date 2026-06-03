/* alr-vk-enum.c — the minimal guest Vulkan client for the ALR ICD device test.
 *
 * Run UNDER the ALR loader, with the ALR Vulkan ICD (libvulkan.so.1) first on the
 * guest library search path (/usr/lib/androlinux) and the VK ring env set, this:
 *   1. vkCreateInstance,
 *   2. vkEnumeratePhysicalDevices (count, then fill),
 *   3. vkGetPhysicalDeviceProperties on device 0 and PRINTS deviceName + apiVersion,
 *   4. (bonus) vkGetPhysicalDeviceQueueFamilyProperties, vkCreateDevice,
 *      vkGetDeviceQueue, then tears down.
 *
 * It calls the PUBLIC Vulkan symbols directly (linked -lvulkan), so this is the
 * "reached through the standard Vulkan ICD entry instead of in-process probe code"
 * proof: the ONLY Vulkan implementation it binds is our libvulkan.so.1, which marshals
 * to real Mali over the ring. The expected PASS line is:
 *
 *     ALR VK ICD CLIENT: PASS device=Mali-G615 MC2 api=1.3
 *
 * Self-contained (vendored ABI header, no Vulkan SDK) so it cross-builds with zig cc.
 */
#include "alr_icd_vk_min.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    VkApplicationInfo app;
    VkInstanceCreateInfo ici;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r;
    uint32_t count = 0;
    VkPhysicalDevice phys[8];
    VkPhysicalDeviceProperties props;
    int pass = 0;

    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "alr-vk-enum";
    app.apiVersion = VK_API_VERSION_1_1;

    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS || inst == VK_NULL_HANDLE) {
        printf("ALR VK ICD CLIENT: FAIL stage=create-instance result=%d\n", (int)r);
        return 1;
    }

    r = vkEnumeratePhysicalDevices(inst, &count, NULL);
    printf("alr vk icd client enumerate count=%u result=%d\n", count, (int)r);
    if (r != VK_SUCCESS || count == 0) {
        printf("ALR VK ICD CLIENT: FAIL stage=enumerate count=%u result=%d "
               "(no host ring? expected on a smoke/off-device run)\n", count, (int)r);
        vkDestroyInstance(inst, NULL);
        return 1;
    }
    if (count > 8) count = 8;
    r = vkEnumeratePhysicalDevices(inst, &count, phys);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
        printf("ALR VK ICD CLIENT: FAIL stage=enumerate-fill result=%d\n", (int)r);
        vkDestroyInstance(inst, NULL);
        return 1;
    }

    memset(&props, 0, sizeof(props));
    vkGetPhysicalDeviceProperties(phys[0], &props);
    unsigned maj = (props.apiVersion >> 22) & 0x7f;
    unsigned min = (props.apiVersion >> 12) & 0x3ff;
    printf("alr vk icd client device0 name=\"%s\" api=%u.%u vendorID=0x%x type=%d\n",
           props.deviceName, maj, min, props.vendorID, (int)props.deviceType);

    /* The PASS condition: a real Mali device name surfaced THROUGH the ICD. */
    pass = (strstr(props.deviceName, "Mali") != NULL);

    /* Bonus rung — queue families + logical device + queue (best-effort; does not
     * gate PASS, but exercises the create-device/get-queue marshalling end to end). */
    {
        uint32_t qf = 0;
        VkQueueFamilyProperties qfp[8];
        vkGetPhysicalDeviceQueueFamilyProperties(phys[0], &qf, NULL);
        if (qf > 8) qf = 8;
        if (qf) vkGetPhysicalDeviceQueueFamilyProperties(phys[0], &qf, qfp);
        printf("alr vk icd client queue families=%u%s\n", qf,
               (qf && (qfp[0].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ? " (qf0 has GRAPHICS)" : "");

        VkDevice dev = VK_NULL_HANDLE;
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
        if (r == VK_SUCCESS && dev != VK_NULL_HANDLE) {
            VkQueue q = VK_NULL_HANDLE;
            vkGetDeviceQueue(dev, 0, 0, &q);
            printf("alr vk icd client create-device=ok get-queue=%s\n",
                   q != VK_NULL_HANDLE ? "ok" : "null");
            vkDestroyDevice(dev, NULL);
        } else {
            printf("alr vk icd client create-device result=%d\n", (int)r);
        }
    }

    vkDestroyInstance(inst, NULL);

    printf("ALR VK ICD CLIENT: %s device=%s api=%u.%u\n",
           pass ? "PASS" : "FAIL", props.deviceName, maj, min);
    return pass ? 0 : 1;
}
