// alr_jit_probe.hpp — V8-style ITERATIVE W^X JIT viability probe.
//
// Decides whether Chromium/V8 (and SwiftShader's JIT) can use executable
// memory the way they actually do — repeatedly rewriting and re-executing a
// code region — on THIS device's untrusted_app SELinux domain. The verdict
// answers exactly one product question: does Chromium need --jitless here?
//
// runtime_report.cpp::build_execmem_probe (~line 766) already proves the
// ONE-SHOT path: anon mmap(RW) -> memcpy an arm64 stub -> __clear_cache ->
// mprotect(RX) -> call -> expect 42. That stub is aarch64-ONLY:
//     movz w0,#42 ; ret  ==  {0x52800540u, 0xd65f03c0u}
// A real JIT never stops at one shot — it cycles the SAME region RW<->RX many
// times, holds many code pages RX at once, and patches some pages while
// executing others. This probe models that. We reuse build_execmem_probe's
// exact arm64 stub *style* (movz w0,#imm ; ret as little-endian instruction
// words) and additionally supply the equivalent stub for the three other ABIs
// so the translation unit COMPILES on all four; only the device's real ABI
// (arm64) ever executes.
//
// HEADER-ONLY. No JNI/registration here (wired separately).
#ifndef ALR_JIT_ALR_JIT_PROBE_HPP
#define ALR_JIT_ALR_JIT_PROBE_HPP

#include <sys/mman.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

namespace alr {
namespace jit {

// ---------------------------------------------------------------------------
// Per-ABI machine-code stub: a leaf function `int fn(void)` that returns `imm`.
//
// Encoded exactly as build_execmem_probe does it — raw bytes written into a
// page, then __builtin___clear_cache() before execution. `imm` is masked to
// 16 bits so every ABI can encode it as a single immediate-load instruction
// (V8's per-iteration sentinels here are 0x1000+i, well under 0xffff).
//
//   aarch64: movz w0,#imm  (0x52800000 | (imm<<5))         ; ret (0xd65f03c0)
//   arm32  : movw r0,#imm  (ARMv7 split-immediate encoding); bx lr (0xe12fff1e)
//   x86    : mov eax,imm32 (0xb8 imm32-LE)                  ; ret (0xc3)
//   x86_64 : mov eax,imm32 (0xb8 imm32-LE)                  ; ret (0xc3)
//
// Returns the number of bytes written into `dst` (0 if the ABI is unsupported,
// which cannot happen for the four Android ABIs).
inline std::size_t alr_emit_return_imm_stub(unsigned char* dst, unsigned int imm) {
    const std::uint32_t v16 = static_cast<std::uint32_t>(imm & 0xffffu);
#if defined(__aarch64__)
    // movz w0,#imm  +  ret   (two little-endian 32-bit instruction words)
    const std::uint32_t insns[2] = {
        0x52800000u | (v16 << 5),  // mirror of build_execmem_probe's 0x52800540 (imm=42)
        0xd65f03c0u,               // ret
    };
    std::memcpy(dst, insns, sizeof(insns));
    return sizeof(insns);
#elif defined(__arm__)
    // movw r0,#imm : imm split as imm4 (bits 15..12) | imm12 (bits 11..0).
    //   0xe3000000 | (imm4<<16) | imm12 ; then bx lr (0xe12fff1e).
    const std::uint32_t imm4 = (v16 >> 12) & 0xfu;
    const std::uint32_t imm12 = v16 & 0xfffu;
    const std::uint32_t insns[2] = {
        0xe3000000u | (imm4 << 16) | imm12,  // movw r0, #imm
        0xe12fff1eu,                         // bx lr
    };
    std::memcpy(dst, insns, sizeof(insns));
    return sizeof(insns);
#elif defined(__i386__) || defined(__x86_64__)
    // mov eax, imm32 (0xb8 + 4-byte little-endian) ; ret (0xc3).
    dst[0] = 0xb8u;
    dst[1] = static_cast<unsigned char>(v16 & 0xffu);
    dst[2] = static_cast<unsigned char>((v16 >> 8) & 0xffu);
    dst[3] = static_cast<unsigned char>((v16 >> 16) & 0xffu);
    dst[4] = static_cast<unsigned char>((v16 >> 24) & 0xffu);
    dst[5] = 0xc3u;  // ret
    return 6;
#else
    (void)dst;
    (void)v16;
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// The probe.
//
// FIRST output line is exactly "ALR JIT WX CYCLE: PASS" or "... FAIL".
// PASS  <=>  test 1 (iterative RW<->RX cycling) ran all K iterations with the
//            correct per-iteration return value  AND  test 4 (W^X granularity:
//            one page RX while a different page is written RW) passed.
// (tests 2 and 3 are reported for completeness but are NOT the gate.)
inline std::string run_jit_wx_cycle_probe() {
    std::ostringstream out;

    // Page size for alignment / mprotect granularity.
    long ps_l = ::sysconf(_SC_PAGESIZE);
    const std::size_t page = (ps_l > 0) ? static_cast<std::size_t>(ps_l) : 4096u;

    int anon_flags = MAP_PRIVATE;
#ifdef MAP_ANONYMOUS
    anon_flags |= MAP_ANONYMOUS;
#endif

    using ret_fn = int (*)();
    auto as_fn = [](void* p) -> ret_fn {
        ret_fn f = nullptr;
        std::memcpy(&f, &p, sizeof(f));
        return f;
    };
    auto clear = [](unsigned char* b, std::size_t n) {
        __builtin___clear_cache(reinterpret_cast<char*>(b),
                                reinterpret_cast<char*>(b) + n);
    };

    // ---- Test 1: iterative RW<->RX cycling on ONE region --------------------
    // mmap one 64 KiB RW region; K times, at a fresh page offset, write a stub
    // that returns 0x1000+i, clear-cache, mprotect(RX), call, check, then
    // mprotect(RW) again to "rewrite". This is precisely the JIT lifecycle:
    // write-code -> exec -> re-protect-to-write -> rewrite -> exec.
    constexpr int kCycles = 8;
    const std::size_t kRegion = 64u * 1024u;  // 64 KiB, page-multiple
    int cycles_ok = 0;
    std::string cycle_fail;  // first failing step + errno, empty if none

    void* region = ::mmap(nullptr, kRegion, PROT_READ | PROT_WRITE, anon_flags, -1, 0);
    if (region == MAP_FAILED) {
        cycle_fail = "mmap_rw errno=" + std::to_string(errno);
    } else {
        for (int i = 0; i < kCycles; ++i) {
            // Spread iterations across distinct pages (wrap if region < K pages).
            const std::size_t pages_in_region = kRegion / page;
            const std::size_t page_idx =
                pages_in_region ? (static_cast<std::size_t>(i) % pages_in_region) : 0u;
            unsigned char* pg =
                static_cast<unsigned char*>(region) + page_idx * page;
            // A small per-iteration offset within the page (kept aligned to 4
            // for arm) so successive writes land at "fresh" code addresses.
            unsigned char* slot = pg + (static_cast<std::size_t>(i) * 16u) % page;
            const unsigned int want = 0x1000u + static_cast<unsigned int>(i);

            const std::size_t n = alr_emit_return_imm_stub(slot, want);
            if (n == 0) {
                cycle_fail = "encode_unsupported_abi i=" + std::to_string(i);
                break;
            }
            clear(slot, n);
            // Re-protect to write was satisfied by the previous iteration's
            // trailing RW (or the initial RW map); now flip THIS page to RX.
            if (::mprotect(pg, page, PROT_READ | PROT_EXEC) != 0) {
                cycle_fail = "mprotect_rx i=" + std::to_string(i) +
                             " errno=" + std::to_string(errno);
                break;
            }
            const int got = as_fn(slot)();
            if (got != static_cast<int>(want)) {
                cycle_fail = "wrong_ret i=" + std::to_string(i) +
                             " got=" + std::to_string(got) +
                             " want=" + std::to_string(static_cast<int>(want));
                break;
            }
            // "rewrite": flip back to RW so the next iteration can patch code.
            if (::mprotect(pg, page, PROT_READ | PROT_WRITE) != 0) {
                cycle_fail = "mprotect_rw i=" + std::to_string(i) +
                             " errno=" + std::to_string(errno);
                break;
            }
            ++cycles_ok;
        }
        ::munmap(region, kRegion);
    }
    const bool cycle_pass = (cycles_ok == kCycles);

    // ---- Test 2: multiple concurrent RX pages -------------------------------
    // V8 keeps many code pages executable at once. Map N separate pages, make
    // each RX with a distinct return value, then call them all and verify.
    constexpr int kConc = 4;
    int concurrent_rx_ok = 0;
    std::string conc_fail;
    {
        void* pages[kConc];
        for (int i = 0; i < kConc; ++i) pages[i] = MAP_FAILED;
        bool armed = true;
        for (int i = 0; i < kConc && armed; ++i) {
            void* p = ::mmap(nullptr, page, PROT_READ | PROT_WRITE, anon_flags, -1, 0);
            if (p == MAP_FAILED) {
                conc_fail = "mmap i=" + std::to_string(i) +
                            " errno=" + std::to_string(errno);
                armed = false;
                break;
            }
            pages[i] = p;
            const unsigned int want = 0x2000u + static_cast<unsigned int>(i);
            const std::size_t n =
                alr_emit_return_imm_stub(static_cast<unsigned char*>(p), want);
            clear(static_cast<unsigned char*>(p), n);
            if (::mprotect(p, page, PROT_READ | PROT_EXEC) != 0) {
                conc_fail = "mprotect_rx i=" + std::to_string(i) +
                            " errno=" + std::to_string(errno);
                armed = false;
                break;
            }
        }
        if (armed) {
            // All pages are simultaneously RX; now invoke each.
            for (int i = 0; i < kConc; ++i) {
                const int want = static_cast<int>(0x2000u + static_cast<unsigned int>(i));
                if (as_fn(pages[i])() == want) {
                    ++concurrent_rx_ok;
                } else if (conc_fail.empty()) {
                    conc_fail = "wrong_ret i=" + std::to_string(i);
                }
            }
        }
        for (int i = 0; i < kConc; ++i) {
            if (pages[i] != MAP_FAILED) ::munmap(pages[i], page);
        }
    }

    // ---- Test 3: direct RWX mmap (modern V8 default since the 2023 revert) ---
    // Cheapest path if the domain allows it: map RWX, write+clear+call, no
    // mprotect at all. Recorded separately (success/errno); NOT a gate.
    bool rwx_mmap_ok = false;
    int rwx_errno = 0;
    {
        void* p = ::mmap(nullptr, page, PROT_READ | PROT_WRITE | PROT_EXEC,
                         anon_flags, -1, 0);
        if (p == MAP_FAILED) {
            rwx_errno = errno;
        } else {
            const unsigned int want = 0x3000u;
            const std::size_t n =
                alr_emit_return_imm_stub(static_cast<unsigned char*>(p), want);
            clear(static_cast<unsigned char*>(p), n);
            rwx_mmap_ok = (as_fn(p)() == static_cast<int>(want));
            if (!rwx_mmap_ok) rwx_errno = errno;
            ::munmap(p, page);
        }
    }

    // ---- Test 4: mprotect-RX-while-RW-elsewhere (W^X granularity) -----------
    // V8 patches some pages while executing others. Hold page A as RX and,
    // at the same time, WRITE into a separate RW page B. Pass iff the live RX
    // page still returns correctly AFTER the unrelated RW write.
    bool wx_granularity_ok = false;
    std::string wx_fail;
    {
        void* a = ::mmap(nullptr, page, PROT_READ | PROT_WRITE, anon_flags, -1, 0);
        void* b = ::mmap(nullptr, page, PROT_READ | PROT_WRITE, anon_flags, -1, 0);
        if (a == MAP_FAILED || b == MAP_FAILED) {
            wx_fail = "mmap errno=" + std::to_string(errno);
        } else {
            const unsigned int want = 0x4000u;
            const std::size_t n =
                alr_emit_return_imm_stub(static_cast<unsigned char*>(a), want);
            clear(static_cast<unsigned char*>(a), n);
            if (::mprotect(a, page, PROT_READ | PROT_EXEC) != 0) {
                wx_fail = "mprotect_rx errno=" + std::to_string(errno);
            } else {
                // B stays RW: write to it while A is executable.
                std::memset(b, 0xa5, page);
                volatile unsigned char* bp = static_cast<volatile unsigned char*>(b);
                const bool wrote = (bp[0] == 0xa5 && bp[page - 1] == 0xa5);
                const int got = as_fn(a)();  // A executes after B was written
                wx_granularity_ok = wrote && (got == static_cast<int>(want));
                if (!wx_granularity_ok) {
                    wx_fail = "post_write_ret got=" + std::to_string(got);
                }
            }
        }
        if (a != MAP_FAILED) ::munmap(a, page);
        if (b != MAP_FAILED) ::munmap(b, page);
    }

    // ---- Verdict ------------------------------------------------------------
    // The gate is test 1 (iterative cycling) AND test 4 (W^X granularity):
    // together they are exactly what a non-jitless V8 requires.
    const bool pass = cycle_pass && wx_granularity_ok;

    out << "ALR JIT WX CYCLE: " << (pass ? "PASS" : "FAIL");
    out << "\ncycles_ok=" << cycles_ok << "/" << kCycles;
    if (!cycle_fail.empty()) out << " (" << cycle_fail << ")";
    out << "\nconcurrent_rx_ok=" << concurrent_rx_ok << "/" << kConc;
    if (!conc_fail.empty()) out << " (" << conc_fail << ")";
    out << "\nrwx_mmap_ok=" << (rwx_mmap_ok ? "true" : "false")
        << " (errno " << rwx_errno << ")";
    out << "\nwx_granularity_ok=" << (wx_granularity_ok ? "true" : "false");
    if (!wx_fail.empty()) out << " (" << wx_fail << ")";
    out << "\npage_size=" << page;
    out << "\nVERDICT: "
        << (cycle_pass ? "V8 JIT viable without --jitless"
                       : "--jitless required");
    return out.str();
}

}  // namespace jit
}  // namespace alr

#endif  // ALR_JIT_ALR_JIT_PROBE_HPP
