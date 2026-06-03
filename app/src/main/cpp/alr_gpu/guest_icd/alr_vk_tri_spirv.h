/* alr_vk_tri_spirv.h — the GUEST triangle app's OWN SPIR-V (vert + frag), as C arrays.
 *
 * This is what a real guest Vulkan app would carry and hand to vkCreateShaderModule. The
 * ALR ICD marshals these blobs over the wire (ALR_VK_OP_CREATE_SHADER_MODULE) to the
 * app-process host, which vkCreateShaderModule's them on the REAL vendor Mali libvulkan —
 * so the GUEST's shaders run on the GPU (not host-embedded SPIR-V). The bytes below are
 * the SAME validated triangle SPIR-V the host vendors (alr_gpu_vk_decode.hpp
 * kAlrVkTriVertSpv/kAlrVkTriFragSpv); shipping them FROM THE GUEST over the wire is the
 * VK-M4 milestone. Keeping a checked-in copy (vs. a build-time glslc dep) keeps the build
 * reproducible on a host without the shader toolchain.
 *
 * GLSL SOURCES (regenerable; identical to the host's, kept here for the guest's audit):
 *   alr_tri.vert (#version 450):
 *       layout(location = 0) in vec2 inPos;
 *       void main() { gl_Position = vec4(inPos, 0.0, 1.0); }
 *   alr_tri.frag (#version 450):
 *       layout(location = 0) out vec4 outColor;
 *       void main() { outColor = vec4(0.95, 0.10, 0.80, 1.0); }   // ALR magenta
 *
 *   $NDK/shader-tools/<host>/glslc --target-env=vulkan1.1 -O alr_tri.vert -o v.spv
 *   $NDK/shader-tools/<host>/glslc --target-env=vulkan1.1 -O alr_tri.frag -o f.spv
 *   # then each .spv emitted as little-endian uint32 words.
 *
 * The frag color is a baked constant (no descriptor set / push constant) so the pipeline
 * layout is empty — maximally portable on Mali — and the presented center pixel is a fixed
 * ~(242, 26, 204) the host (and a headless self-test) can assert.
 */
#ifndef ALR_VK_TRI_SPIRV_H
#define ALR_VK_TRI_SPIRV_H

#include <stdint.h>

/* Vertex shader: vec2 inPos (location 0) -> gl_Position. SPIR-V 1.3 (word[1]=0x00010300). */
static const uint32_t kAlrTriVertSpv[] = {
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

/* Fragment shader: out vec4 = (0.95, 0.10, 0.80, 1.0). */
static const uint32_t kAlrTriFragSpv[] = {
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

#endif /* ALR_VK_TRI_SPIRV_H */
