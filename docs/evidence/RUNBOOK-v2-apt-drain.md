<!-- v2 apt-unpack device-drain RUNBOOK. 통합(integration) 세션이 v2 `dpkg -i hello.deb`
device drain을 **단 1회**로 끝내는 정확한 순서. device는 5세션 공유 — 통합 세션만 실행.
SSOT = docs/design/v2-apt-pipeline-ssot.md(§5 계약·§7 DEVICE-REQ). 이 런북은 그 §7을 한 번에
집행하는 절차서다. 작성: research/apt-launch (T3), base main 4b3396e (v160). -->

# RUNBOOK — v2 apt-unpack device drain (`dpkg -i hello.deb` → `unpacked=true`)

> **목표 게이트:** logcat에 `aptdrain: ... unpacked=true` AND device rootfs에
> `var/lib/dpkg`(또는 status)에 `hello` 기록 AND 풀린 파일 `stat` = `root:root`(0:0).
> **DEVICE-REQ 마커:** 통합 세션의 마지막 커밋 메시지에 `DEVICE-REQ: ALR-V2-apt-unpack`.
>
> 이 런북은 **device를 만지는 통합 세션 전용**이다. research/apt-launch(이 문서 소유 세션)는
> APK를 빌드/설치하지 않는다(공유 device 덮어쓰기 방지). host 산출물·요구·diff까지만 소유.

대상 device: adb serial **`R5KL20B6S3X`** (SM-X236N / Mali-G615 / Android 16 / untrusted_app).
applicationId: `dev.chanwoo.androlinux`. device rootfs:
`/data/data/dev.chanwoo.androlinux/files/rootfs/<rootfsName>`
(= `/data/user/0/dev.chanwoo.androlinux/files/rootfs/<rootfsName>`).

---

## 0. 한 줄 요약 (왜 이 순서인가)

`unpacked=true` ⟺ **G1 ∧ G2 ∧ G3** (SSOT §1). G2(fakeroot)·G3(apt+dpkg staging)는 **host-ready**라
adb push 한 번이면 device에 깔린다. 남은 선결은 **G1 exec-re-entry static-startup crash** —
이것이 PR#8(`research/static-reentry`)의 코드 fix 대상이다. 따라서 1회 drain의 뼈대는:

> **(선행 머지 3건) → (1빌드) → (host stage 3산출물) → (adb push + 마커) → (콜드스타트 1회) → (게이트 grep)**.

G1이 풀리는 순간 G2+G3는 **이미 staged**이므로 같은 콜드스타트 안에서 `dpkg -i hello.deb`가 종단까지
간다(SSOT §6 결론 3: extract child와 maintainer-script가 **동일** G1 crash 한 곳에 막혀 있어 한 번에 열림).

---

## 1. 선행 머지 (PR#8 + T1 + T2) — drain 전에 main에 들어가 있어야 함

이 3건이 **하나라도 빠지면** drain은 무조건 실패한다. 통합 세션이 머지 게이트(메모리
`merge-gate-single-integration`)로 순서대로 머지한다.

| # | 무엇 | 브랜치/PR | 게이트 의미 | 빠지면 |
|---|------|-----------|-------------|--------|
| **(a) PR#8** | **G1 static re-map fix**: IRELATIVE 1027→1032, `enter_guest` TP `xzr`→zeroed-TCB mmap(16384)+center, brk(0) init (`alr_inproc_reexec.c` + `alr_reentry.c`) | `research/static-reentry` (PR #8) | re-map된 압축해제기/`/bin/sh` child가 entry에서 안 죽음 | extract child가 SIGILL/SIGSEGV → `unpacked=false` |
| **(b) T1** | **MainActivity wiring** (§5 제안 diff B): `.alr-aptdrain` 마커 게이트 + 두 stage 추출 + `ALR_FAKEROOT=1` env 후 `dpkg -i hello.deb` probe | T1 작업(본체 추가-위주) | drain 진입점·마커 게이트·체인 env 푸시 | probe 자체가 없음 → 아무 일도 안 일어남 |
| **(c) T2** | **runtime_report.cpp patch** (§5 제안 diff A): `ALR_FAKEROOT=1`이면 fakeroot `.so`를 LD_PRELOAD **앞에** 체인 + `FAKEROOTUID/GID=0` 푸시 | T2 작업(본체 loader) | 비root dpkg가 `requires superuser`/`chown EPERM` 통과 | fakeroot 미체인 → dpkg가 게이트/EPERM으로 죽음 |

**머지 확인(통합 세션 worktree, main):**
```sh
git log --oneline -6        # PR#8/T1/T2 머지 커밋 확인
grep -n "ALR_FAKEROOT"  app/src/main/cpp/runtime_report.cpp       # T2: 1+ hit
grep -n "alr-aptdrain\|ALR_FAKEROOT" app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt  # T1: 1+ hit
grep -n "1032" app/src/main/cpp/alr_inproc_reexec.c              # PR#8: IRELATIVE=1032
```
세 grep이 전부 hit여야 다음 단계로. (T1/T2는 SSOT §5 diff A/B가 정본.)

---

## 2. APK 빌드 (통합 세션이 stamp 소유 — research/apt-launch는 stamp 불변)

> `JAVA_HOME` 미설정 시 빌드가 조용히 **stale APK**를 남긴다(메모리 `build-needs-java-home`).
> 반드시 설정 후 versionCode를 device에서 재확인.

```sh
export JAVA_HOME="$(/usr/libexec/java_home -v 17)"   # openjdk@17
./gradlew :app:assembleDebug
# 결과: app/build/outputs/apk/debug/app-debug.apk (versionCode = 통합 세션 stamp)
```

---

## 3. host stage 산출 (3개 — research/apt-launch 소유, host-ready)

한방 래퍼가 fakeroot-stage + apt-dpkg-stage + `hello.deb`를 한 번에 만든다(SSOT §3·§4).

```sh
# (권장) 한방 — 세 산출물 + §5 device-cmd 출력까지
tools/build_v2_stage.sh --out /tmp/v2-stage
#   → /tmp/v2-stage/fakeroot-stage.tar
#   → /tmp/v2-stage/apt-dpkg-stage.tar
#   → /tmp/v2-stage/hello_2.10-3build1_arm64.deb   (apt-dpkg-stage가 var/cache/apt/archives에도 포함)

# (개별) 위가 막히면 분리 빌드:
python3 -m tools.build_fakeroot_overlay  --out /tmp/fakeroot-stage.tar --json          # zig만 필요(오프라인)
python3 -m tools.build_apt_dpkg_overlay  --out /tmp/apt-dpkg-stage.tar --base <base-rootfs.tar|dir> \
        --self-contained --fetch-test-deb /tmp/v2-stage --json                         # 네트워크(ports.ubuntu.com noble)
```
**전제 도구:** `zig`(fakeroot shim aarch64 크로스컴파일, REQUIRED), `ar`+네트워크(apt-dpkg closure).
빌드 검증치(SSOT §4): fakeroot `.so` 23/23 심볼·EM_AARCH64, apt-dpkg closure unsatisfied=∅·violations=0,
`hello_2.10-3build1_arm64.deb` (25184 B, `data.tar.zst`).

---

## 4. adb push + 마커 (device에 stage 적재 + drain 게이트 ON)

```sh
S=R5KL20B6S3X
adb -s $S install -r app/build/outputs/apk/debug/app-debug.apk
adb -s $S push /tmp/v2-stage/fakeroot-stage.tar   /data/local/tmp/fakeroot-stage.tar
adb -s $S push /tmp/v2-stage/apt-dpkg-stage.tar   /data/local/tmp/apt-dpkg-stage.tar
# hello.deb는 apt-dpkg-stage.tar의 var/cache/apt/archives/ 안에 이미 포함 → 별도 push 불필요.
# (개별 빌드로 분리했으면) adb -s $S push /tmp/v2-stage/hello_2.10-3build1_arm64.deb /data/local/tmp/
adb -s $S shell touch /data/local/tmp/.alr-aptdrain          # ★ drain 게이트 마커 (이게 없으면 무회귀)
```
**무회귀 계약:** `.alr-aptdrain`이 없으면 onCreate는 평소대로 — fakeroot 체인/drain 미발동
(`.alr-cr1`·`.alr-cr2`와 동일 관례, SSOT §5 L3). 평상시 cold start는 영향 없음.

---

## 5. 콜드 스타트 1회 (force-stop FIRST — onCreate 재실행 필수)

> warm resume는 `onCreate`(오버레이 추출 + drain probe)를 건너뛴다(메모리
> `device-test-force-stop-first`). 반드시 **force-stop → logcat -c → start** 순서.

```sh
S=R5KL20B6S3X
adb -s $S shell am force-stop dev.chanwoo.androlinux
adb -s $S shell logcat -c
adb -s $S shell am start -n dev.chanwoo.androlinux/.MainActivity
# probe가 끝날 시간(두 stage 추출 + dpkg unpack): 30~60s 대기.
adb -s $S logcat -d -s alr_loader > /tmp/v2-apt-drain-$(date +%Y%m%d).txt
```

---

## 6. 게이트 판정 (logcat grep + device 사실 2건)

### 6-A. 종단 게이트 — `unpacked=true` (PASS 조건의 핵심)
```sh
grep -E "aptdrain: .*unpacked=true" /tmp/v2-apt-drain-*.txt     # T1 로그 라인 (launchAptDrainProbe, §5 diff B)
grep -E "Unpacking hello|Preparing to unpack" /tmp/v2-apt-drain-*.txt   # dpkg unpack 마커
```
둘 다 보이면 extract child re-map(G1) 통과. **추가로 device 사실 2건을 직접 확인:**
```sh
RF=/data/data/dev.chanwoo.androlinux/files/rootfs           # 실제 <rootfsName>은 manifest명; ls로 확정
adb -s $S shell "ls -la $RF/*/usr/bin/hello"                # 풀린 바이너리 실재
adb -s $S shell "grep -A1 '^Package: hello' $RF/*/var/lib/dpkg/status"   # dpkg-db에 hello 기록
adb -s $S shell "stat -c '%u:%g' $RF/*/usr/bin/hello"       # ★ '0:0' (fakeroot 오버레이) 여야 함
```

### 6-B. 중간 하위게이트 (G1 독립 — fakeroot 단독, 먼저 떨어져 있을 수 있음)
T2 fakeroot 체인이 살아있는지 빠르게 본다(`unpacked`가 실패해도 여기까진 통과해야 정상):
```sh
grep -E "requires superuser|must be superuser" /tmp/v2-apt-drain-*.txt   # ← 이게 보이면 fakeroot 미체인 (T2 회귀)
grep -E "FAKEROOTUID|getuid.*=0|chown.*EPERM" /tmp/v2-apt-drain-*.txt
```
`requires superuser`/`chown EPERM`이 보이면 **G2 회귀** → §7 트러블슈팅 T2 항목.

### 6-C. 게이트 식 (SSOT §7 요약)
```
PASS ⟺ unpacked=true ∧ (stat 풀린파일 == 0:0) ∧ (dpkg status에 hello) ∧ (mknod=placeholder 인지)
```
`hello`는 device 노드가 없으므로 `mknod` placeholder 항목은 N/A(영향 없음). 회귀 게이트(bench)는
이 v2 drain에 별도 룰이 없으면 생략 가능 — 핵심은 위 3 grep + stat.

---

## 7. crash 시 분기 — PR#8 static fix `si_code` 분리 (SSOT §6·ADR-static-reentry)

`unpacked=false`이고 guest가 죽으면, **어느 fix가 안 먹었는지**를 device `si_code`로 가른다
(`adr-static-reentry-startup-crash.md` §2, PR#8 §검증):

| 증상 (logcat) | si_code / fault | 원인 | 조치 |
|---------------|-----------------|------|------|
| guest signal 11, fault addr ≈ small-near-0 | `SEGV_MAPERR` | **TP=NULL** (pre-TLS canary) — PR#8 FIX-2(zeroed-TCB) 미적용/회귀 | `alr_inproc_reexec.c` `enter_guest`가 `xzr` 아닌 mmap(16384) center를 `tpidr_el0`에 쓰는지 |
| guest signal 4, fault PC가 데이터/0 영역 | `ILL_ILLOPC` | **IRELATIVE=1027 오인** (데이터를 함수 호출) — PR#8 FIX-1 미적용 | `R_AARCH64_IRELATIVE`가 **1032**(`alr_inproc_reexec.c`·`alr_reentry.c` 둘 다), RELATIVE=1027 분리 |
| guest signal 11, `__libc_setup_tls`/`__sbrk` 부근 | `SEGV` (brk) | **brk(0) 미초기화** (glibc BZ2066147) — PR#8 FIX-3 | jump 전 `brk(0)` init 적용 확인 |
| crash인데 fault PC를 못 읽음(handler 미발화) | — | ART libsigchain 2차 SEGV 마스킹 | (FIX-4 SIG_DFL reset; PR#8 범위 밖 — 진단 필요 시만) |

**핵심 사실(SSOT §6 마지막):** extract child(압축해제기 static 바이너리)와 maintainer-script `/bin/sh`는
**같은 G1 static-startup crash 한 곳**에 막힌다. PR#8 한 묶음이 풀리면 extract·configure가 **동시에** 열린다.
즉 crash 분기는 "어느 fix 항목 회귀인가"만 가르면 되고, 새 벽이 아니다.

---

## 8. 정직한 범위 + 알려진 한계 (SSOT §8)

- **host 천장:** 호스트=Darwin → aarch64 `.so`/`dpkg` 실행 불가. host는 stage 빌드·구조·심볼·체인
  정합까지만 증명. `unpacked=true`는 **device-only** 주장(이 런북이 그 device 1회를 집행).
- **`mknod`/`mknodat` 미interpose** = 유일 hard wall. device 노드 포함 패키지의 그 멤버만 placeholder.
  `hello`는 device 노드 없음 → 무영향.
- **cross-process fake DB 비영속:** maintainer-script child는 부모 fake DB를 안 물려받음 → 자기 chown
  재실행으로 재유도(README 설계). 단일 `.deb` unpack엔 충분. 영속 `.fakeroot` save-file은 후속.
- **`Setting up hello`(configure, 스트레치):** maintainer-script `/bin/sh` re-map까지 가야 함
  (`dpkg --status hello` → `Status: install ok installed`). `unpacked=true`가 1차 게이트, configure는 +α.

---

## 9. drain 1회 체크리스트 (복붙용)

```sh
S=R5KL20B6S3X
# 1) 선행 머지 확인 (PR#8/T1/T2)
git log --oneline -6
grep -q ALR_FAKEROOT app/src/main/cpp/runtime_report.cpp && \
grep -q alr-aptdrain app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt && \
grep -q 1032 app/src/main/cpp/alr_inproc_reexec.c && echo "merges OK" || echo "MISSING MERGE"
# 2) 빌드
export JAVA_HOME="$(/usr/libexec/java_home -v 17)"; ./gradlew :app:assembleDebug
# 3) host stage
tools/build_v2_stage.sh --out /tmp/v2-stage
# 4) push + 마커
adb -s $S install -r app/build/outputs/apk/debug/app-debug.apk
adb -s $S push /tmp/v2-stage/fakeroot-stage.tar /data/local/tmp/fakeroot-stage.tar
adb -s $S push /tmp/v2-stage/apt-dpkg-stage.tar /data/local/tmp/apt-dpkg-stage.tar
adb -s $S shell touch /data/local/tmp/.alr-aptdrain
# 5) 콜드스타트
adb -s $S shell am force-stop dev.chanwoo.androlinux
adb -s $S shell logcat -c
adb -s $S shell am start -n dev.chanwoo.androlinux/.MainActivity
# (30~60s 대기)
adb -s $S logcat -d -s alr_loader > /tmp/v2-apt-drain-$(date +%Y%m%d).txt
# 6) 게이트
grep -E "aptdrain: .*unpacked=true|Unpacking hello|Preparing to unpack" /tmp/v2-apt-drain-*.txt
RF=/data/data/dev.chanwoo.androlinux/files/rootfs
adb -s $S shell "ls -la $RF/*/usr/bin/hello; stat -c '%u:%g' $RF/*/usr/bin/hello"
adb -s $S shell "grep -A1 '^Package: hello' $RF/*/var/lib/dpkg/status"
```

**PASS 시:** `docs/evidence/_TEMPLATE.md`로 새 evidence 파일 작성(실제 logcat 라인 붙여넣기, 의역 금지),
SSOT §7 게이트 항목을 evidence 파일명과 함께 PASS로 승급, 마지막 커밋에 `DEVICE-REQ: ALR-V2-apt-unpack`.

---

## 10. 교차참조
- §5 launch 계약 · §7 DEVICE-REQ 정본: `docs/design/v2-apt-pipeline-ssot.md`.
- G1 crash 분기(si_code) 정본: `docs/design/adr-static-reentry-startup-crash.md` (+ PR#8 `research/static-reentry`).
- host stage 빌더: `tools/build_v2_stage.sh`, `tools/build_fakeroot_overlay.py`, `tools/build_apt_dpkg_overlay.py`.
- device 캡처 일반 관례: `docs/evidence/CAPTURE-RUNBOOK.md` (force-stop-first, evidence 작성).
