// ==========================================================================
//  GENERATED FILE — DO NOT EDIT BY HAND.
//  Produced by tools/gen_vk_passthrough.py from tools/vk_registry/vk.xml
//  (Vulkan-Headers v1.3.275, VK_HEADER_VERSION 275). Regenerate with:
//      python3 tools/gen_vk_passthrough.py
//  Edit the SPECS in that tool, not this file. The wire opcodes here are
//  APPEND-ONLY (they are the on-the-wire ABI shared with the host decoder).
// ==========================================================================

// Generated ICD-side reply scanners. They walk the generated reply stream the host
// wrote (each record is u8 ALR_VK_REPLY_GEN_ESCAPE + u16 sub-opcode + payload,
// terminated by u8 ALR_VK_REPLY_END) and pull the fields each round-tripping op
// needs. Generated so the field order can never drift from the host encoder. They
// use the ICD's AlrVkReader (a tiny LE reader the ICD defines).
#ifdef ALR_ICD_GEN_DEFINE

/* Forward decls (the scanners below call skip/next, which are defined after them). */
static int alr_icd_gen_skip(AlrVkReader *rd, uint16_t sub);
static int alr_icd_gen_next(AlrVkReader *rd, uint16_t *sub);

/* Advance `rd` past ONE generated reply record we are not collecting (its sub-op is
 * `sub`), so the scanner can keep walking. Byte sizes mirror the host reply encoder. */
static int alr_icd_gen_skip(AlrVkReader *rd, uint16_t sub) {
    uint32_t u; int32_t i; uint64_t q;
    switch (sub) {
        case (uint16_t)ALR_VK_GEN_REPLY_CREATE_COMMAND_POOL: { if(!alr_vk_reader_u32(rd,&u)||!alr_vk_reader_i32(rd,&i)) return 0; } return 1;
        case (uint16_t)ALR_VK_GEN_REPLY_ALLOCATE_MEMORY: { if(!alr_vk_reader_u32(rd,&u)||!alr_vk_reader_i32(rd,&i)||!alr_vk_reader_u64(rd,&q)) return 0; } return 1;
        case (uint16_t)ALR_VK_GEN_REPLY_MAP_MEMORY: { if(!alr_vk_reader_u32(rd,&u)||!alr_vk_reader_u64(rd,&q)||!alr_vk_reader_i32(rd,&i)) return 0; } return 1;
        case (uint16_t)ALR_VK_GEN_REPLY_CREATE_BUFFER: { if(!alr_vk_reader_u32(rd,&u)||!alr_vk_reader_i32(rd,&i)) return 0; } return 1;
        case (uint16_t)ALR_VK_GEN_REPLY_GET_BUFFER_MEMORY_REQUIREMENTS: { if(!alr_vk_reader_u32(rd,&u)||!alr_vk_reader_i32(rd,&i)||!alr_vk_reader_u64(rd,&q)||!alr_vk_reader_u64(rd,&q)||!alr_vk_reader_u32(rd,&u)) return 0; } return 1;
        case (uint16_t)ALR_VK_GEN_REPLY_BIND_BUFFER_MEMORY: { if(!alr_vk_reader_u32(rd,&u)||!alr_vk_reader_i32(rd,&i)) return 0; } return 1;
        case (uint16_t)ALR_VK_GEN_REPLY_CREATE_IMAGE: { if(!alr_vk_reader_u32(rd,&u)||!alr_vk_reader_i32(rd,&i)) return 0; } return 1;
        case (uint16_t)ALR_VK_GEN_REPLY_GET_IMAGE_MEMORY_REQUIREMENTS: { if(!alr_vk_reader_u32(rd,&u)||!alr_vk_reader_i32(rd,&i)||!alr_vk_reader_u64(rd,&q)||!alr_vk_reader_u64(rd,&q)||!alr_vk_reader_u32(rd,&u)) return 0; } return 1;
        case (uint16_t)ALR_VK_GEN_REPLY_BIND_IMAGE_MEMORY: { if(!alr_vk_reader_u32(rd,&u)||!alr_vk_reader_i32(rd,&i)) return 0; } return 1;
        case (uint16_t)ALR_VK_GEN_REPLY_CREATE_IMAGE_VIEW: { if(!alr_vk_reader_u32(rd,&u)||!alr_vk_reader_i32(rd,&i)) return 0; } return 1;
        default: return 0;  /* unknown record: stop (don't guess a size) */
    }
    (void)u; (void)i; (void)q;
}

/* Read the next generated reply record's sub-opcode into *sub. Returns 1 if a record
 * is present (escape byte consumed + sub read), 0 at ALR_VK_REPLY_END / on malformed
 * input. A non-escape, non-END byte is treated as end-of-generated-records (a
 * hand-written reply record the generated scanner doesn't own). */
static int alr_icd_gen_next(AlrVkReader *rd, uint16_t *sub) {
    uint8_t op;
    if (!alr_vk_reader_u8(rd, &op)) return 0;
    if (op == (uint8_t)ALR_VK_REPLY_END) return 0;
    if (op != (uint8_t)ALR_VK_REPLY_GEN_ESCAPE) return 0;
    return alr_vk_reader_u16(rd, sub);
}

/* { u32 vid, i32 result }. */
static int alr_icd_gen_scan_result(const uint8_t *buf, uint32_t len, uint16_t want,
                                   int32_t *res_out) {
    AlrVkReader rd; alr_vk_reader_init(&rd, buf, len); uint16_t sub;
    while (alr_icd_gen_next(&rd, &sub)) {
        if (sub == want) { uint32_t vid; int32_t res;
            if (!alr_vk_reader_u32(&rd, &vid) || !alr_vk_reader_i32(&rd, &res)) return 0;
            if (res_out) *res_out = res; return 1; }
        if (!alr_icd_gen_skip(&rd, sub)) return 0; }
    return 0;
}

/* { u32 vid, i32 result, u64 arena_off }. */
static int alr_icd_gen_scan_alloc(const uint8_t *buf, uint32_t len, uint16_t want,
                                  int32_t *res_out, uint64_t *arena_out) {
    AlrVkReader rd; alr_vk_reader_init(&rd, buf, len); uint16_t sub;
    while (alr_icd_gen_next(&rd, &sub)) {
        if (sub == want) { uint32_t vid; int32_t res; uint64_t off;
            if (!alr_vk_reader_u32(&rd, &vid) || !alr_vk_reader_i32(&rd, &res) ||
                !alr_vk_reader_u64(&rd, &off)) return 0;
            if (res_out) *res_out = res; if (arena_out) *arena_out = off; return 1; }
        if (!alr_icd_gen_skip(&rd, sub)) return 0; }
    return 0;
}

/* { u32 vid, u64 mapped_off, i32 result }. */
static int alr_icd_gen_scan_map(const uint8_t *buf, uint32_t len, uint16_t want,
                                int32_t *res_out, uint64_t *off_out) {
    AlrVkReader rd; alr_vk_reader_init(&rd, buf, len); uint16_t sub;
    while (alr_icd_gen_next(&rd, &sub)) {
        if (sub == want) { uint32_t vid; uint64_t off; int32_t res;
            if (!alr_vk_reader_u32(&rd, &vid) || !alr_vk_reader_u64(&rd, &off) ||
                !alr_vk_reader_i32(&rd, &res)) return 0;
            if (off_out) *off_out = off; if (res_out) *res_out = res; return 1; }
        if (!alr_icd_gen_skip(&rd, sub)) return 0; }
    return 0;
}

/* { u32 vid, i32 result, u64 size, u64 align, u32 bits }. */
static int alr_icd_gen_scan_reqs(const uint8_t *buf, uint32_t len, uint16_t want,
                                 int32_t *res_out, uint64_t *size_out,
                                 uint64_t *align_out, uint32_t *bits_out) {
    AlrVkReader rd; alr_vk_reader_init(&rd, buf, len); uint16_t sub;
    while (alr_icd_gen_next(&rd, &sub)) {
        if (sub == want) { uint32_t vid; int32_t res; uint64_t size, align; uint32_t bits;
            if (!alr_vk_reader_u32(&rd, &vid) || !alr_vk_reader_i32(&rd, &res) ||
                !alr_vk_reader_u64(&rd, &size) || !alr_vk_reader_u64(&rd, &align) ||
                !alr_vk_reader_u32(&rd, &bits)) return 0;
            if (res_out) *res_out = res; if (size_out) *size_out = size;
            if (align_out) *align_out = align; if (bits_out) *bits_out = bits; return 1; }
        if (!alr_icd_gen_skip(&rd, sub)) return 0; }
    return 0;
}

#endif  // ALR_ICD_GEN_DEFINE
