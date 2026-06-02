# R12 remaining-gaps status — 6-lane drain checklist (WS-5 / L5 SSOT)

> **"v144(round-11) 토대 위에서 G1~G5 의 남은 갭을 6 lane 으로 어디까지 밀었고, 각 lane 이 device 로
> 검증되려면 무엇을 봐야 하는가"** 를 한 곳에 모은 SSOT. 갭 자체의 "왜 막혔나/무엇을 잠금해제하나"는
> `docs/research/loader-feature-gaps.md`(G1~G5 표)가 소유하고, 이 문서는 그 갭들을 R12 의 **실행 가능한
> 6 lane** 으로 쪼개 **단일 WS-1 드레인 체크리스트**로 묶는다(통합 세션이 단일 기기 직렬로 한 번에
> 드레인). chromium/CP-6 진행은 `docs/research/cp6-status.md`.
>
> 소유: WS-5(L5). **HOST-ONLY** — 새 device 측정 없음, 기존 `docs/evidence/` 인용만. 벤치 문서 아님
> (성능 숫자는 `docs/PERFORMANCE.md`/`docs/research/cp3-cpu-overhead-ratio.md`).
>
> baseline: 통합 트리 v144 (round-11 device-proven `docs/evidence/2026-06-02-round11-remap-sigill-fixed.md`;
> round-10 step2 `docs/evidence/2026-06-02-round10-step2-inproc-remap-mapjump.md`;
> round-10 step1 `docs/evidence/2026-06-02-round10-step1-inproc-reexec-mechanism-proven.md`;
> round-9 `docs/evidence/2026-06-02-round9-optionS-dead-wx-execve.md`;
> round-7 drain `docs/evidence/2026-06-02-round7-vkrender-pass-drain.md`).
> 디바이스 `R5KL20B6S3X` (SM-X236N, mt6878, Mali-G615 MC2, Android 16, 1200×1920@90Hz, untrusted_app).
>
> **정직 주의.** 이 6 lane 은 **전부 device-pending** 이다 — host-clean increment 와 host 게이트는
> 통과했으나 **어느 lane 도 device 검증으로 셀(RUNS/DONE) 승급되지 않았다**. `loader-feature-gaps.md`
> 의 G1=재-맵 매퍼 정확(SIGILL fixed)·G3=clear render PASS·G5=2-scene PASS·G4=포인터 USABLE 은
> 이미 device-proven 인 *토대*이고, R12 는 그 위의 *breadth/통합* 이다. device evidence 전까지 어떤
> lane 도 "풀림"으로 올리지 않는다.

---

## 0. 한눈에 — 6 lane × 베이스 갭 × device-req 게이트

| lane | 베이스 갭(loader-feature-gaps) | R12 가 미는 것 | host 상태 | device-req 게이트 |
|------|-------------------------------|----------------|-----------|-------------------|
| **g1-seqint** | G1 exec-re-entry | inproc-redirect per-exec scoping + `/proc/self/exe` pass-through + apt fakeroot 통합 | host-clean | 전역 `ALR_REEXEC_INPROC=1` 로 onCreate 직렬 프로브 시퀀스 **완주**(PERF 후 정지 0) + 재-맵 자식이 깨끗이 exit + `/proc/self/exe` exec 가 rootfs-재맵과 구분돼 통과 |
| **g3-vk** | G3 Vulkan render | textured draw + multi-draw → ICD(VK-M3) | host-clean | 게스트 textured `vkCmdDraw(Indexed)` 가 AHB color-attach 로 렌더되고 **컴포지터 sample** + multi-draw/state 전환 정확 replay (clear-only PASS 를 넘어) |
| **g5-gles3** | G5 GLES3 scene coverage | GLES3 entry point + 추가 glmark2 scene | host-clean | GLES3 scene(VAO/UBO/instanced/MRT)이 실 Mali 에 **software=false** 로 렌더 + GL_RENDERER passthrough `Mali-G615 MC2` |
| **g4-input** | G4 입력 interaction | 텍스트 입력 modifiers/repeat/keymap | host-clean | GTK/Qt 텍스트 위젯에 대문자(Shift)+단축키(Ctrl)+key-repeat 입력이 device 에서 들어가고 **NO_KEYMAP crash 0** (`wl_keyboard.keymap` XKB rootfs 경로) |
| **apt-fakeroot** | G1 의 독립 driver | 비-root dpkg superuser(fakeroot/root-emulation) | host-clean | `dpkg -i`/`apt install` 이 `requires superuser` 없이 unpack+configure(`unpacked=true`) — exec-re-entry 와 **독립** |
| **docs** | — (이 lane) | SSOT 갱신 + host-test enforcement | host-clean(pytest) | N/A (docs lane) — 이 문서가 6 lane 을 정직하게 추적하는지 host pytest 로 enforce |

> **순서.** g1-seqint 가 멀티프로세스(apt/dpkg/GIMP-plugin/chromium) 잠금해제의 최고 레버리지이고
> (`loader-feature-gaps.md` §0), apt-fakeroot 는 g1-seqint 의 (3)번 조각과 짝이나 exec-re-entry 와
> **독립**으로 진행 가능하다(비-root dpkg 거부는 exec 벽이 아니라 권한 벽). g3-vk/g5-gles3 는 GPU
> breadth(서로 ANGLE 경로 일부 공유), g4-input 은 GUI 입력 breadth — 셋 다 G1 무관이라 병렬.

---

## 1. lane g1-seqint — sequence integration (inproc-redirect scoping + /proc/self/exe + apt fakeroot)

**베이스(device-proven 토대).** round-11 v144 가 재-맵 게스트 SIGILL 을 고쳐(단일-span anon 매핑 +
세그먼트별 memcpy + 페이지별 union mprotect + 실 `AT_HWCAP` IRELATIVE)
**in-process 재-맵 매퍼는 이제 정확**하다(`docs/evidence/2026-06-02-round11-remap-sigill-fixed.md`:
재-맵 `/bin/sh` 가 `signal 4` 0, `sp_align=ok`, `bss_zeroed`, `entry=0x400640` 점프).

**R12 가 미는 것(세 조각).**
- **(1) inproc-redirect scoping** — `inproc` 전역 ON 은 onCreate 직렬 프로브 시퀀스를 PERF 후 정지시킨다
  (직렬 supervision + `/proc/self/exe` 엣지) → 현재 `ALR_REEXEC_INPROC=1` opt-in(기본 OFF). per-exec
  단위로 재-맵을 켜고(rootfs-내 절대경로 자식만) non-blocking 처리해 직렬 supervision 이 wedge 되지 않게.
- **(2) `/proc/self/exe` pass-through** — 게스트가 `/proc/self/exe`(interp `/system/bin/linker64`,
  chromium zygote 가 *Android* 앱 바이너리를 re-exec)를 exec 할 때는 rootfs-바이너리 재-맵과 **구분**해
  통과(Debian rootfs 로 매개 불가).
- **(3) apt fakeroot** — 비-root `dpkg: requires superuser` 우회(아래 apt-fakeroot lane 과 짝).

**host 상태.** host-clean(이 docs lane 의 pytest + 기존 exec_map/pathrw 프로토타입). darwin 호스트는 실커널
seccomp-across-execve/SEIZE-EVENT_EXEC 거동 불가 → 분류/분기 로직 회귀만, 통합 효과는 device-only.

**device-req 게이트.** 전역 `ALR_REEXEC_INPROC=1` 로 **onCreate 직렬 프로브 시퀀스가 완주**(PERF 프로브
후 정지 0, 이후 GPU/GUI 프로브 모두 실행) + 재-맵된 자식이 깨끗이 exit + `/proc/self/exe` exec 가
rootfs-재맵과 구분돼 통과. **이 게이트 PASS 전엔 `loader-feature-gaps.md` G1 셀 승급 금지.**

---

## 2. lane g3-vk — VK render breadth (textured/multi-draw toward ICD)

**베이스(device-proven 토대).** round-7 v139 가 clear `vkQueueSubmit`=VK_SUCCESS 로 render 경로
device-검증을 마쳤다(`docs/evidence/2026-06-02-round7-vkrender-pass-drain.md`: device created + clear
submit VK_SUCCESS, Mali-G615 MC2, API 1.3). enumerate(round-5) + clear render 둘 다 device-PASS.

**R12 가 미는 것.** **(a) textured draw** — vertex/index buffer + sampler + texture upload 를 거치는
실제 `vkCmdDraw`/`vkCmdDrawIndexed` 를 마샬해 AHB color-attach 로 렌더; **(b) multi-draw / 다중
파이프라인** — 여러 draw 와 state 전환이 한 command buffer 안에서 정확히 replay; **(c) ICD(VK-M3)** —
게스트 `libvulkan_alr.so` ICD + manifest 로 게스트가 표준 `libvulkan.so` loader 경로로 ALR 마샬 device 를
잡게 함(현재 직접 링크). clear-only → textured/multi-draw 가 ICD 로 가는 길의 분자.

**host 상태.** host-clean(wire-check decoder replay + 4-ABI NDK 빌드 게이트는 WS-2/통합 소유; 이 docs
lane 은 추적만).

**device-req 게이트.** 게스트 textured `vkCmdDraw(Indexed)` 가 AHB color-attach 로 렌더되고 컴포지터가
sample(화면 present) + multi-draw/state 전환 정확 replay. **clear-only PASS 만으론 G3 PARTIAL 유지.**

---

## 3. lane g5-gles3 — GLES3 scene coverage (full glmark2 14-scene + GLES3+)

**베이스(device-proven 토대).** glmark2-es2 build+texture **2-scene** 이 실 Mali 에 렌더
(Score~1000+, software=false; `docs/evidence/2026-06-02-cp2-FINAL-glmark2-score-1074.md`,
`docs/evidence/2026-06-02-cp5-batch-8mibring-texture-ws4-overlays.md`). host Mali context 는 이미 GLES
**3.2**(`docs/evidence/2026-06-02-5ws-fanout-renderer-getpwuid-pkgfunc.md` passthrough).

**R12 가 미는 것.** **(a) GLES3 entry point** — VAO/UBO/`glDrawElementsInstanced`/sampler object/MRT 등을
shim → ring → host decoder 로 마샬(현 GLES2 19-op state-setter 커버리지 확장); **(b) 추가 scene** —
glmark2 14-scene 중 shading/bump/refract/conditionals 등 더 많은 scene(duration↑ 또는 분할 launch);
**(c) host wire-check** — 새 op 마다 decoder replay + wire-check 케이스(off-device round-trip).

**host 상태.** host-clean(shim 빌드 + wire-check + 4-ABI NDK 게이트는 WS-2/통합 소유).

**device-req 게이트.** GLES3 scene(VAO/UBO/instanced/MRT)이 실 Mali 에 **software=false** 로 렌더 +
GL_RENDERER passthrough `Mali-G615 MC2`. **GLES2 2-scene PASS 만으론 G5 PARTIAL 유지.**

---

## 4. lane g4-input — GUI text input (modifiers/repeat/keymap)

**베이스(device-proven 토대).** GIMP 에서 포인터 입력 주입 USABLE✓(터치로 메뉴/다이얼로그/브러시까지;
`alr-gui-android-native-polish` 메모리). base rootfs 는 이미 `/usr/share/X11/xkb` + libxkbcommon 동봉
(`ws4-xkb-data-present` 메모리) — NO_KEYMAP 는 `XKB_CONFIG_ROOT`→rootfs 경로 fix(compositor 측).

**R12 가 미는 것(세 축).** **(1) modifiers** — Shift/Ctrl/Alt/Super 가 `wl_keyboard.modifiers`
(mods_depressed/latched/locked/group)로 정확히 전달돼 대문자·단축키 동작; **(2) key repeat** —
`wl_keyboard.repeat_info`(rate/delay) + Android long-press → 게스트 반복 입력; **(3) keymap** —
`wl_keyboard.keymap`(XKB_V1)가 rootfs xkb-data 로 해석돼 NO_KEYMAP SEGV 없이 키코드→keysym.

**host 상태.** host-clean(입력 라우팅/keymap 빌드는 WS-3 소유; 이 docs lane 은 추적만).

**device-req 게이트.** GTK/Qt 텍스트 위젯에 대문자(Shift)+단축키(Ctrl)+key-repeat 입력이 device 에서
들어가고 **NO_KEYMAP crash 0**. **포인터 USABLE 만으론 텍스트-입력 셀 미승급.**

---

## 5. lane apt-fakeroot — non-root dpkg superuser (fakeroot/root-emulation)

**베이스(device-확정 벽).** `apt install` 은 `unpacked=false` 이고 핵심 driver 는 **exec-re-entry 와
독립**으로 드러났다(`docs/evidence/2026-06-02-round7-vkrender-pass-drain.md`:
`dpkg: error: requires superuser privilege` — 비-root dpkg 가 unpack 거부). 이는 exec 벽이 아니라
**권한 벽** 이라 g1-seqint 와 별개 lane.

**R12 가 미는 것.** 비-root `dpkg`/`apt` 가 superuser 권한을 요구하는 경로를 fakeroot/root-emulation 으로
우회(uid/gid 0 가장, chown/chmod no-op-or-record) → maintainer-script 실행 + unpack/configure 성립.
full apt+dpkg+solver 스테이징 오버레이(현 `apt-config-stage.tar` minimally-staged)와 병행.

**host 상태.** host-clean(스테이징 오버레이 빌드는 WS-4/통합 소유; 이 docs lane 은 추적만).

**device-req 게이트.** `dpkg -i`/`apt install` 이 `requires superuser` 없이 **`unpacked=true` +
configured** 로 진행(maintainer-script 실행). exec-re-entry(g1-seqint)와 독립으로 device-검증 가능.

---

## 6. lane docs — SSOT 갱신 + host-test enforcement (이 lane)

**R12 가 미는 것.** (1) `loader-feature-gaps.md` 의 G1/G3/G4/G5 에 각 lane 의 `(R12 in-flight)` 노트를
추가(device-proven 사실은 불변·정직 유지, in-flight 를 DONE 으로 표시 금지). (2) 이 문서
(`r12-remaining-gaps-status.md`)로 6 lane 을 단일 WS-1 드레인 체크리스트로 묶음. (3) host pytest
doc-enforcement 테스트로 이 문서 존재 + 6 lane 명시 + loader-feature-gaps 의 R12 in-flight 노트를 enforce
(immutable round-6/7/9/10 history 테스트 불변).

**host 게이트.** `uvx pytest -q` 그린. device-req 게이트 = **N/A(docs lane)**.

---

## 7. 단일 WS-1 드레인 체크리스트 (통합 세션이 단일 기기 직렬로 한 번에)

device 측정은 통합 세션이 단일 기기 직렬로 수행(빌드→`adb install -r -d`→`am force-stop`→`logcat -c`→
cold start→`logcat -s alr_loader`). 한 번의 드레인에서 6 lane 게이트를 순서대로 확인:

- [ ] **g1-seqint** — `ALR_REEXEC_INPROC=1` 전역 ON 으로 onCreate 직렬 프로브 시퀀스 **완주**(PERF 후
      정지 0, GPU/GUI 프로브 전부 실행) + 재-맵 자식 깨끗이 exit + `/proc/self/exe` exec pass-through.
- [ ] **apt-fakeroot** — `dpkg -i`/`apt install` 이 `requires superuser` 없이 `unpacked=true` +
      configured(maintainer-script 실행). g1-seqint 와 독립으로 판정 가능.
- [ ] **g3-vk** — 게스트 textured `vkCmdDraw(Indexed)` AHB color-attach 렌더 + 컴포지터 sample +
      multi-draw 정확 replay (`ALR VK RENDER MARSHAL: PASS` 를 textured/multi-draw 로 확장).
- [ ] **g5-gles3** — GLES3 scene(VAO/UBO/instanced/MRT) 실 Mali software=false 렌더 + GL_RENDERER
      passthrough `Mali-G615 MC2`.
- [ ] **g4-input** — GTK/Qt 텍스트 위젯에 Shift(대문자)+Ctrl(단축키)+key-repeat 입력 device 진입 +
      **NO_KEYMAP crash 0**.
- [ ] **no-regress** — `ALR GPU LIVE INTEGRATION: PASS`, `ALR VK RENDER MARSHAL: PASS`,
      `ALR GPU SCREEN CUBE: PASS`, foot/netsurf/qt6 `rendered=true`, glmark2 Score>0, Chromium 147
      runs, crash 0 (v144 회귀 기준선 `docs/evidence/2026-06-02-round11-remap-sigill-fixed.md`).

> **승급 규칙.** 각 lane 은 위 게이트가 **device evidence 로 PASS** 한 뒤에만 `loader-feature-gaps.md`
> 의 해당 G# 를 PARTIAL→DONE(또는 RUNS) 으로 올린다(evidence 파일명 명기). host-clean increment·매퍼
> 정확성·host 프로토타입만으로는 셀 승급 금지. CP-6/chromium 은 사용자 보류 — 이 6 lane 에 chromium
> 멀티프로세스 *셀 승급*은 포함하지 않는다(g1-seqint 토대 위에 올라가나 보류 해제 + device 게이트 후).
