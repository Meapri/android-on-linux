# M13 — `wrappers.def` 와 심볼 게이트, 그리고 그것이 즉시 찾아낸 24개

- **날짜**: 2026-08-03
- **기기**: MediaTek arm64, `uid=10297 Seccomp=2 u:r:untrusted_app_27:s0`
- **수용 테스트**: **PASS=74 FAIL=0 KNOWN_FAIL=2** (이전 73/0/2)
- **호환성 폭**: 96/96 설치, 95/96 실행 — 회귀 없음
- **호스트 게이트**: 9/9 PASS

[M12](2026-08-03-m12-spawn-resolver.md) 에서 exec 진입점 6개가 프로젝트 내내 없었는데 **아무것도 그것을 알려주지 않았다**. 그 사실 자체가 가장 큰 결함이었다. `docs/04-preload-spec.md` §6 은 `src/preload/wrappers.def` 를 심볼의 **단일 정본**으로 지정하고 §14 는 그것을 읽는 `PRELOAD CHK SYMBOLS PRESENT` 게이트를 요구하는데, **둘 다 존재한 적이 없었다.**

## 1. 게이트를 만들자 24개가 더 나왔다

`wrappers.def` 를 **스펙 §6 에서** 작성하고(바이너리에서 덤프하면 동어반복이라 아무것도 못 잡는다 — termux 레시피의 무결성 가드가 정확히 그 함정에 빠져 있었다), 실제 바이너리와 대조했다:

```
스펙 §6 에서 추출한 후보: 170
바이너리에 정의됨:        140
```

산문 언급(가로챌 수 없는 glibc 내부 함수 등)을 걷어내고 남은 **진짜 누락 24개**:

| 계열 | 누락 심볼 |
|---|---|
| §6.2 stat | `fstat` `fstat64` `__fxstat` `__fxstat64` `newfstatat` |
| §6.5 디렉토리·cwd | `scandir` `scandir64` `getwd` `__getwd_chk` `__getcwd_chk` |
| §6.3 access | `faccessat2` |
| §5.3 정규화 | `__realpath_chk` `__readlinkat_chk` |
| §6.12 터미널 | `ttyname` `ttyname_r` `__ttyname_r_chk` |
| §6.14 임시 파일 | `tmpfile` `tmpfile64` `tmpnam` `tmpnam_r` `tempnam` |
| §6.6/6.7 경로 인자 | `mkfifoat` `name_to_handle_at` |
| §6.13 동적 로딩 | `dlmopen` |

전부 구현했다. 심볼 수 140 → 164.

## 2. `fstat` 누락은 실제로 깨져 있었다 — 대조군으로 확정

스펙 §6.2 는 실패 모드까지 적어 두었다:

> `fstat`/`fstat64` 는 경로 인자가 없어 재작성과 무관하지만 **link2symlink 에 필수다**. 빠뜨리면 `stat(path)` 는 `st_nlink=2` 를 보고하는데 같은 파일의 `fstat(fd)` 는 1을 보고해 dpkg 무결성 검사가 깨진다.

"고쳤더니 통과한다" 는 "이전에 깨져 있었다" 의 증거가 아니다. `ALR_NO_FSTAT` 컴파일 가드로 래퍼만 뺀 빌드를 배포해 **깨진 상태를 직접 측정**했다.

측정은 **한 프로세스 안에서** 해야 한다. 신원 테이블은 설계상 프로세스 단위이고(§8.2 — git 이 링크와 검사를 한 프로세스에서 하기 때문), 셸에서 `ln` 과 확인을 나눠 하면 테이블이 비어 있어 아무것도 증명하지 못한다. 그래서 `tests/device/probe_nlink.c` 를 게스트 gcc 로 컴파일해 `link()` → `stat()` → `fstat()` 를 한 프로세스에서 돌렸다.

**대조군 (`fstat` 래퍼 없음, 배포본 심볼 0개 확인):**

```
stat(a)  nlink=2 ino=355816
stat(b)  nlink=2 ino=355816
fstat(b) nlink=1 ino=355817      ← nlink 도 inode 도 불일치
MISMATCH
```

**수정 후:**

```
stat(a)  nlink=2 ino=355756
stat(b)  nlink=2 ino=355756
fstat(b) nlink=2 ino=355756
NLINKOK
```

기존 `PRELOAD LINK2SYMLINK` 테스트는 `ln` 이 죽지 않는 것만 보므로 이것을 잡을 수 없었다. `PRELOAD LINK IDENTITY NLINK` 를 추가했다.

## 3. 게이트가 동어반복이 아님을 증명

가짜 심볼 한 줄을 `wrappers.def` 에 넣고 확인:

```
PRELOAD CHK SYMBOLS PRESENT: FAIL  declared in wrappers.def but not defined: alr_nonexistent_probe_symbol
ALR PRELOAD GATES: FAIL
```

원복하면 다시 PASS. **선언이 먼저, 구현이 나중이고, 게이트가 틀리면 게이트가 맞다.**

`wrappers.def` 에는 **"일부러 구현하지 않은 것"** 절도 둔다 — 게이트가 의도적 누락을 오탐하지 않도록, 그리고 각 결정의 이유가 코드 옆에 남도록:

- **§6.15 fd 상태 쓰기 표면** (`write` `pwrite` `writev` `fwrite` `fputs` …): `/dev/full`(§12) 전용인데 그것이 영구 비목표다. 게다가 빠뜨린 stdio 심볼은 **조용히 성공한 쓰기**가 되고, preload 자신이 내부적으로 `write()` 를 부르므로 정의하면 자기 호출을 가로챈다.
- **§11 `ioctl` 번역**: 명세되었으나 **미구현**. `FIONREAD` 에뮬레이션은 PTY 마스터 fd 가 필요한데 게스트 쪽은 그것을 쥐고 있지 않다. **포기가 아니라 미해결**이며, 없을 때 무엇이 깨지는지는 `UNVERIFIED` 다.
- **glibc 내부** (`__open` `__nss_files_fopen` `__gen_tempname` …): 공개 심볼이 아니라 원리적으로 불가. 스펙에 이름이 있는 것은 그 위의 공개 진입점을 왜 가로채는지 설명하기 위해서다.
- **`setgroups`**: seccomp 차단. 가상화할 것이 없다.

## 4. 함께 닫은 것

- **`codex` 체크섬 검증**: node 는 `SHASUMS256.txt` 로 검증하는데 codex 는 **아무 검증도 없었다**. 릴리스의 체크섬 에셋을 흔한 이름들로 시도하고, 없으면 "UNVERIFIED" 를 명시적으로 경고한다. 조용한 미검증보다 낫다. 자산 이름의 `-musl` 이 [M12 §8](2026-08-03-m12-spawn-resolver.md) 에서 확인한 정적 링크의 근본 원인이라는 점도 코드 주석에 남겼다.
- **rootfs 가 자기 자신을 설명하게**: `install_preload()` 가 `.so` 옆에 manifest 도 복사한다(§3.4). 이 프로젝트에서 "배포된 바이너리가 내가 생각한 것이 아니었다" 가 세 번 있었다.
- **존재하지 않을 파일을 약속하던 스펙 정정**: `libalr_fakeroot.so` 는 만들지 않는다 — fakeroot 신원 사칭은 `ALR_FAKEROOT=1` 뒤에서 preload 안에 있다. `.so` 를 하나 더 두면 exec 마다 DSO 매핑·재배치가 늘어난다. `docs/02-architecture.md` §4.4, `05-provisioning-spec.md` §3.4, `08-milestones.md` M0 에 정정을 달았다.

## 5. 교훈

M12 는 **잘못된 프록시로 검증하는 실수**가 다섯 번 반복됐다고 기록했다. M13 이 보여주는 것은 그보다 앞선 층위다: **검증 항목 자체가 목록에 없으면 프록시가 옳든 그르든 상관이 없다.**

스펙은 처음부터 `wrappers.def` 와 심볼 게이트를 요구하고 있었다. 그것을 만들지 않은 채로 "PASS=73 FAIL=0" 을 보고해 왔고, 그 숫자는 존재하는 테스트에 대해서는 정확했다. 없는 테스트에 대해서는 아무 말도 하지 않았다 — 그리고 없는 테스트가 24개 심볼을 가리고 있었다.
