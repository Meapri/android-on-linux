// Host wire test for the GENERATED Vulkan passthrough batch (the codegen output of
// tools/gen_vk_passthrough.py). NO Vulkan SDK required: a synthetic Mali provider answers
// the generated ops, so this proves the REQUEST -> host-decode -> REPLY round trip for the
// first render batch (CreateCommandPool / AllocateMemory / MapMemory / CreateBuffer /
// CreateImage / Get*MemoryRequirements / Bind*Memory / CreateImageView / Destroy*) entirely
// on the host — the same way native_vk_marshal_test.cpp proves the hand-written 200..229
// band. It ALSO proves the same-process MAP_SHARED arena: vkAllocateMemory hands back an
// arena offset, and vkMapMemory turns it into a real, writable arena pointer (zero-copy).
//
// Built + run by scripts/test-native-core.sh alongside native_vk_marshal_test.cpp.

#include "alr_gpu/alr_gpu_vk_decode.hpp"
#include "alr_gpu/generated/alr_gpu_vk_arena.hpp"
#include "alr_gpu/generated/alr_gpu_vk_gen_decode.hpp"
#include "alr_gpu/generated/alr_gpu_vk_gen_proto.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace alr::gpu;

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { printf("FAIL %s\n", what); ++failures; }
}

// ---- A synthetic "Mali" for the generated ops: it tracks virtual ids, carves arena slabs
// for host-visible allocations (the SAME arena the device path uses), and answers
// get-reqs/bind/map. It is the no-SDK stand-in for vk_gen_real_*. ----
struct SynthGen {
    // Per-virtual-memory arena offset (so map returns a real pointer) + a fixed "driver"
    // memory-type bits for created buffers/images.
    std::map<uint32_t, uint64_t> mem_off;
    int created_pools = 0, created_buffers = 0, created_images = 0, created_views = 0;
    int destroyed = 0;

    static int s_create(void* ctx, uint16_t op, uint32_t vdev, uint32_t vh, uint64_t a,
                        uint64_t b) {
        auto* s = static_cast<SynthGen*>(ctx);
        (void)vdev; (void)vh; (void)a; (void)b;
        if (op == ALR_VK_GEN_OP_CREATE_COMMAND_POOL) s->created_pools++;
        else if (op == ALR_VK_GEN_OP_CREATE_BUFFER) s->created_buffers++;
        else if (op == ALR_VK_GEN_OP_CREATE_IMAGE) s->created_images++;
        else if (op == ALR_VK_GEN_OP_CREATE_IMAGE_VIEW) s->created_views++;
        return 0;  // VK_SUCCESS
    }
    static int s_alloc(void* ctx, uint32_t vdev, uint32_t vmem, uint64_t size,
                       uint32_t mem_type, uint64_t* arena_off_out) {
        auto* s = static_cast<SynthGen*>(ctx);
        (void)vdev; (void)mem_type;
        // Carve a real arena slab (the same primitive the device path uses).
        const uint64_t off = alr_vk_arena_alloc(size ? size : 4096);
        s->mem_off[vmem] = off;
        if (arena_off_out) *arena_off_out = off;
        return off == kAlrVkArenaNoOffset ? -1 : 0;
    }
    static bool s_map(void* ctx, uint32_t vdev, uint32_t vmem, uint64_t* off_out) {
        auto* s = static_cast<SynthGen*>(ctx);
        (void)vdev;
        auto it = s->mem_off.find(vmem);
        if (it == s->mem_off.end()) return false;
        if (off_out) *off_out = it->second;
        return true;
    }
    static int s_reqs(void* ctx, uint16_t op, uint32_t vdev, uint32_t vh, uint64_t* size_out,
                      uint64_t* align_out, uint32_t* bits_out) {
        (void)ctx; (void)op; (void)vdev; (void)vh;
        if (size_out) *size_out = 65536;    // a plausible buffer/image size
        if (align_out) *align_out = 256;
        if (bits_out) *bits_out = 0x3;       // two memory types usable
        return 0;
    }
    static int s_bind(void* ctx, uint16_t op, uint32_t vdev, uint32_t vh, uint32_t vmem,
                      uint64_t off) {
        (void)ctx; (void)op; (void)vdev; (void)vh; (void)vmem; (void)off;
        return 0;  // VK_SUCCESS
    }
    static void s_destroy(void* ctx, uint16_t op, uint32_t vdev, uint32_t vh) {
        auto* s = static_cast<SynthGen*>(ctx);
        (void)op; (void)vdev; (void)vh;
        s->destroyed++;
    }
    VkGenProvider as_provider() {
        VkGenProvider p;
        p.create_handle = &s_create;
        p.alloc_memory = &s_alloc;
        p.map_memory = &s_map;
        p.get_reqs = &s_reqs;
        p.bind_memory = &s_bind;
        p.destroy_handle = &s_destroy;
        p.ctx = this;
        return p;
    }
};

// ---- A tiny host-side reply reader for the generated reply records (escape + u16 sub +
// payload). The guest ICD has its own C scanners; the host test reads the stream directly
// to assert the round trip. ----
struct GenReplies {
    std::map<uint32_t, int32_t> pool_results;     // vhandle -> result
    std::map<uint32_t, std::pair<int32_t, uint64_t>> allocs;  // vmem -> (result, arena_off)
    std::map<uint32_t, std::pair<int32_t, uint64_t>> maps;    // vmem -> (result, mapped_off)
    std::map<uint32_t, std::tuple<int32_t, uint64_t, uint64_t, uint32_t>> reqs;  // vh -> (res,size,align,bits)
    std::map<uint32_t, int32_t> binds;            // vhandle -> result
    std::map<uint32_t, int32_t> creates;          // vhandle -> result (buffer/image/view/pool)
    bool ok = true;
};

static bool decode_gen_replies(const std::vector<uint8_t>& bytes, GenReplies& out) {
    VkReader r(bytes.data(), bytes.size());
    for (;;) {
        uint8_t op = 0;
        if (!r.u8(op)) break;
        if (op == ALR_VK_REPLY_END) break;
        if (op != ALR_VK_REPLY_GEN_ESCAPE) { out.ok = false; return false; }
        uint16_t sub = 0;
        if (!r.u16(sub)) { out.ok = false; return false; }
        switch (sub) {
            case ALR_VK_GEN_REPLY_CREATE_COMMAND_POOL:
            case ALR_VK_GEN_REPLY_CREATE_BUFFER:
            case ALR_VK_GEN_REPLY_CREATE_IMAGE:
            case ALR_VK_GEN_REPLY_CREATE_IMAGE_VIEW: {
                uint32_t vh = 0; int32_t res = 0;
                if (!r.u32(vh) || !r.i32(res)) { out.ok = false; return false; }
                out.creates[vh] = res;
                if (sub == ALR_VK_GEN_REPLY_CREATE_COMMAND_POOL) out.pool_results[vh] = res;
                break;
            }
            case ALR_VK_GEN_REPLY_ALLOCATE_MEMORY: {
                uint32_t vmem = 0; int32_t res = 0; uint64_t off = 0;
                if (!r.u32(vmem) || !r.i32(res) || !r.u64(off)) { out.ok = false; return false; }
                out.allocs[vmem] = {res, off};
                break;
            }
            case ALR_VK_GEN_REPLY_MAP_MEMORY: {
                uint32_t vmem = 0; uint64_t off = 0; int32_t res = 0;
                if (!r.u32(vmem) || !r.u64(off) || !r.i32(res)) { out.ok = false; return false; }
                out.maps[vmem] = {res, off};
                break;
            }
            case ALR_VK_GEN_REPLY_GET_BUFFER_MEMORY_REQUIREMENTS:
            case ALR_VK_GEN_REPLY_GET_IMAGE_MEMORY_REQUIREMENTS: {
                uint32_t vh = 0; int32_t res = 0; uint64_t size = 0, align = 0; uint32_t bits = 0;
                if (!r.u32(vh) || !r.i32(res) || !r.u64(size) || !r.u64(align) || !r.u32(bits)) {
                    out.ok = false; return false;
                }
                out.reqs[vh] = {res, size, align, bits};
                break;
            }
            case ALR_VK_GEN_REPLY_BIND_BUFFER_MEMORY:
            case ALR_VK_GEN_REPLY_BIND_IMAGE_MEMORY: {
                uint32_t vh = 0; int32_t res = 0;
                if (!r.u32(vh) || !r.i32(res)) { out.ok = false; return false; }
                out.binds[vh] = res;
                break;
            }
            default:
                out.ok = false;
                return false;
        }
    }
    return out.ok;
}

int main() {
    // The arena the alloc/map ops use (anonymous mmap on this host; memfd on device).
    check(alr_vk_arena_create(8u << 20), "arena create");

    // Virtual ids (disjoint, mirroring the ICD's monotonic pools).
    const uint32_t kVdev = 1000, kVpool = 3000, kVmem = 9000, kVbuf = 7000, kVimg = 8000,
                   kVview = 8500;

    // 1) The dispatcher must be registered just by including the generated header.
    check(vk_gen_dispatch() != nullptr, "generated dispatcher registered into decode seam");

    // 2) Build ONE batch exercising the whole first render batch and run it through
    //    decode_vk_batch (NOT decode_vk_gen_op directly) — proving the escape hand-off from
    //    the hand-written decoder into the generated band works.
    std::vector<uint8_t> req(2048);
    AlrVkEncoder e;
    alr_vk_enc_init(&e, req.data(), static_cast<uint32_t>(req.size()));
    // command pool
    alr_vk_enc_gen_create_command_pool_begin(&e, kVdev, kVpool, /*flags=*/0x2u,
                                             /*queueFamilyIndex=*/0u);
    alr_vk_gen_pnext_count(&e, 0);
    // buffer + its reqs + a memory alloc + bind + map
    alr_vk_enc_gen_create_buffer_begin(&e, kVdev, kVbuf, /*flags=*/0u, /*size=*/65536ull,
                                       /*usage=*/0x80u /*VERTEX*/, /*sharingMode=*/0u);
    alr_vk_gen_pnext_count(&e, 0);
    alr_vk_enc_gen_get_buffer_memory_requirements(&e, kVdev, kVbuf);
    alr_vk_enc_gen_allocate_memory_begin(&e, kVdev, kVmem, /*allocationSize=*/65536ull,
                                         /*memoryTypeIndex=*/0u);
    alr_vk_gen_pnext_count(&e, 0);
    alr_vk_enc_gen_bind_buffer_memory(&e, kVdev, kVbuf, kVmem, /*memoryOffset=*/0ull);
    alr_vk_enc_gen_map_memory(&e, kVdev, kVmem, /*offset=*/0ull, /*size=*/65536ull);
    // image + its reqs + view
    alr_vk_enc_gen_create_image_begin(&e, kVdev, kVimg, /*flags=*/0u, /*imageType=*/1u /*2D*/,
                                      /*format=*/37u /*R8G8B8A8_UNORM*/, /*w=*/128u, /*h=*/128u,
                                      /*d=*/1u, /*mip=*/1u, /*layers=*/1u, /*samples=*/1u,
                                      /*tiling=*/0u, /*usage=*/0x10u /*COLOR_ATTACHMENT*/,
                                      /*sharingMode=*/0u, /*initialLayout=*/0u);
    alr_vk_gen_pnext_count(&e, 0);
    alr_vk_enc_gen_get_image_memory_requirements(&e, kVdev, kVimg);
    alr_vk_enc_gen_create_image_view_begin(&e, kVdev, kVview, /*flags=*/0u, /*image=*/kVimg,
                                           /*viewType=*/1u /*2D*/, /*format=*/37u,
                                           /*comp r,g,b,a=*/0u, 0u, 0u, 0u,
                                           /*aspect=*/0x1u /*COLOR*/, 0u, 1u, 0u, 1u);
    alr_vk_gen_pnext_count(&e, 0);
    // tear the handles down
    alr_vk_enc_gen_destroy_image_view(&e, kVdev, kVview);
    alr_vk_enc_gen_destroy_image(&e, kVdev, kVimg);
    alr_vk_enc_gen_destroy_buffer(&e, kVdev, kVbuf);
    alr_vk_enc_gen_free_memory(&e, kVdev, kVmem);
    alr_vk_enc_gen_destroy_command_pool(&e, kVdev, kVpool);
    alr_vk_enc_u8(&e, static_cast<uint8_t>(ALR_VK_OP_END));
    check(!e.overflow, "request batch encodes within buffer");

    SynthGen syn;
    VkGenProvider gp = syn.as_provider();
    set_vk_gen_provider(&gp);  // decode_vk_batch forwards this opaque ptr to the gen band

    VkDecodeState st;
    VkReplyEncoder reply;
    const bool ok = decode_vk_batch(req.data(), e.len, st, reply, /*provider=*/nullptr);
    set_vk_gen_provider(nullptr);
    check(ok, "decode_vk_batch (with generated escape) ok");
    // 13 generated ops dispatched (pool, buffer, get-reqs, alloc, bind, map, image,
    // get-reqs, view, + 5 destroys = 14). Count via st.decoded.
    check(st.decoded == 14, "14 generated ops dispatched through the escape");
    check(syn.created_pools == 1 && syn.created_buffers == 1 && syn.created_images == 1 &&
          syn.created_views == 1, "synthetic Mali created pool+buffer+image+view");
    check(syn.destroyed == 5, "5 destroys dispatched");

    // 3) Decode the reply stream + assert every round-tripping op came back.
    GenReplies dr;
    check(decode_gen_replies(reply.bytes(), dr), "generated reply stream decodes");
    check(dr.creates.count(kVpool) && dr.creates[kVpool] == 0, "command pool create OK");
    check(dr.creates.count(kVbuf) && dr.creates[kVbuf] == 0, "buffer create OK");
    check(dr.creates.count(kVimg) && dr.creates[kVimg] == 0, "image create OK");
    check(dr.creates.count(kVview) && dr.creates[kVview] == 0, "image view create OK");
    check(dr.binds.count(kVbuf) && dr.binds[kVbuf] == 0, "bind buffer memory OK");
    check(dr.reqs.count(kVbuf), "buffer mem-reqs returned");
    if (dr.reqs.count(kVbuf)) {
        check(std::get<1>(dr.reqs[kVbuf]) == 65536, "buffer reqs size round-trips");
        check(std::get<3>(dr.reqs[kVbuf]) == 0x3u, "buffer reqs memoryTypeBits round-trips");
    }
    check(dr.reqs.count(kVimg), "image mem-reqs returned");

    // 4) The SAME-PROCESS ARENA: alloc returned an offset, and map turned it into a
    //    usable pointer. This is the zero-copy keystone (a guest write through this pointer
    //    lands in the memory the real VkDeviceMemory aliases).
    check(dr.allocs.count(kVmem) && dr.allocs[kVmem].first == 0, "memory alloc OK");
    const uint64_t alloc_off = dr.allocs.count(kVmem) ? dr.allocs[kVmem].second : kAlrVkArenaNoOffset;
    check(alloc_off != kAlrVkArenaNoOffset, "alloc returned an arena offset");
    check(dr.maps.count(kVmem) && dr.maps[kVmem].first == 0, "map memory OK");
    const uint64_t mapped_off = dr.maps.count(kVmem) ? dr.maps[kVmem].second : kAlrVkArenaNoOffset;
    check(mapped_off == alloc_off, "mapped offset == alloc offset (offset 0 into the slab)");
    void* host_ptr = alr_vk_arena_ptr(mapped_off);
    check(host_ptr != nullptr, "arena offset resolves to a real pointer");
    if (host_ptr) {
        // Prove it is genuinely writable shared arena memory.
        std::memset(host_ptr, 0xAB, 64);
        const auto* b = static_cast<const uint8_t*>(host_ptr);
        check(b[0] == 0xAB && b[63] == 0xAB, "mapped arena pointer is writable");
    }

    // 5) Malformed: an escape with a truncated sub-opcode must fail-stop, not over-read.
    {
        uint8_t bad[1] = {static_cast<uint8_t>(ALR_VK_OP_GEN_ESCAPE)};  // escape, no sub
        VkDecodeState st2;
        VkReplyEncoder reply2;
        set_vk_gen_provider(&gp);
        const bool bok = decode_vk_batch(bad, sizeof(bad), st2, reply2, nullptr);
        set_vk_gen_provider(nullptr);
        check(!bok, "escape with truncated sub-opcode fails cleanly");
    }

    // 6) An escape with an UNKNOWN sub-opcode must fail-stop (never accept off-contract).
    {
        std::vector<uint8_t> bad(8, 0);
        AlrVkEncoder be;
        alr_vk_enc_init(&be, bad.data(), static_cast<uint32_t>(bad.size()));
        alr_vk_gen_op_begin(&be, /*sub=*/60000);  // not a defined sub-op
        bad.resize(be.len);
        VkDecodeState st3;
        VkReplyEncoder reply3;
        const bool bok = decode_vk_batch(bad.data(), bad.size(), st3, reply3, nullptr);
        check(!bok, "escape with unknown sub-opcode fails cleanly");
    }

    // 7) WAVE D — the DEEPEST create: vkCreateGraphicsPipelines + vkCreateComputePipelines +
    //    vkDestroyPipeline through the escape band. A graphics pipeline with one vertex stage
    //    (+ a specialization-info: map entries + a data blob) and one fragment stage, then the
    //    full fixed-function chain — vertex input (a binding + an attr), input assembly,
    //    viewport (1 vp + 1 scissor), rasterization, multisample (+ a sample-mask word),
    //    depth-stencil (front+back), color blend (1 attachment), dynamic state (2 states) —
    //    with tessellation ABSENT (present=0). This exercises the lock-step nested decode end
    //    to end (a wrong byte count anywhere desyncs + fails). The synthetic provider answers
    //    via create_handle; the reply carries { pipeline_count, result }.
    {
        const uint32_t kVdev2 = 1001, kVgfx = 6000, kVcomp = 6001, kVlayout = 5000,
                       kVrpass = 4000, kVmod = 2000, kVpcache2 = 3500;
        std::vector<uint8_t> preq(4096);
        AlrVkEncoder pe;
        alr_vk_enc_init(&pe, preq.data(), static_cast<uint32_t>(preq.size()));
        // ---- graphics ----
        alr_vk_enc_gen_create_graphics_pipelines_begin(&pe, kVdev2, kVpcache2, /*count=*/1u);
        alr_vk_enc_gen_create_graphics_pipelines_pipeline(&pe, kVgfx, /*flags=*/0u, kVlayout,
                                                          kVrpass, /*subpass=*/0u, /*vbase=*/0u,
                                                          /*base_index=*/-1, /*stage_count=*/2u);
        // vertex stage with a specialization-info (2 map entries + 8 data bytes)
        const char* vname = "main";
        alr_vk_enc_gen_create_graphics_pipelines_stage(&pe, /*VERTEX=*/0x1u, kVmod, vname,
                                                       (uint32_t)std::strlen(vname),
                                                       /*spec_present=*/1u);
        alr_vk_enc_gen_create_graphics_pipelines_stage_spec_begin(&pe, /*map_entries=*/2u,
                                                                  /*data_len=*/8u);
        alr_vk_enc_gen_create_graphics_pipelines_stage_spec_entry(&pe, /*id=*/0u, /*off=*/0u, /*sz=*/4u);
        alr_vk_enc_gen_create_graphics_pipelines_stage_spec_entry(&pe, /*id=*/1u, /*off=*/4u, /*sz=*/4u);
        const uint8_t spec_bytes[8] = {1, 0, 0, 0, 2, 0, 0, 0};
        alr_vk_enc_gen_create_graphics_pipelines_stage_spec_data(&pe, spec_bytes, 8u);
        // fragment stage, no spec
        const char* fname = "main";
        alr_vk_enc_gen_create_graphics_pipelines_stage(&pe, /*FRAGMENT=*/0x10u, kVmod, fname,
                                                       (uint32_t)std::strlen(fname),
                                                       /*spec_present=*/0u);
        // fixed-function chain
        alr_vk_enc_gen_create_graphics_pipelines_vertex_input(&pe, /*present=*/1u, /*bind=*/1u, /*attr=*/1u);
        alr_vk_enc_gen_create_graphics_pipelines_vertex_binding(&pe, /*binding=*/0u, /*stride=*/16u, /*rate=*/0u);
        alr_vk_enc_gen_create_graphics_pipelines_vertex_attr(&pe, /*loc=*/0u, /*bind=*/0u, /*fmt=*/103u, /*off=*/0u);
        alr_vk_enc_gen_create_graphics_pipelines_input_assembly(&pe, /*present=*/1u, /*topology=*/3u, /*restart=*/0u);
        alr_vk_enc_gen_create_graphics_pipelines_tessellation(&pe, /*present=*/0u, 0u);  // ABSENT
        alr_vk_enc_gen_create_graphics_pipelines_viewport(&pe, /*present=*/1u, /*vp=*/1u, /*sc=*/1u);
        alr_vk_enc_gen_create_graphics_pipelines_viewport_elem(&pe, 0.f, 0.f, 1920.f, 1080.f, 0.f, 1.f);
        alr_vk_enc_gen_create_graphics_pipelines_scissor_elem(&pe, 0, 0, 1920u, 1080u);
        alr_vk_enc_gen_create_graphics_pipelines_rasterization(&pe, /*present=*/1u, 0u, 0u, /*fill=*/0u,
                                                               /*cull=*/2u, /*ccw=*/1u, 0u, 0.f, 0.f, 0.f, 1.f);
        alr_vk_enc_gen_create_graphics_pipelines_multisample(&pe, /*present=*/1u, /*samples=*/1u, 0u, 0.f,
                                                             /*mask_words=*/1u, 0u, 0u);
        alr_vk_enc_gen_create_graphics_pipelines_sample_mask(&pe, 0xFFFFFFFFu);
        alr_vk_enc_gen_create_graphics_pipelines_depth_stencil(&pe, /*present=*/1u, 1u, 1u, /*LE=*/3u, 0u, 0u, 0.f, 1.f);
        alr_vk_enc_gen_create_graphics_pipelines_stencil_op(&pe, 0u, 0u, 0u, 0u, 0u, 0u, 0u);  // front
        alr_vk_enc_gen_create_graphics_pipelines_stencil_op(&pe, 0u, 0u, 0u, 0u, 0u, 0u, 0u);  // back
        alr_vk_enc_gen_create_graphics_pipelines_color_blend(&pe, /*present=*/1u, 0u, 0u, /*att=*/1u, 0.f, 0.f, 0.f, 0.f);
        alr_vk_enc_gen_create_graphics_pipelines_blend_attachment(&pe, 0u, 0u, 0u, 0u, 0u, 0u, 0u, /*RGBA=*/0xFu);
        alr_vk_enc_gen_create_graphics_pipelines_dynamic_state(&pe, /*present=*/1u, /*count=*/2u);
        alr_vk_enc_gen_create_graphics_pipelines_dynamic_elem(&pe, /*VIEWPORT=*/0u);
        alr_vk_enc_gen_create_graphics_pipelines_dynamic_elem(&pe, /*SCISSOR=*/1u);
        // ---- compute (single stage, no fixed-function) ----
        alr_vk_enc_gen_create_compute_pipelines_begin(&pe, kVdev2, /*vpcache=*/0u, /*count=*/1u);
        alr_vk_enc_gen_create_compute_pipelines_pipeline(&pe, kVcomp, /*flags=*/0u, kVlayout,
                                                         /*vrpass=*/0u, /*subpass=*/0u, /*vbase=*/0u,
                                                         /*base_index=*/-1, /*stage_count=*/1u);
        const char* cname = "main";
        alr_vk_enc_gen_create_compute_pipelines_stage(&pe, /*COMPUTE=*/0x20u, kVmod, cname,
                                                      (uint32_t)std::strlen(cname), /*spec_present=*/0u);
        // ---- destroy both ----
        alr_vk_enc_gen_destroy_pipeline(&pe, kVdev2, kVgfx);
        alr_vk_enc_gen_destroy_pipeline(&pe, kVdev2, kVcomp);
        alr_vk_enc_u8(&pe, static_cast<uint8_t>(ALR_VK_OP_END));
        check(!pe.overflow, "pipeline batch encodes within buffer");

        SynthGen syn2;
        VkGenProvider gp2 = syn2.as_provider();
        set_vk_gen_provider(&gp2);
        VkDecodeState pst;
        VkReplyEncoder preply;
        const bool pok = decode_vk_batch(preq.data(), pe.len, pst, preply, nullptr);
        set_vk_gen_provider(nullptr);
        check(pok, "decode_vk_batch (graphics+compute pipelines, deep nested) ok");
        // 2 creates + 2 destroys = 4 generated ops dispatched.
        check(pst.decoded == 4, "4 pipeline ops dispatched through the escape");
        check(syn2.destroyed == 2, "2 pipeline destroys dispatched");
        // The reply stream carries one { pipeline_count=1, result=0 } per create.
        VkReader pr(preply.bytes().data(), preply.bytes().size());
        int pipe_replies = 0;
        for (;;) {
            uint8_t op = 0;
            if (!pr.u8(op) || op == ALR_VK_REPLY_END) break;
            check(op == ALR_VK_REPLY_GEN_ESCAPE, "pipeline reply rides the gen escape");
            uint16_t sub = 0; uint32_t cnt = 0; int32_t res = 0;
            if (!pr.u16(sub) || !pr.u32(cnt) || !pr.i32(res)) { check(false, "pipeline reply truncated"); break; }
            check(sub == ALR_VK_GEN_REPLY_CREATE_GRAPHICS_PIPELINES ||
                  sub == ALR_VK_GEN_REPLY_CREATE_COMPUTE_PIPELINES, "pipeline reply sub-op");
            check(cnt == 1 && res == 0, "pipeline create reply: count=1 result=0");
            ++pipe_replies;
        }
        check(pipe_replies == 2, "both pipeline creates replied");
    }

    // 8) Pipeline lock-step desync: a graphics _pipeline that promises 1 stage but is then
    //    truncated mid-stage must fail-stop (proves the nested reader is bounded, not trusting).
    {
        std::vector<uint8_t> bad(64, 0);
        AlrVkEncoder be;
        alr_vk_enc_init(&be, bad.data(), static_cast<uint32_t>(bad.size()));
        alr_vk_enc_gen_create_graphics_pipelines_begin(&be, 1u, 0u, /*count=*/1u);
        alr_vk_enc_gen_create_graphics_pipelines_pipeline(&be, 7u, 0u, 5u, 4u, 0u, 0u, -1, /*stages=*/1u);
        // ...and stop: no stage bytes follow. The decode must not over-read.
        bad.resize(be.len);
        VkDecodeState st4;
        VkReplyEncoder reply4;
        const bool bok = decode_vk_batch(bad.data(), bad.size(), st4, reply4, nullptr);
        check(!bok, "truncated pipeline stage fails cleanly (bounded nested read)");
    }

    if (failures == 0) {
        printf("native_vk_gen_passthrough_test: ALL PASS (generated render batch: "
               "pool+memory(arena)+buffer+image+view + the deep-nested graphics/compute "
               "pipeline create-forwards, all marshalled through the escape band)\n");
        return 0;
    }
    printf("native_vk_gen_passthrough_test: %d FAILURE(S)\n", failures);
    return 1;
}
