# ADR 0005 — Play Store Termux 미지원 (v1)

## Status

Accepted (설계 단계, 2026-08-02).

## Context

Termux는 두 개의 서로 다른 코드베이스로 배포된다.

| | F-Droid / GitHub (`termux-app`) | Play Store (`termux-apps`) |
|---|---|---|
| `targetSdkVersion` | **28** | **37** |
| `minSdkVersion` | 21 | 30 |
| SELinux 도메인 | `untrusted_app_27` | `untrusted_app` |
| `app_data_file` execve | **허용** | **거부** (`execute_no_trans` 없음) |
| 서명 키 | F-Droid | Google Play (다름) |

`sharedUserId` 때문에 한 디바이스의 모든 Termux APK는 한 출처에서 와야 한다.

Play 빌드는 termux-exec의 `system_linker_exec` 모드로 우회한다: `execve(<elf>, ...)`를 `execve("/system/bin/linker64", [<elf>, args...])`로 재작성한다. Android ≥ 10에서 시스템 링커(`system_linker_exec` 타입)는 절대 경로의 실행 파일을 인자로 받을 수 있어, 커널과 SELinux는 링커만 실행되는 것으로 본다.

## Decision

**v1에서 Play Store 빌드를 지원하지 않는다.** 시작 시 감지하고 명확한 메시지로 거부한다.

## Rationale

**`/system/bin/linker64`는 bionic 링커라 glibc 프로그램을 로드할 수 없다.** termux-exec의 트릭은 bionic 바이너리에만 통한다. glibc 게스트에는 원리적으로 적용되지 않는다.

이론적으로는 경로가 하나 있다:
```
execve("/system/bin/linker64", ["linker64", "<app-data의 bionic 트램폴린>", ...])
  → 진짜 커널 execve, fresh 주소공간
  → 그 트램폴린이 유저스페이스 ELF 로더로 ld-linux-aarch64.so.1 을 PROT_EXEC mmap
    (§B3에 따라 file-backed PROT_EXEC mmap 은 허용된다)
  → 합성한 argc/argv/envp/auxv 로 ld.so entry 에 점프
```

**기각 이유**:
- 구현 선례가 없다.
- bionic과 glibc가 한 주소공간에 공존하며 `TPIDR_EL0`(TLS 포인터)을 다툰다. 상위 프로젝트가 이 문제로 `rt_sigaction`을 raw syscall로 재설정해야 했던 기록이 있다 (ART의 libsigchain이 TPIDR_EL0 이동 후 bionic TLS를 역참조하려 함).
- 자체 TLS, atfork 핸들러, 시그널 핸들러 스토리가 전부 필요하다.
- **exec 단위 프로세스 의미론을 포기**해야 한다 — [ADR 0001](0001-signal-only-ptrace-supervisor.md)의 옵션 (A)와 같은 비용에 더해 bionic/glibc 경계 문제가 추가된다.

이것은 설정 플래그가 아니라 **별개 프로젝트**다.

## Consequences

- README와 `alr doctor`가 명확히 안내한다.
- 감지: `$PREFIX/lib/libtermux-exec-linker-ld-preload.so` 존재 확인이 1차, `$PREFIX`의 스크래치 ELF execve가 `EACCES`인지 확인이 확정적 판정.
- **조용한 버그 리포트 채널이 되지 않게 한다** — 감지 없이 실패하면 사용자가 원인을 알 수 없다.

에러 메시지 ([06-cli-spec.md §7](../06-cli-spec.md)):
```
alr: Play Store 버전 Termux는 지원하지 않습니다
  reason=unsupported-android-policy
  이 Termux는 targetSdk >= 29 라 앱 데이터 경로의 실행이 SELinux에 의해 거부됩니다.
  F-Droid 또는 GitHub 릴리스 버전을 설치하세요: https://github.com/termux/termux-app/releases
```

## 관련 위협 — F-Droid 빌드도 영원하지 않다

`untrusted_app_27`의 exec 허용은 Android 16과 AOSP main까지 **깨지지 않는다.** 위협 모델은 "Android가 SELinux 규칙을 지운다"가 아니라 **"Android가 `MIN_INSTALLABLE_TARGET_SDK`를 28 위로 올린다"**이다. 릴리스당 +1씩 올라왔으므로(23→24) 29까지는 수년 남았다.

**대응**: 마이그레이션 계획을 특정 Android 버전에 걸지 않는다. 대신 첫 실행 때 스크래치 ELF execve를 프로브하고, `EACCES`가 나오면 **크게 실패**한다 (rootfs를 망가뜨리는 대신).
