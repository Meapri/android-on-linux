> 격리 브랜치 research/static-reentry 진단 산출(보고/연구 담당). 본체 alr_inproc_reexec.c 읽기만 — fix는 본체(WS-1) 구현. main·ws-N 미변경. v2 apt + GIMP plugin + chromium 멀티프로세스 공통 잠금(CR-5).

# ADR — static-reentry guest startup crash: 근본원인 + fix 설계

## 1) 한 줄 결론 (best 근본원인 가설)

**트램폴린 `alr_inproc_reexec_worker`는 device-proven 작동 로더(runtime_report.cpp, v73에서 static `/bin/hello`를 exit(0)까지 실행)가 in-process 글리브c 공존을 위해 갖춘 4개의 검증된 준비 단계 중 *3개를 빠뜨린 채* map+jump한다.** 단일 1순위는 단정 불가이고, **공동 1순위 = (A) `enter_guest`의 `msr tpidr_el0, xzr` (TCB 없는 TP=NULL) + (B) `#define R_AARCH64_IRELATIVE 1027` 상수 오류(실제 RELATIVE 값)**. 이 둘은 작동 로더에 이미 존재하는 패턴의 단순 이식으로 동시 수정 가능하며, 디바이스 드레인의 `si_code`가 둘을 분리한다.

## 2) 정확한 crash 지점·원인 (코드 + glibc startup 대조)

### 결정적 대조표 — 작동 로더(v73 PASS) vs 트램폴린(crash)

| 준비 단계 | 작동 로더 runtime_report.cpp | 트램폴린 alr_inproc_reexec.c | 영향 |
|---|---|---|---|
| **TP/TCB** | L1888: `mmap(16384)`, TP=중앙+8192; L1166 `msr tpidr_el0, x2` | L514 `msr tpidr_el0, xzr` (TP=**NULL**) | **공동 1순위 — SEGV** |
| **IRELATIVE 상수** | L36 `1032` (정본) | L128 `1027` (= 실제 RELATIVE) | **공동 1순위 — ILL** |
| **SIG_DFL/libsigchain reset** | L1782-1797: raw `rt_sigaction`로 1..64 SIG_DFL (libsigchain 우회) | **없음** | crash 마스킹 + 2차 SEGV |
| **SHT_RELA fallback** | L1411-1434 (PT_DYNAMIC 부재 시) | **없음** (DT_RELA만) | static-PIE/DT_JMPREL 타깃만 |

### glibc aarch64 static startup 순서 (libc-start.c)
`_dl_aux_init` → `__tunables_init` → `_dl_relocate_static_pie`(`#if ENABLE_STATIC_PIE` — **비-PIE ET_EXEC는 컴파일아웃**) → `ARCH_SETUP_IREL`(`apply_irel(__rela_iplt_start, __rela_iplt_end)` — ET_EXEC가 **자기 ifunc를 self-resolve**) → `ARCH_SETUP_TLS`(`__libc_setup_tls` → `TLS_INIT_TP`로 비로소 TP 재설치).

**(A) TP=NULL** — `TLS_INIT_TP` *이전* 구간(`_dl_aux_init`/`__tunables_init`)은 stack-protector prologue가 `THREAD_SELF`(=`TPIDR_EL0`)에서 canary를 `ldr xN,[TP,#off]`로 읽는다. TP=0 → NULL-근처 load → **SIGSEGV(SEGV_MAPERR)**. v62/v63/v64 device evidence가 정확히 이 모습: signal 11, alt-stack handler 미발화(= 초기 glibc TLS/TCB 상태 손상). v73이 이걸 16384B TCB로 고쳐 PASS.

**(B) IRELATIVE=1027** — ARM ABI 정본: **R_AARCH64_RELATIVE=1027, R_AARCH64_IRELATIVE=1032** (둘 다 1024부터 시작하는 dynamic reloc 범위; 웹 확인). 트램폴린 L467은 `1027==RELATIVE`를 ifunc로 오인 → L469 `(Resolver)(base+addend)` = **데이터 주소를 함수로 호출** → 비명령 실행 → **SIGILL(ILL_ILLOPC)**. 동시에 실제 1032(IRELATIVE)는 스킵 → 미해결 슬롯. **단 ET_EXEC `/bin/sh`/`/bin/hello`는 PT_DYNAMIC 없음**(round-11 diag `IREL=` 직후 `bss_zeroed`, 즉 count=0) → 이 경로가 **마스킹**되어 (A)만 노출. static-PIE(ET_DYN, DT_RELA 보유) 타깃에서 (B)가 활성.

### 신호 정합성 (정직)
round-10 foot 드레인 = **SIGILL(signal 4)**. round-11이 'SIGILL fixed'로 본 건 boundary-clobber 수정 후이며, 그 diag는 `jumping entry=0x400640`에서 끝난다 — **점프 이후 untraced 실행이라 startup 통과를 증명하지 못함**. L327 주석도 SIGILL을 boundary-clobber 탓으로 본다. 즉 현재 상태에서 (A)[SEGV]와 (B)[ILL] 중 어느 게 *지금* 터지는지는 ET_EXEC vs ET_DYN 타깃과 마스킹에 따라 갈리며, **device si_code로만 확정**된다.

### 강등된 후보 (R12 목록 교정)
- **PT_GNU_RELRO(0x6474e552) RO-mprotect 없음 → crash 원인 아님.** 비-PIE는 `_dl_relocate_static_pie`/`_dl_protect_relro` 미호출. 하드닝일 뿐.
- **auxv → 1차 용의자 아님.** L697-713이 PHDR/ENTRY/BASE/RANDOM/HWCAP 등 완비. AT_RANDOM=0이어도 `_dl_setup_stack_chk_guard` NULL fallback.
- **brk (S2 가설) → 가능하나 미확정 차순위.** `__libc_setup_tls`의 `_dl_early_allocate`가 fresh-BSS `__curbrk==NULL`에서 `__brk_call`로 break를 in-process 이동, bionic heap과 경합. 단 glibc 2.36+는 brk 실패 시 mmap fallback이 있어 *즉사*는 약함. v73이 set_robust_list 1개만 supervisor로 서비스하고 PASS한 점은 brk가 치명적이지 않았음을 시사. **모니터 대상**이되 (A)/(B)보다 후순위.

## 3) fix 설계 (본체 alr_inproc_reexec.c 소유 — 미접촉 diff 제안)

본체가 적용. 나는 진단/설계만. 모두 **작동 로더에 이미 있는 검증 패턴의 이식**이라 리스크 최소.

**FIX-1 (공동1순위, IRELATIVE 상수) — 1줄.** L128
```
-#define R_AARCH64_IRELATIVE 1027
+#define R_AARCH64_IRELATIVE 1032   // ARM ABI: 1027=RELATIVE, 1032=IRELATIVE (runtime_report.cpp:36)
```
(`alr_reentry.c:115`도 동일 교정.)

**FIX-2 (공동1순위, zeroed TCB) — enter_guest 시그니처에 tcb 추가.** L512-519 → `xzr` 금지, 호출자가 mmap한 zeroed-TCB 중앙을 x2로:
```c
__attribute__((noreturn)) static void enter_guest(void* sp, void* entry, void* tcb) {
    __asm__ volatile(
        "msr tpidr_el0, %2\n"   // clean zeroed TCB (NOT xzr): canary/THREAD_SELF reads land in mapped-zero
        "mov sp, %0\n" "mov x0, #0\n" "br %1\n"
        :: "r"(sp), "r"(entry), "r"(tcb) : "memory");
    __builtin_unreachable();
}
```
호출 직전(L745 부근): `void* tcbr = (void*)sys6_(SYS_mmap, 0, 16384, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);` → `enter_guest((void*)start, (void*)jump_entry, (char*)tcbr + 8192);` (MAP_FAILED 시 0 fallback).

**FIX-3 (crash 가시화 + 2차 SEGV 차단) — SIG_DFL/libsigchain reset.** `enter_guest` 직전에 raw `rt_sigaction` 1..64 SIG_DFL + `rt_sigprocmask` unblock 이식 (runtime_report.cpp:1782-1799 그대로, freestanding `sys*`로). **이게 없으면 (A)/(B)가 터져도 ART libsigchain → bionic `pthread_getspecific`(TP+0x30) 2차 SEGV로 마스킹되어 fault PC를 못 읽는다** — v73 발견. 진단을 위해서도 필수.

**FIX-4 (2순위, static-PIE 대비) — SHT_RELA fallback + RELATIVE/DT_JMPREL.** map_elf_image의 reloc 루프(L450-480)에 PT_DYNAMIC 부재 시 SHT_RELA fallback(runtime_report.cpp:1411-1434) 이식 + DT_JMPREL/DT_PLTRELSZ 처리. **단 ET_EXEC IRELATIVE 더블-어플라이 주의**(S2 지적): ET_EXEC는 glibc `apply_irel`가 self-resolve하므로, 매퍼 IRELATIVE 적용을 **ld.so/static-PIE에만 게이트**하는 게 안전(ET_EXEC엔 적용 안 해도 glibc가 함). `dash -c echo OK`(동적)는 FIX-1·2·3로 통과하므로 FIX-4는 dpkg unpack 단계 전 별도 검증.

## 4) device 게이트 + DEVICE-REQ

device 없음(나) → host 모델까지만 완료. 본체 적용 후 device 1회 드레인 필수.

- **DEVICE-REQ-1 (분리 진단):** FIX-1·2·3 적용, `ALR_REEXEC_INPROC=1`로 re-mapped **`dash -c 'echo OK'`**(동적, ld.so 경유) 구동. 기대: (i) stdout `OK` + clean exit. crash 시 PTRACE로 `si_code` 캡처 — `SEGV_MAPERR`+fault addr≈small-near-0 → (A) TP 가설 / `ILL_ILLOPC`+fault PC가 데이터·0 영역 → (B) reloc 가설. fault PC를 `/proc/self/maps`의 트램폴린 execmem 범위와 대조.
- **DEVICE-REQ-2 (static 회귀):** re-mapped static `/bin/hello`(ET_EXEC) → `hello from static arm64 rootfs` + exit 0 (v73 작동 로더와 동일 결과여야 — 동일 패턴 이식이므로).
- **DEVICE-REQ-3 (목표 unlock):** dpkg unpack 경로(static-PIE `ldconfig` 포함 가능성) 1회. brk 거동 모니터(점프 후 첫 brk syscall) → S2 brk 가설 활성 여부 확정.

## 5) 정직 — device 전 미확정 + 대안 가설

- **단일 1순위 단정 불가.** 신호 증거(SIGILL)가 (B)와, 코드+v62 evidence(SIGSEGV)가 (A)와 정합 — **device si_code 없이는 분리 불가**. 그래서 FIX-1·2·3 **동시** 적용 후 1드레인이 최선(둘 다 작동 로더 검증 패턴, 회귀 리스크 낮음).
- **round-11 'SIGILL fixed'는 startup 통과 증명이 아님** (점프 후 untraced). 현재 crash가 정확히 어디인지는 미관측.
- **대안 가설 (차순위, 배제 못 함):** brk-unset SEGV in `__libc_setup_tls`(S2/BZ2066147) — glibc 2.36+ mmap fallback이 약화하나 bionic heap 경합 가능; FIX 적용 후에도 잔존하면 ld.so 경로 선호 또는 guest arena above-break가 대안. ET_EXEC IRELATIVE 더블-어플라이(S2) — FIX-4를 ld.so-게이트로 회피.
- **`git add/commit 금지` 준수**, 코드 미접촉(읽기만), 본체가 구현.

## 핵심 파일 경로 (절대)
- 본체 소유, 수정 대상: `/Users/naen/Documents/alr-static-reentry/app/src/main/cpp/alr_inproc_reexec.c` (L128 IRELATIVE, L450-480 reloc, L512-519 enter_guest, L745-746 호출부, L697-713 auxv)
- 작동 로더 대조 기준: `/Users/naen/Documents/alr-static-reentry/app/src/main/cpp/runtime_report.cpp` (L36 IRELATIVE=1032, L1160-1169 alr_enter_guest, L1411-1434 SHT_RELA fallback, L1782-1799 SIG_DFL reset, L1886-1889 16384B TCB)
- 미러: `/Users/naen/Documents/alr-static-reentry/app/src/main/cpp/alr_reentry/alr_reentry.c` (L115 동일 IRELATIVE 오류)
- 근거 evidence: `/Users/naen/Documents/alr-static-reentry/docs/evidence/2026-05-31-device-SM-X236N-v73-glibc-static-runs.md` (static `/bin/hello` PASS, TCB+set_robust_list+SIG_DFL), `.../2026-05-31-device-SM-X236N-v62-native-loader.md` (TP 공존 미처리 시 SIGSEGV+handler 미발화), `.../2026-06-02-round11-remap-sigill-fixed.md` (boundary-clobber 수정, 점프까지만)

Sources:
- [ELF for the Arm 64-bit Architecture (AArch64) — ARM-software/abi-aa](https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst)
- [Add AARCH64 relocation constants to elf/elf.h — sourceware libc-alpha](https://sourceware.org/legacy-ml/libc-alpha/2013-06/msg01069.html)
- [Relative relocations and RELR — MaskRay](https://maskray.me/blog/2021-10-31-relative-relocations-and-relr)