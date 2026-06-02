# v2 범용 앱 설치(apt/dpkg) 파이프라인 — SSOT

> ALR이 "임의 Debian/Ubuntu arm64 앱을 device에서 실제로 **설치**(`apt install` / `dpkg -i`)
> 한다"를 비root·public-API·W^X·in-process 계약 안에서 끝내기 위한 **단일 진실 소스**.
> 게이트 전체 + 각 게이트의 host/device 상태 + 본체(MainActivity/runtime_report.cpp)에
> 요구하는 §5 launch 계약 + device 증명 요구(DEVICE-REQ)를 한 곳에 고정한다.
>
> 담당 분담(메모리 `fakeroot-research-ownership`): **메인 세션 = chromium 엔진 + exec-re-entry
> in-process re-map**. **이 문서(연구/제품) = v2 apt 파이프라인 + fakeroot + staging**.
> 이 문서는 host 산출물(builder/shim/staging tar) + 본체에 대한 **요구사항/제안 diff**만
> 소유한다 — 본체 파일(`MainActivity.kt`/`runtime_report.cpp`/`alr_inproc_reexec.c`/
> `libalr_interpose.c`)은 **읽기만** 하고 직접 수정하지 않는다.
>
> 작성 baseline: `main` 450b6d8 (v150). 호스트=Darwin(=aarch64 glibc .so/dpkg 실행 불가 →
> host 증명 천장 = **staging + 메커니즘**까지; "unpack 풀림"은 device에서만 주장 가능).

---

## 1. 한 줄 결론

v2 범용 설치는 **세 개의 직교 게이트**의 곱이다 — (G1) exec-re-entry in-process re-map[메인],
(G2) non-root unpack을 위한 fakeroot[host-ready], (G3) apt+dpkg 런타임 staging[host-ready].
**G2/G3는 host-side로 완성**(builder + shim + staging tar 빌드·검증 끝)이고, **남은 단일 선결은
G1**이다. 단 정직한 분해(§6)에 따르면 **`dpkg -i hello.deb`의 "파일 풀기(extract)"는 G2만으로
거의 끝나지만, dpkg가 압축 해제기(`zstd`/`xz`)와 maintainer-script(`sh`)를 fork+exec하는 지점에서
G1을 건드린다** — 즉 "unpack만은 single-process라 G1 독립"이 아니라 **"chown/chmod/stat 메타
연산은 G2 단독으로 device 가능하지만, 종단 `unpacked=true`는 최소 1회의 exec-re-entry(압축
해제기 child)를 통과해야 한다."** 따라서 device drain 순서는 **G1이 풀리는 순간 G2+G3는 이미
staged → 즉시 `dpkg -i hello.deb` 종단 가능**이며, 그 한 줄을 위해 본체가 해야 할 일이 §5 계약이다.

---

## 2. 게이트 보드 (상태 한눈에)

| 게이트 | 무엇 | 소유 | 상태 | 증명 천장 |
|--------|------|------|------|-----------|
| **G1 exec-re-entry re-map** | 게스트 `dpkg`가 fork+exec한 child(`zstd`/`sh`/`dpkg-deb`)를 커널-execve 실패(게스트 ld.so resolve 불가) 대신 loader가 **재-map+jump** + interposer/`ALR_ROOTFS` 재주입 | **메인 세션** | **WALL** — B-1(execve x0 path-rewrite) device-fires(v138), B-3(child envp 재주입) round-7 진행, **static-startup crash** 잔존 | device |
| **G2 fakeroot (non-root unpack)** | 비root(uid 10xxx)가 dpkg의 `requires superuser` 게이트 + `chown/chmod` EPERM을 통과: `getuid→0`, `chown` no-op+메타DB, `stat` 오버레이 | **이 세션(host)** | **HOST-READY** — `libalr_fakeroot.c` 23/23 심볼, `fakeroot-stage.tar` 빌드·검증(아래 §4); device staging+drain pending | host(staging+메커니즘) |
| **G3 apt+dpkg staging** | noble `apt`/`dpkg` 런타임 closure + dpkg admindir scaffold + 테스트 `hello.deb`를 rootfs에 §5-E 오버레이로 적재 | **이 세션(host)** | **HOST-READY** — `build_apt_dpkg_overlay.py` closure 13–58pkg 해소, admindir scaffold, `--fetch-test-deb`; device staging pending | host(staging) |

곱(AND) 의미: `unpacked=true` ⟺ G1 ∧ G2 ∧ G3. G2·G3는 이미 1(host-ready·staging만 남음),
G1만 0. **하드 제약**(불변): 비root, public Android API only, W^X-safe(exec-mem/seccomp/ptrace
**없는** 순수 libc interposition — `libalr_fakeroot.c` 헤더·selftest가 강제), SELinux 미접촉,
in-process, device evidence 없이 완료 주장 금지, version stamp 불변.

---

## 3. host 산출물 인벤토리 (이 세션 소유, 전부 host)

| 산출물 | 경로 | 역할 | 검증 |
|--------|------|------|------|
| fakeroot shim 소스 | `tools/fakeroot/libalr_fakeroot.c` | non-root uid=0 + chown/chmod/stat 오버레이, `dlsym(RTLD_NEXT)`로 interposer와 **체인** | `tests/test_build_fakeroot_overlay.py` |
| fakeroot 빌더 | `tools/build_fakeroot_overlay.py` | `zig cc --target=aarch64-linux-gnu.2.36`로 `.so` 컴파일 → §5-E `fakeroot-stage.tar`; `--out`/`--device-cmd`/`--list`/`--selftest` | `--selftest` ALL PASS |
| apt+dpkg 빌더 | `tools/build_apt_dpkg_overlay.py` | noble main+universe closure(`deb_closure`) base-subtract + admindir scaffold + `--self-contained` 프런트엔드 + `--fetch-test-deb hello` → `apt-dpkg-stage.tar` | `tests/test_build_apt_dpkg_overlay.py` |
| closure resolver | `tools/deb_closure.py` | SONAME base-subtract, overlay_guard downgrade-safe | 기존 |
| **한방 staging 래퍼** | `tools/build_v2_stage.sh` | 두 stage tar + `hello.deb` + §5 device-cmd를 한 번에 생성·검증(`--out`/`--base`/`--rootfs`/`--fakeroot-only`) | host 실행 검증 |
| **staging 종단 계약 테스트** | `tests/test_v2_staging.py` | 두 stage tar의 §5-E 규약(./-rooted/flat SONAME/안전심링크) + admindir + 체인 device-cmd + (zig-gated)실 aarch64 ELF + (`ALR_V2_STAGING_NET=1`)live closure/build/fetch | 39 passed offline / +3 net PASS |
| **non-root unpack 메커니즘 증명** | `tools/dpkg_unpack_model.py`, `tests/test_dpkg_unpack_fakeroot.py` | fakeroot 하 `dpkg --unpack` 종단을 host로 증명(비root wall → fakeroot 적용 → unpacked + 0-mismatch) | selftest ALL PASS / 18 passed |
| fakeroot README | `tools/fakeroot/README.md` | 체인 LD_PRELOAD 계약 + device 호출 라인 | — |

빌드 명령(host):
```sh
# G2 fakeroot stage (오프라인, zig만 필요)
python3 -m tools.build_fakeroot_overlay --out /tmp/fakeroot-stage.tar \
    --rootfs <ROOTFS> --json
# G3 apt+dpkg stage (네트워크 — noble ports.ubuntu.com)
python3 -m tools.build_apt_dpkg_overlay --out /tmp/apt-dpkg-stage.tar \
    --base <base-rootfs.tar|dir> --self-contained --json
# 테스트 .deb (device dpkg -i 타깃; 오버레이에 안 들어가는 별도 자산)
python3 -m tools.build_apt_dpkg_overlay --fetch-test-deb /tmp/aptdrain/
```

---

## 4. staging 빌드 — 실제 증명 (host)

### 4.1 fakeroot-stage.tar (G2) — 실 빌드 검증됨
host(Darwin, zig 0.16.0)에서 실제 빌드:

```
member : ./usr/lib/androlinux/libalr_fakeroot.so  (0o755, 73456 bytes)
sha256 : d6c4bd0bbc359116815860952b1bc6208354cbae7672959963b45a3827653d8e
ELF    : ELF 64-bit LSB shared object, ARM aarch64, version 1 (SYSV)  ← EM_AARCH64=183
exports: chown / getuid / stat / statx … (nm -D: global T)  ← 실제로 interpose 가능
symbols: 23/23 REQUIRED_SYMBOLS (credentials/setters/chown/chmod/stat-family)
```

`--selftest` = **ALL PASS** (소스가 23개 심볼 전부 정의, W^X 무위반, `dlsym(RTLD_NEXT)` 체인,
`FAKEROOTUID/GID` 계약, §5-E stage_tar_spec 적합, device-cmd가 fakeroot **FIRST**로 체인).
pytest `test_build_fakeroot_overlay.py` 통과.

### 4.2 apt-dpkg-stage.tar (G3) — 실 네트워크 빌드 검증됨(2026-06-02)
`--selftest` = **ALL PASS**. closure resolver(synthetic noble index)가 `apt`/`dpkg`/`apt-utils`/
`libapt-pkg`/`gpgv`/`libzstd1`/`tar` 전이 의존까지 해소, unsatisfied=∅. admindir scaffold
(`var/lib/dpkg/{,info,updates,triggers,alternatives}` + `status`/`available`/`arch=arm64`)와
`--self-contained` 프런트엔드(`dpkg dpkg-deb dpkg-split dpkg-query apt apt-get tar …`), zstd
`.deb` 압축·절대심볼릭링크(`./etc/rmt`) 내성까지 selftest로 증명.

**실제 네트워크 빌드(host, ports.ubuntu.com noble) 실행·검증됨:**
```
closure : 58 packages, 18.7 MiB compressed (unsatisfied = NONE)
overlay : 589 files, base 125 SONAME-subtracted, violations = 0
admindir: 10 members (status/available/arch=arm64 + triggers)
self-contained: 14 front-ends — usr/bin/{dpkg,dpkg-deb,apt,apt-get,tar,…} 모두 aarch64 ELF(EM=183, 0755)
§5-E    : stage_tar_spec --base tiny-rootfs → 0 error / 0 warning = CONFORMANT
hello.deb: hello_2.10-3build1_arm64.deb (25184 B, ar archive, data.tar.zst)
```
zstd `.deb` 추출은 host의 `zstd` CLI 경유(`build_stage_tar.extract_deb`)로 실동작. 유일한
`unsupported`는 `tar` 패키지의 merged-root 추출이 절대-심링크(`./etc/rmt`)로 스킵되는 것뿐인데,
`--self-contained`가 `usr/bin/tar` 바이너리를 직접 주입하므로 device는 동작하는 `tar`를 받는다
(base가 `tar`를 이미 가지면 무영향). 한방 래퍼 `tools/build_v2_stage.sh`가 두 stage + `hello.deb`
+ §5 device-cmd를 한 번에 생성·검증한다.

### 4.3 host 증명 천장 (정직)
호스트=**Darwin**이라 aarch64 glibc `.so`도 `dpkg`도 실행 불가. 따라서 host에서 증명 가능한 최대치는
**(a) 두 stage tar의 빌드·구조·심볼·체인 정합** + **(b) fakeroot 메커니즘이 dpkg가 두드리는 정확한
libc 진입점(credentials/chown/chmod/stat-family)을 전부 interpose**한다는 소스·심볼 사실까지다.
"`dpkg -i hello.deb` → `unpacked=true`"는 **device에서만** 주장한다(§7 DEVICE-REQ).

---

## 5. 본체에 요구하는 §5 launch 계약 (요구사항 + 제안 diff — 본체 미접촉)

> 이 절은 **요구**다. 실제 코드 변경은 본체(MainActivity/runtime_report) 소유 세션이 한다.
> 아래 diff는 "이렇게 하면 충돌 없이 v2 drain이 켜진다"는 **제안**이며, 이 세션은 적용하지 않는다.

### 5-아키텍처 사실 (읽기로 확인)
- `MainActivity.launchPackageManagerProbes()`(L1754)가 이미 `dpkg -i <localDeb>`를
  `nativeAlrNativeLoaderProbe(...)`로 실행하고 `unpacked`/`configured` 마커를 logcat에 찍는다.
- 그러나 native probe 시그니처는 `(package, libDir, filesDir, cacheDir, rootfsName, program)`
  **고정** — 추가 env를 받지 않는다. 그리고 `runtime_report.cpp`(L1623)는 LD_PRELOAD를
  `<rootfs>/usr/lib/androlinux/libalr_interpose.so` **하나만** 하드코딩한다 → 현재 그대로면
  fakeroot가 체인되지 않아 비root dpkg가 `requires superuser`/EPERM으로 죽는다.
- 단 loader는 여러 게이트를 **host(app) env에서 `getenv`로 읽는다**(`ALR_DISABLE_INTERPOSE`,
  `ALR_PCGATE`; L1607/L1611). 이게 **시그니처를 안 바꾸고** fakeroot를 켜는 깨끗한 훅이다.

### 5-요구 (R-V2-LAUNCH, 세 항목)

**(L1) 오버레이 추출.** onCreate에서 `fakeroot-stage.tar`와 `apt-dpkg-stage.tar`를 기존
`extractOverlayTar`(WS-4 M1 가드: SONAME downgrade 차단) 경로로 추출. chromium/foot/gtk3demo와
동일 패턴(`/data/local/tmp/<name>-stage.tar` 존재 시 추출, 크기-키 마커). 결과: rootfs에
`usr/lib/androlinux/libalr_fakeroot.so`(0755) + `usr/bin/dpkg`/`apt`/`tar`/… + `var/lib/dpkg`
admindir + `var/cache/apt/archives/hello_*.deb`.

**(L2) 체인 LD_PRELOAD.** fakeroot drain일 때 dpkg는 반드시
`LD_PRELOAD=<rootfs>/usr/lib/androlinux/libalr_fakeroot.so:<rootfs>/usr/lib/androlinux/libalr_interpose.so`
(**fakeroot FIRST**, interpose는 **드롭 금지** — 둘은 직교: fakeroot=credentials/stat, interpose=path)
+ `ALR_ROOTFS=<rootfs>` + `FAKEROOTUID=0` `FAKEROOTGID=0`로 실행.

**(L3) 마커 게이트.** 단일 device 자산 `/data/local/tmp/.alr-aptdrain` 존재 시에만 fakeroot
체인 drain을 활성화(`.alr-cr1` 마커와 동일 관례 — 평상시 무회귀, 통합/device 세션이 명시적으로 켬).

### 5-제안 diff A — runtime_report.cpp (loader, 메인 소유 — 제안만)
`ALR_FAKEROOT` host env가 set이면 fakeroot `.so`를 LD_PRELOAD **앞에** 체인 + FAKEROOT* 푸시.
시그니처·version stamp 불변, 일반 게스트(env unset) 완전 무회귀:

```cpp
// (L1623 부근, interpose LD_PRELOAD 결정 직후)
const bool fakeroot_on = []{
    const char* f = ::getenv("ALR_FAKEROOT");
    return f != nullptr && f[0] == '1';
}();
if (!interpose_off) {
    std::string preload = config.rootfs_dir + "/usr/lib/androlinux/libalr_interpose.so";
    if (fakeroot_on)                                  // fakeroot FIRST, interpose 뒤
        preload = config.rootfs_dir + "/usr/lib/androlinux/libalr_fakeroot.so:" + preload;
    guest_env.push_back("LD_PRELOAD=" + preload);
}
if (fakeroot_on) {                                    // fakeroot identity 계약
    guest_env.push_back("FAKEROOTUID=0");
    guest_env.push_back("FAKEROOTGID=0");
}
```
(`ALR_FAKEROOT`는 app 프로세스 env에 있어야 loader가 본다 → MainActivity가 drain 직전에
`Os.setenv("ALR_FAKEROOT","1",true)` 후 probe 호출, 일반 probe 경로는 unset 유지.)

### 5-제안 diff B — MainActivity.kt (launchPackageManagerProbes, 메인 소유 — 제안만)
`.alr-aptdrain` 마커 게이트 + 두 stage 추출 + `dpkg -i hello.deb`를 fakeroot env로 실행.
`hello`(libc6만 의존, device base 제공)를 타깃으로:

```kotlin
// 오버레이 추출 (onCreate, 기존 *-stage 패턴과 동일)
for (name in listOf("fakeroot", "apt-dpkg")) {
    val tar = java.io.File("/data/local/tmp/$name-stage.tar")
    if (tar.isFile) RootfsInstaller(this).extractOverlayTar(tar, rootfsStatus.rootfsDir)
}
// launchPackageManagerProbes(): 마커 게이트 + fakeroot 체인 dpkg -i
if (java.io.File("/data/local/tmp/.alr-aptdrain").isFile) {
    android.system.Os.setenv("ALR_FAKEROOT", "1", true)        // loader가 체인 켬
    val deb = File(rootfsDir, "var/cache/apt/archives/hello_2.10-3build1_arm64.deb")
    if (deb.isFile) {
        val out = nativeAlrNativeLoaderProbe(packageName, applicationInfo.nativeLibraryDir,
            filesDir.absolutePath, cacheDir.absolutePath, rootfsName,
            "/usr/bin/dpkg\n--force-not-root\n--force-bad-path\n-i\n" +
            "/var/cache/apt/archives/hello_2.10-3build1_arm64.deb")
        val unpacked  = out.contains("Unpacking hello") || out.contains("Preparing to unpack")
        val installed = out.contains("Status: install ok installed") // dpkg --status hello 후
        android.util.Log.i("alr_loader", "apt-drain: unpacked=$unpacked installed=$installed")
    }
    android.system.Os.unsetenv("ALR_FAKEROOT")                 // 일반 probe 무회귀
}
```

이 두 diff의 합 = "G1이 풀리면 마커 하나로 v2 종단이 켜진다"는 **단일 변경 표면**. 본체 세션이
적용하며, 이 세션은 요구·diff 제안까지만 소유한다(version stamp 불변).

---

## 6. 정직 분석 — exec-re-entry(G1)가 unpack의 선결인가? (핵심 질문)

질문: **"unpack만은 single-process라 G1 독립"인가, 아니면 maintainer-script fork+exec 때문에
G1이 선결인가?** dpkg 내부 동작을 단계로 쪼개 정직하게 답한다.

`dpkg -i hello.deb`의 단계와 각 단계가 두드리는 자원:

| 단계 | 동작 | 프로세스 | fakeroot(G2)로 충분? | exec-re-entry(G1) 필요? |
|------|------|----------|----------------------|--------------------------|
| 1. 게이트 통과 | `requires superuser` 체크 | dpkg in-proc | **예**(`getuid→0`) | 아니오 |
| 2. control 추출 | control.tar 읽기 | dpkg in-proc (libdpkg) | 무관 | 아니오 |
| 3. **data 압축해제** | `data.tar.zst` → `zstd`/`xz` **fork+execvp** 압축해제기 child | **fork+exec** | 무관 | **예 (최소 1회)** |
| 4. 파일 배치 | 각 파일 write + `chown(root:root)` + `chmod` + `stat` 확인 | dpkg in-proc | **예**(no-op+메타DB+오버레이) | 아니오 |
| 5. 디바이스 노드 | `mknod` (일부 패키지) | dpkg in-proc | **부분**(미interpose=hard wall) | 아니오 |
| 6. **maintainer scripts** | `preinst`/`postinst` → `/bin/sh` **fork+exec** | **fork+exec** | 무관 | **예** |

**정직한 결론(세 갈래):**

1. **메타 연산(1·2·4·5)은 G2 단독으로 device 가능 — G1과 독립.** `getuid`/`chown`/`chmod`/`stat`
   오버레이는 전부 dpkg **자기 프로세스 안**에서 일어나므로 fakeroot preload만 있으면 비root에서
   통과한다. 이 부분은 device에서 G1 없이도 증명 가능(아래 §7 DEVICE-REQ의 stat/chown 하위게이트).

2. **그러나 종단 `unpacked=true`는 G1을 통과해야 한다(완전 독립 아님).** dpkg는 압축 해제를
   `zstd`/`xz`를 **fork+execvp**로 돌린다(단계 3). 게스트 child의 커널-execve는 ALR에서 실패하므로
   (게스트 `ld.so` resolve 불가, ADR-003-v2 device-반증), **압축 .deb의 data 추출은 최소 1회의
   exec-re-entry re-map을 통과**해야 실제 파일이 풀린다. 즉 "unpack만은 single-process"는 **틀렸다**
   — extract 자체가 압축 해제기 child를 fork한다. (예외: data.tar가 **무압축**이면 libdpkg가
   in-proc로 읽어 단계 3을 건너뛸 수 있으나, noble `hello`는 `data.tar.zst`다.)

3. **설치 완료(`Setting up`, 단계 6)는 명백히 G1 의존.** maintainer scripts는 `/bin/sh` fork+exec
   이라 exec-re-entry B-3(child envp에 abs-rootfs `LD_PRELOAD`/`ALR_ROOTFS` 재주입) 없이는
   `Setting up hello`에 도달 못 한다. 기존 probe 주석(MainActivity L1806)도 동일하게 인정.

**따라서 device drain 설계:** G2+G3를 먼저 staged(이 세션 host 완료) → **(중간 하위게이트)**
G1 없이도 fakeroot 단독으로 "비root에서 dpkg가 게이트를 넘고 chown/stat이 root:root로 보인다"를
device 증명(메타 연산 = G1 독립 부분) → **(종단 게이트)** G1(static-startup crash까지) 풀리는 순간
`unpacked=true`(extract child re-map) → `installed`(maintainer script re-map). 이 순서가 §7.

**static-startup crash 위치(메인 영역, 읽기로 본 사실):** G1은 R10(map+jump device-proven) ·
R11(SIGILL fix) · R12(sequence-safe)까지 왔고 남은 게 **re-map된 static-startup child의 crash**다.
이건 fork+exec child(압축 해제기 `zstd` 같은 static 바이너리 포함)가 re-map 직후 entry에서 죽는
문제이므로, **단계 3(extract)도 단계 6(maintainer script)도 동일 G1 crash에 막힌다** → G1 한 곳이
풀리면 extract·configure 양쪽이 동시에 열린다(둘이 별개 벽이 아니다).

---

## 7. DEVICE-REQ — `ALR-V2-apt-unpack`

> 통합/device 세션(단일 device 소유)이 §9 drain으로 게이트. 이 세션은 APK install 안 함.
> 마지막 커밋 메시지에 `DEVICE-REQ: ALR-V2-apt-unpack` 마커로 요청.

**전제(staging — 이 세션 host 완료, device push 필요):**
- `fakeroot-stage.tar`, `apt-dpkg-stage.tar`를 `/data/local/tmp/`에 adb push → onCreate 추출
  (rootfs에 `libalr_fakeroot.so` + `dpkg`/`apt`/admindir + `hello_*.deb`).
- 게이트 마커 `/data/local/tmp/.alr-aptdrain` 생성(`adb shell touch …`).
- 본체에 §5 R-V2-LAUNCH(L1·L2·L3 + 제안 diff A/B) 적용됨.

**중간 하위게이트(G1 독립 — fakeroot 단독, 먼저 통과 가능):**
- `getuid()==0` under chained preload (fakeroot credential).
- 비root에서 dpkg가 `requires superuser` 게이트를 **넘는다**(`--force-not-root` + fakeroot).
- 게스트가 임의 파일 `chown(root,root)` 후 `stat` → **`st_uid==0 && st_gid==0`** 로 보인다
  (fakeroot 메타DB 오버레이). EPERM 없이 rc=0.

**종단 게이트(G1 ∧ G2 ∧ G3 — `unpacked=true`):**
- `dpkg -i hello.deb` under
  `LD_PRELOAD=<rootfs>/…/libalr_fakeroot.so:<rootfs>/…/libalr_interpose.so` (fakeroot FIRST)
  `ALR_ROOTFS=<rootfs> FAKEROOTUID=0 FAKEROOTGID=0` →
  - **`unpacked=true`**: `Unpacking hello` / `Preparing to unpack` 마커 + 풀린
    `<rootfs>/usr/bin/hello` 실재(extract child re-map 통과).
  - **`stat root:root`**: 풀린 파일의 `stat`이 `0:0`(fakeroot 오버레이).
  - **mknod placeholder**: device 노드가 있는 패키지는 `mknod` 미interpose → **placeholder**
    (정직한 hard wall; `hello`는 device 노드 없음 → 이 게이트엔 영향 없음).
- (스트레치) `dpkg --status hello` → `Status: install ok installed` (maintainer script
  re-map까지 = `Setting up hello`; G1 configure 경로).

**게이트 식(요약):** `PASS ⟺ unpacked=true ∧ (stat 풀린파일 == root:root) ∧ (mknod=placeholder 인지)`.
`unpacked`는 **여전히 PENDING-device**(G1 선결) — host는 staging+메커니즘까지만.

---

## 8. 알려진 한계 (정직)

| 한계 | 영향 | 비고 |
|------|------|------|
| `mknod`/`mknodat` 미interpose | device 노드 포함 패키지의 그 멤버는 placeholder | **유일 hard wall**. `hello`/대다수 앱은 device 노드 없음 → 무영향. fakeroot의 mknod 오버레이는 후속(메타DB에 노드 기록 + `stat`이 S_IFCHR/S_IFBLK 보고) |
| 호스트=Darwin | host에서 dpkg/.so 실행 불가 | host 증명 천장 = staging+메커니즘. unpack은 device-only |
| cross-process fake DB 비영속 | maintainer-script child가 부모 fake DB를 안 물려받음 | fake DB는 in-proc(`(dev,ino)` 해시). child는 자기 chown 재실행으로 재유도(README 설계). 단일 `.deb` unpack엔 충분; 영속 `.fakeroot` save-file은 후속 |
| data.tar 압축 시 extract도 G1 의존 | "unpack=single-process" 아님 | §6 결론 2. noble `hello`=`.zst` → 압축 해제기 child fork |

---

## 9. 교차참조

- 호환 매트릭스 apt-install 행: `docs/research/alr-compat-matrix.md`(이 SSOT가 진전 반영).
- exec-re-entry(G1) 설계: `docs/design/adr-003-multiprocess-exec-reentry.md`(§8 ADR-003-v2),
  `docs/research/loader-feature-gaps.md`(G1).
- fakeroot(G2) 메커니즘·체인 계약: `tools/fakeroot/README.md`, `tools/fakeroot/libalr_fakeroot.c`.
- staging(G3) 빌더: `tools/build_apt_dpkg_overlay.py`, `tools/build_fakeroot_overlay.py`,
  `tools/deb_closure.py`.

---
*갱신 규칙:* device evidence 추가 시 §7 게이트 항목을 evidence 파일명과 함께 갱신. host-only
진전만으로 `unpacked` 게이트를 PASS로 승급하지 않는다(device evidence 없이 완료 주장 금지).
*소유:* 이 문서·`tools/build_*overlay.py`·`tools/fakeroot/**` = 이 세션. 본체(§5 diff 대상) = 메인.
