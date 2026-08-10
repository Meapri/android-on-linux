# M18 — codex 설정은 쓰이지만 읽히는지는 모른다, 그리고 잘림을 부재로 읽은 네 번째

- **날짜**: 2026-08-03
- **기기**: MediaTek MT8775, Android 16, `uid=10297 Seccomp=2`
- **대상**: RISKS R8 (`alr` 이 쓰는 codex 샌드박스 설정이 실제로 효력이 있는가)

## 1. alr 이 하는 일

`src/cli/alr.c` `with_codex()` 는 `<R>/root/.codex/config.toml` 에 228 바이트를 쓴다:

```toml
# alr: Codex's Linux sandbox relies on Landlock and bubblewrap,
# neither of which functions inside an Android app process.
# Confirm the exact mode name with `codex --help` for your version.
sandbox_mode = "danger-full-access"
```

파일은 **정상적으로 생성된다** — `--with codex` 로 설치한 rootfs 에서 확인했다.

## 2. 음성 대조 — 전부 무반응

파일을 세 가지로 망가뜨리고 codex 를 돌렸다:

| 조작 | codex 출력 |
|---|---|
| 완전히 깨진 TOML (`=== not toml === [[[`) | `codex-cli 0.146.0` |
| 파일 삭제 | `codex-cli 0.146.0` |
| 존재하지 않는 모드 값 (`this-mode-does-not-exist`) | `codex-cli 0.146.0` |

전부 정상 실행과 **글자 하나 다르지 않다.**

## 3. 그래도 "읽지 않는다" 는 증명이 아니다

`codex --version` 이 config 를 파싱하기 전에 끝나는 경로일 수 있다. 이 저장소에 정확히 그 선례가 있다 — php 는 `-h` 는 정상이고 `--version` 은 abort 했다([M14 §2](2026-08-03-m14-ioctl-php.md)). 옵션 하나가 코드 경로 전체를 가른다.

config 를 실제로 소비하는 codex 명령은 인증과 네트워크를 요구해서 이 하네스로는 돌릴 수 없다.

**정황은 강하다.** codex 는 정적 musl 이라 `LD_PRELOAD` 가 닿지 않고([M12 §8](2026-08-03-m12-spawn-resolver.md)), 따라서 `~/.codex` 를 **Android 루트 기준**으로 푼다. 그리고 Android 에는 `/root` 가 없다:

```
$ ls -ld /root
ls: cannot access '/root': No such file or directory
```

시작할 때마다 나오는 `could not create PATH aliases: Read-only file system (os error 30)` 도 같은 방향이다.

## 4. 그래서 출력 문구를 고쳤다

`alr` 은 설치 때 이렇게 단정하고 있었다:

```
alr: NOTE codex sandbox disabled; alr is not a security boundary
```

**"disabled" 는 우리가 아는 것보다 많은 주장이다.** 우리가 아는 것은 "파일을 썼다" 뿐이고, 그 파일이 효력이 있는지는 확인하지 못했다. 지금은 이렇게 말한다:

```
alr: NOTE wrote <R>/root/.codex/config.toml requesting a disabled
     sandbox. codex is statically linked, so alr cannot confirm it
     reads that file -- treat the guest as UNSANDBOXED either way.
     alr is not a security boundary.
```

사용자에게 실질적으로 중요한 결론(샌드박스 없다고 가정하라)은 유지하면서, 근거 없는 부분만 뺐다.

## 5. 잘림을 부재로 읽었다 — 이 세션 네 번째

조사 중간에 `ls -la .../root/.codex/ | head -3` 을 보고 **디렉토리가 비었다** 고 결론지었다. 그 위에서 "`fopen` 이 조용히 실패하는 버그" 라는 가설을 세우고 원인을 찾기 시작했다.

`head -3` 이 `total` · `.` · `..` 까지만 보여 주고 **네 번째 줄의 `config.toml` 을 잘랐다.** 파일은 처음부터 있었다.

같은 계열의 실수가 이 세션에서 네 번이다:

| | 잘못 읽은 "없음" | 실제 |
|---|---|---|
| M14 | 심볼을 빼니 php 가 고쳐짐 | 무관한 심볼을 빼도 재발 — 레이아웃 민감 |
| M15 | `/proc` 접근 0건 | 트레이스가 배포되지 않은 빌드 |
| M16 | `logcat -b events` 0줄 | 버퍼 읽기 권한 없음 |
| M18 | `ls \| head -3` 에 파일 없음 | 네 번째 줄에 있었음 |

앞의 셋은 "계측기가 작동하는지 먼저 보라" 로 정리했다. M18 은 그보다 단순한 층이다: **출력을 자르는 명령의 결과를 부재의 증거로 쓰지 말 것.** `head`·`grep -c`·`| wc -l` 은 전부 같은 함정이고, 이 저장소는 이미 `grep -c` 로 한 번 당했다([M12 §8](2026-08-03-m12-spawn-resolver.md) 의 주석).
