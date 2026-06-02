# runtime_report §5 apt-drain 계약 — LD_PRELOAD chain + ALR_REEXEC_INPROC (제안 diff)

> **T2 산출물.** `runtime_report.cpp`(+ `alr_runtime/alr_exec.cpp`)를 **읽기만** 해서,
> `dpkg -i hello.deb`를 **in-process + fakeroot**로 돌리려면 로더가 무엇을 더 해야 하는지를
> 정확한 함수/라인 인용과 함께 **제안 diff**로 고정한다. 본체(loader/MainActivity)는 **미접촉** —
> 이 문서는 요구·diff 제안만 소유한다(소유: WS-1/메인 = 실제 코드 변경). version stamp 불변.
>
> 짝 호스트 모델: `tools/aptdrain_env_model.py` (+ `tests/test_aptdrain_env_model.py`, 30 passed).
> 상위 계약: `docs/design/v2-apt-pipeline-ssot.md` §5(R-V2-LAUNCH).
> baseline: `research/apt-launch` (main 4b3396e).

---

## 0. 한 줄

dpkg-drain이 in-process+fakeroot로 종단(`unpacked=true`)하려면 로더는 **세 군데**를 바꿔야 한다 —
**(A)** 첫 게스트 env에 fakeroot `.so`를 LD_PRELOAD **FIRST**로 체인 + `FAKEROOTUID/GID` 푸시,
**(B)** exec'd **child**의 envp 재주입 분류기(`decide_exec_envp_injection`)에 fakeroot `.so`를
**FIRST**로 추가(현재는 interpose `.so`만 안다), **(C)** `ALR_REEXEC_INPROC`를 dpkg-drain에서
**enable**(압축 해제기/maintainer-script child가 죽은 커널-execve 대신 re-map). 셋 다 **app-process
env 게이트 하나(`ALR_FAKEROOT`)** + **device 마커(`.alr-aptdrain`)**로만 켜져 일반 launch 완전 무회귀.

---

## 1. 읽기로 확인한 현재 동작 (정확한 위치)

### 1-a. 첫 게스트 env 구성 — interpose만, fakeroot 모름
`runtime_report.cpp`, `build_native_loader_probe()` 내부:

| 라인 | 현재 코드(사실) | 의미 |
|------|------------------|------|
| **L1593** | `guest_env.push_back("ALR_ROOTFS=" + config.rootfs_dir);` | 첫 게스트는 ALR_ROOTFS를 받음 ✓ |
| **L1617-1620** | `interpose_off = getenv("ALR_DISABLE_INTERPOSE")[0]=='1'` | host(app) env 게이트를 `::getenv`로 읽음 — **시그니처 안 바꾸는 깨끗한 훅** |
| **L1621-1624** | `pcgate_on = !(getenv("ALR_PCGATE")[0]=='0')` | 동일 패턴(host env 게이트) |
| **L1625-1635** | `if (!interpose_off) { guest_env.push_back("LD_PRELOAD=" + config.rootfs_dir + "/usr/lib/androlinux/libalr_interpose.so"); }` | **LD_PRELOAD에 interpose `.so` 하나만** 하드코딩. fakeroot 없음 → 비root dpkg가 `requires superuser`/`chown` EPERM으로 죽음 |
| **L1626-1632(주석)** | "LD_PRELOAD MUST be the ABSOLUTE ROOTFS HOST path" (R3) | 체인할 fakeroot 경로도 **절대 rootfs host 경로**여야 함(호스트 모델 `_abs()`가 강제) |

→ **결론:** 첫 게스트(=`dpkg` 자신)는 ALR_ROOTFS는 받지만 **fakeroot `.so`도 FAKEROOTUID/GID도 못
받는다.** §5-제안 diff A가 이걸 메운다.

### 1-b. exec'd **child** envp 재주입(B-3) — 역시 interpose만
dpkg는 압축 해제기(`zstd`)와 maintainer-script(`sh`)를 **fork+exec**한다(SSOT §6 단계 3·6). 그 child가
mediation에 재진입하려면 child의 envp에 우리 env가 들어가야 한다. 로더는 이미 그걸 한다:

| 위치 | 사실 |
|------|------|
| `runtime_report.cpp` **L2819-2836(주석)** | "child envp re-injection … If that envp lacks `LD_PRELOAD=<abs rootfs interpose .so>` and `ALR_ROOTFS`, the new image's interposer never loads and **dpkg→sh→dpkg-deb hit the bare Android fs (unpack fails)**" — 문제를 정확히 인식 |
| `runtime_report.cpp` **L2837-2873** | tracee 메모리에서 child의 `envp char**`를 읽음(NULL-terminated, kEnvMax=512) |
| `runtime_report.cpp` **L2874-2877** | `alr::runtime::decide_exec_envp_injection(config.rootfs_dir, env_entries)` 호출 — **이 분류기가 무엇을 넣을지 결정** |
| `runtime_report.cpp` **L2881-2949** | 결정대로 scratch 윈도(`sp-128KiB … sp-2048`)에 새 envp 문자열+포인터 배열을 빌드, x2/x3를 가리키게 함. **매 exec trap마다** 실행(체인 전체 영속, idempotent) |
| **인입 register** L2607-2613(inproc) / x2·x3(execve/at) | inproc re-map redirect도 **같은 B-3 블록을 통과**(L2606-2613이 x21=envp로 트램펄린에 넘김) → 분류기 하나가 커널-execve·inproc 양쪽의 **단일 chokepoint** |

분류기 본체(`alr_runtime/alr_exec.cpp` **L340-408**, 선언 `alr_exec.hpp` L191-193)의 현재 규칙
(주석 L175-190 + 코드):
- `out.interpose_so = rootfs_dir + "/usr/lib/androlinux/libalr_interpose.so";` (**L355** — fakeroot
  경로는 어디에도 없음)
- LD_PRELOAD 없으면 `add "LD_PRELOAD=<interpose_so>"`; 있는데 interpose 미포함이면
  `replace = "<interpose_so>:<old>"`(interpose FIRST, **L378-392**); ALR_ROOTFS 없으면 추가(L393-395)
- reason ∈ {`inject-both`,`inject-ld`,`inject-rootfs`,`prepend-ld`,`already`}

→ **결론(핵심):** B-3 재주입은 **interpose `.so`만** child에 넣는다. dpkg가 fork한 `zstd`/`sh`는
**fakeroot `.so`를 못 받아** child 안에서 `getuid()`가 비root로 보이고 `chown`이 EPERM → unpack/
configure 실패. **이 분류기에 fakeroot `.so`를 FIRST로 추가하는 것이 §5-제안 diff B다.**

### 1-c. ALR_REEXEC_INPROC — 존재하지만 default-OFF
| 위치 | 사실 |
|------|------|
| `runtime_report.cpp` **L2077-2080** | `inproc_reexec_on = getenv("ALR_REEXEC_INPROC")[0]=='1'` — **host(app) env 게이트, default-OFF** |
| **L2072-2076(주석)** | "PROVEN(v141 drain#19, R10 step1): trampoline IS reached in-guest with NO kernel execve … Now gated default-OFF … until STEP 2 replaces the probe body with the real map(ld.so+target)+jump" |
| **L2547-2582** | inproc redirect SCOPE: `/proc/self/exe`(loader 자기 bionic), non-rootfs, stub은 skip(L2554/2562/2574) — over-broad redirect가 onCreate 시퀀스를 wedge했던 device 교훈 |

→ **결론:** dpkg의 압축 해제기/maintainer-script child(rootfs glibc 타깃, skip 대상 아님)가 in-process로
re-map되려면 **이 게이트를 drain에서 켜야** 한다. STEP 2(real map+jump) + static-startup crash fix(PR#8)가
**전제**. drain은 이 두 전제 위에서만 종단 `unpacked=true`를 주장(SSOT §6 결론 2·3, §7).

### 1-d. JNI 시그니처는 고정 — env로만 켜야 한다
`runtime_report.cpp` **L6165-6184**: `Java_..._nativeAlrNativeLoaderProbe(env, thiz, package_name,
native_library_dir, app_files_dir, app_cache_dir, rootfs_name, program)` — **6 string 고정**, 추가 env
인자 없음. 따라서 fakeroot/inproc은 **반드시 app-process env(`Os.setenv`) → 로더 `::getenv`** 경로로만
켠다(L1617/L2077과 동일 관례). 시그니처 불변, version stamp 불변.

---

## 2. drain이 in-process+fakeroot로 돌기 위한 요구 (호스트 모델로 형식화)

`tools/aptdrain_env_model.py`가 아래 셋을 순수 함수로 고정하고 `tests/`(30 passed)로 검증한다.
로더 제안 diff는 정확히 이 모델의 결정을 구현해야 한다.

| 요구 | 호스트 모델 함수 | 불변식(테스트) |
|------|------------------|----------------|
| **R1. LD_PRELOAD chain** | `chain_ld_preload(rootfs, fakeroot, existing)` | fakeroot `.so` **FIRST** · interpose **KEPT**(드롭 금지) · 둘 다 **절대 rootfs host 경로**(R3) · guest preload 보존 · 우리 두 shim **de-dup**(idempotent) |
| **R2. drain env block** | `decide_drain_env(rootfs, fakeroot, inproc_reexec)` | guest env = `LD_PRELOAD`(R1) + `ALR_ROOTFS` + `FAKEROOTUID=0` + `FAKEROOTGID=0`; app gate env = `ALR_FAKEROOT=1`(+`ALR_REEXEC_INPROC=1`) — **app gate는 guest로 누출 금지** |
| **R3. marker→drain 결정** | `decide_drain_gate(program, marker, *_staged)` / `resolve(...)` | **default-OFF**: 마커(`.alr-aptdrain`) ∧ program∈{dpkg/apt…} ∧ 두 shim staged 전부여야 arm. 일반 app launch는 마커 있어도 `not-pkg-manager`로 미arm(무회귀) |

호스트 모델은 `build_fakeroot_overlay.device_cmd`(R1 체인 모양)와 `runtime_report.cpp` L1633의 interpose
경로에 **drift 가드**로 묶여 있다(`test_chain_matches_build_fakeroot_overlay_device_cmd`,
`test_interpose_rel_matches_loader_hardcoded_path`).

---

## 3. 제안 diff (본체 미접촉 — WS-1/메인이 적용)

> 시그니처·version stamp 불변. 게이트 미설정(`ALR_FAKEROOT` unset) 시 **모든 hunk가 no-op** → 일반
> launch·기존 chromium/GUI drain 완전 무회귀.

### 3-A. `runtime_report.cpp` — 첫 게스트 env에 fakeroot 체인 (L1617-1636 부근)

`interpose_off`/`pcgate_on`을 읽는 바로 그 자리(L1617-1624)에 게이트 하나를 더 읽고, LD_PRELOAD
push(L1633-1634)를 fakeroot-aware하게:

```cpp
// (L1624 직후, interpose_off/pcgate_on 옆 — 같은 host-env 게이트 패턴)
const bool fakeroot_on = []{
    const char* f = ::getenv("ALR_FAKEROOT");      // app-process가 drain 직전 setenv
    return f != nullptr && f[0] == '1';
}();

// (L1625-1635 교체) interpose는 유지, fakeroot면 FIRST로 prepend
if (!interpose_off) {
    std::string preload =
        config.rootfs_dir + "/usr/lib/androlinux/libalr_interpose.so";
    if (fakeroot_on) {                              // fakeroot FIRST (credential 외곽)
        preload = config.rootfs_dir +
                  "/usr/lib/androlinux/libalr_fakeroot.so:" + preload;
    }
    guest_env.push_back("LD_PRELOAD=" + preload);
}
if (fakeroot_on) {                                  // fakeroot identity 계약
    guest_env.push_back("FAKEROOTUID=0");
    guest_env.push_back("FAKEROOTGID=0");
}
```

호스트 모델 대응: `chain_ld_preload(rootfs, fakeroot=fakeroot_on)` == 위 `preload`. 일반 launch
(`fakeroot_on==false`)는 **기존 L1633-1634와 바이트 동일** → 무회귀.

### 3-B. `alr_runtime/alr_exec.cpp` `decide_exec_envp_injection` — child envp에 fakeroot FIRST (L340-408)

이게 **핵심 hunk.** child(`zstd`/`sh`/`dpkg-deb`)가 fakeroot를 물려받게 한다. 분류기는 fs 접근이
없는 순수 함수이므로 fakeroot 여부를 **인자**로 받아야 한다(현재 `(rootfs_dir, env_entries)` →
`(rootfs_dir, env_entries, fakeroot)`; 기본값 `false`로 모든 기존 호출 무회귀).

```cpp
// alr_exec.hpp L191-193 선언 확장 (기본 인자로 기존 호출 무회귀)
ExecEnvpInjection decide_exec_envp_injection(
    std::string_view rootfs_dir,
    const std::vector<std::string>& env_entries,
    bool fakeroot = false);                         // ← 추가

// alr_exec.cpp L355 부근: 체인할 .so 목록을 fakeroot-aware하게
const std::string interpose_so =
    std::string(rootfs_dir) + "/usr/lib/androlinux/libalr_interpose.so";
const std::string fakeroot_so =
    std::string(rootfs_dir) + "/usr/lib/androlinux/libalr_fakeroot.so";
// 원하는 chain (fakeroot FIRST, interpose KEPT):
const std::string desired = fakeroot
    ? (fakeroot_so + ":" + interpose_so)
    : interpose_so;
// L366-392의 "interpose_so 포함?" 판정을 "desired의 모든 항목 포함?"으로 확장:
//   - LD_PRELOAD 없음            → add "LD_PRELOAD=<desired>"
//   - 있는데 desired 미충족       → replace = "<desired>:<guest의 desired 제외 나머지>"
//                                  (fakeroot FIRST, interpose 다음, guest preload 보존, de-dup)
//   - 이미 충족(둘 다 포함)        → leave (idempotent — 부모가 이미 주입한 child)
// ALR_ROOTFS 분기(L393-395)는 불변.
```

호스트 모델 대응: `chain_ld_preload(rootfs, fakeroot=True, existing=<guest LD_PRELOAD>)`가 정확히
이 `replace`/`add` 값을 만든다(FIRST·KEEP·de-dup·preserve 전부 테스트됨). reason은 fakeroot 경로용으로
`prepend-fakeroot` 같은 값을 추가하면 device 로그에서 구분 가능(선택).

**호출부(`runtime_report.cpp` L2876)**: `decide_exec_envp_injection(config.rootfs_dir, env_entries,
/*fakeroot=*/fakeroot_on)`. `fakeroot_on`(3-A에서 함수 스코프로 이미 계산됨)을 3번째 인자로 전달.
나머지 B-3 빌드 로직(L2881-2949)은 `inj.add_entries`/`inj.replace_ld_preload`/`inj.ld_preload_value`만
소비하므로 **무변경**.

### 3-C. `ALR_REEXEC_INPROC` enable — drain에서만 (app-side, MainActivity 제안)

`inproc_reexec_on`(L2077-2080) 게이트 자체는 그대로 둔다. **app-process가 drain 직전 켜고 끈다**
(SSOT §5-제안 diff B와 합치):

```kotlin
// MainActivity launchPackageManagerProbes(), .alr-aptdrain 마커 게이트 안:
android.system.Os.setenv("ALR_FAKEROOT", "1", true)       // 3-A/3-B 켬
android.system.Os.setenv("ALR_REEXEC_INPROC", "1", true)  // child re-map 켬 (PR#8 전제)
// … nativeAlrNativeLoaderProbe(… "/usr/bin/dpkg\n--force-not-root\n-i\n…hello.deb") …
android.system.Os.unsetenv("ALR_REEXEC_INPROC")           // 무회귀
android.system.Os.unsetenv("ALR_FAKEROOT")
```

호스트 모델 대응: `decide_drain_env(rootfs, fakeroot=True, inproc_reexec=True).app_env ==
{ALR_FAKEROOT:1, ALR_REEXEC_INPROC:1}`; `resolve(...)`가 `.alr-aptdrain` 마커 + dpkg program +
두 shim staged의 AND로만 arm(default-OFF).

---

## 4. 전제·한계 (정직)

- **3-C는 STEP 2 + static-startup fix(PR#8) 전제.** `ALR_REEXEC_INPROC`를 켜도 inproc 트램펄린
  본체가 아직 marker-print+exit이면(L2072-2076) child가 exit 123으로 죽는다. 즉 `unpacked=true`
  종단은 **G1(메인 영역) 선결** — 이 문서는 env 배선만 소유(SSOT §6 결론).
- **3-A/3-B만으로도 device 중간 하위게이트는 가능.** fakeroot 체인이 들어가면(inproc 없이도) dpkg 자기
  프로세스의 `getuid→0`/`chown` no-op/`stat root:root`는 통과(SSOT §7 중간 하위게이트). extract/
  configure child만 3-C+G1을 기다린다.
- **`mknod` 미interpose**는 유일 hard wall(`hello`은 device 노드 없음 → 무영향, SSOT §8).
- **host=Darwin** → 이 문서/모델은 **배선 정합**까지만 증명. `unpacked=true`는 device-only
  (DEVICE-REQ `ALR-V2-apt-unpack`).

---

## 5. 검증 (host)

```sh
uvx pytest tests/test_aptdrain_env_model.py -q     # 30 passed
python3 -m tools.aptdrain_env_model --rootfs /data/x/rootfs   # armed device block
python3 -m tools.aptdrain_env_model --no-marker --rootfs /data/x/rootfs  # INACTIVE: marker-absent
```

`test_chain_matches_build_fakeroot_overlay_device_cmd` / `test_interpose_rel_matches_loader_hardcoded_path`가
모델을 `build_fakeroot_overlay.device_cmd` + `runtime_report.cpp` L1633에 묶어, 본체가 경로/순서를
바꾸면 host test가 깨져 drift를 잡는다.

---

## 6. 교차참조

- 상위 계약·게이트 보드: `docs/design/v2-apt-pipeline-ssot.md` (§5 R-V2-LAUNCH, §6 정직 분해, §7 DEVICE-REQ).
- fakeroot 체인 host 소스: `tools/build_fakeroot_overlay.py` `device_cmd()`, `tools/fakeroot/README.md`.
- 읽은 본체(미접촉): `app/src/main/cpp/runtime_report.cpp`(L1593·L1617-1636·L2077-2080·L2547-2582·
  L2819-2949·L6165-6184), `app/src/main/cpp/alr_runtime/alr_exec.cpp`(L340-408) /
  `alr_exec.hpp`(L191-193).
- 짝 모델/테스트(이 세션 소유): `tools/aptdrain_env_model.py`, `tests/test_aptdrain_env_model.py`.
