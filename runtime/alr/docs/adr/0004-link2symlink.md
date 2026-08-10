# ADR 0004 — link2symlink 에뮬레이션

## Status

Accepted, **무조건** (2026-08-03 개정. 원래는 조건부였다).

> 원래 Status 는 "`alr doctor` P6 이 디바이스에서 `link(2)` 성공을 보고하면 이 계층 전체를 끈다" 였다. **그 조건은 지원 범위 안에서 충족될 수 없다** — P6 은 참조 기기 2대 모두에서 `EACCES` 였고([RISKS R10](../RISKS.md), [M19](../evidence/2026-08-03-m19-snapdragon.md)), [ADR 0007](0007-android-16-only.md) 로 다른 Android 릴리스는 범위 밖이 되었다. 하드링크 폴백은 **상시 켜짐**이다.
>
> ⚠️ 그리고 여기서 말한 런타임 스위치(`state/<name>/doctor.json`)는 **구현된 적이 없다** — 코드에서 그 파일을 읽거나 쓰는 곳이 0건이다. 없는 스위치를 조건으로 걸어 두고 있었다. [ADR 0007 §2](0007-android-16-only.md) 참조.

## Context

같은 앱-데이터 디렉토리 안 두 파일에 대한 `link(2)`가 **`EACCES`로 실패**한다 (Android 10~16 + main, SELinux AVC로 확인). ext4/f2fs 무관, `fs.protected_hardlinks` 무관.

**errno가 `EACCES`이지 `EPERM`/`EXDEV`가 아니라는 점이 중요하다** — 게스트의 폴백 코드는 대개 `EXDEV`나 `EPERM`만 잡으므로 발동하지 않고, 그냥 실패한다.

깨지는 것:
- `dpkg -i` — 하드링크 멤버를 가진 tar, 그리고 `status`→`status-old` 원자적 백업
- `git clone --local` — 객체를 기본으로 하드링크
- `pnpm` — 콘텐츠 주소 저장소 전체가 하드링크

**스톡 Ubuntu rootfs를 쓰기로 한 결정의 직접적 대가다.** Termux가 패치한 dpkg를 쓰는 게 아니므로 이 문제를 우리가 풀어야 한다.

## Decision

> ⚠️ **이 절은 2026-08-03 에 실측으로 개정됐다.** 아래 그림자 스킴은 **구현되지 않았고**, 실제로 출하된 것은 이 문서가 아래 §Alternatives 에서 **기각한 (A) 복사 폴백**이다. 기각 사유 중 하나(`st_nlink` 가 1로 남는다)는 그 사이 **별도 수단으로 해결**됐다. ADR 이 결정을 기록하는 문서인 이상, **결정이 뒤집혔으면 그것을 적는 것이 이 문서의 일**이다.

### 실제 구현 (현행)

`link(old, new)` 는 **먼저 진짜 `link(2)` 를 시도**하고, `EACCES`/`EPERM`/`EXDEV` 일 때만 폴백한다:

1. `copy_path(old, new)` — 내용을 복사한다
2. `lid_record(old, new)` — **링크 신원 테이블**에 짝을 기록한다
3. 이후 `stat`/`fstat` 계열이 그 테이블을 보고 **`st_nlink = 2` 를 합성**한다

즉 (A) 의 가장 큰 기각 사유였던 "`st_nlink` 가 1로 남아 dpkg 무결성 검사가 실패한다" 는 **복사를 포기하지 않고** 해결됐다 — 그림자 파일도, mmap 메타DB도, rename 도 필요 없었다. `src/preload/alr_preload.c` 의 `link()`/`linkat()` 과 `lid_*` 가 그것이다.

**여전히 성립하는 (A) 의 한계**(정직하게 남긴다): 한쪽을 수정해도 다른 쪽에 반영되지 않는다. `git clone --local` 과 `pnpm` 의 디스크 절약도 사라진다. 대상 워크로드에서 이것이 문제가 된 적은 아직 없고, 문제가 되면 그때 그림자 스킴을 다시 본다.

<details><summary>구현되지 않은 원래 설계 (기록용)</summary>

preload에 **link2symlink 에뮬레이션**을 구현한다. `alr doctor` P6 결과에 따라 런타임에 켜고 끈다.

알고리즘 (`link(old, new)`):
1. `old`가 미그림자화면: `old` → `<R>/.alr/l2s/<n>`로 **rename**, `old` 자리에 그 그림자를 가리키는 심링크 생성, 링크 카운트 = 1 기록
2. `new` 자리에 같은 그림자를 가리키는 심링크 생성, 카운트 += 1
3. 그림자 인덱스와 카운트를 mmap 메타DB에 기록

</details>

## Consequences

### 함께 인터포즈해야 하는 것 — 빠뜨리면 일관성이 깨진다

경로 재작성만으로는 **부족하다.** 다음을 함께 처리해야 dpkg의 무결성 검사와 `find -samefile`, git이 정상 동작한다:

| 함수 | 필요한 동작 |
|---|---|
| `stat`/`lstat`/`fstat`/`statx` | 그림자 심링크에 대해 `st_nlink`를 기록된 카운트로, `st_ino`/`st_dev`를 그림자의 것으로 보고. **`lstat`이 심링크가 아니라 일반 파일로 보이게** 해야 한다 (dpkg가 심링크를 다르게 취급) |
| `readlink` | 그림자 심링크에 `EINVAL` (일반 파일인 척) |
| `unlink`/`unlinkat` | 카운트 감소, 0이면 그림자 삭제 |
| `rename`/`renameat` | 그림자 참조 유지 |
| `open` | 심링크를 따라가므로 대개 동작하나 `O_NOFOLLOW` 케이스 확인 |

### 비용

- 복잡도와 버그 표면이 상당하다. 그래서 **P6이 켜줄 때만** 활성화한다.
- 메타DB 조회가 `stat` 계열 hot path에 들어간다 → [ADR 0003](0003-ld-preload-path-virtualization.md)의 성능 예산을 위협한다. **그림자 디렉토리 접두사 검사를 먼저** 해서 대부분의 `stat`이 DB를 건드리지 않게 한다.

### 필수 테스트 매트릭스

```
dpkg -i (하드링크 멤버를 가진 .deb)
git clone --local
pnpm install
find -samefile
ln a b && stat -c %h a    → 2 를 보고해야 함
ln a b && rm a && stat b  → 여전히 읽을 수 있어야 함
```

## Alternatives considered

### (A) 하드링크를 복사로 대체

가장 단순하다.

> ⚠️ **이것이 실제로 채택된 것이다** (2026-08-03 확인). 아래 기각 사유는 당시 판단이며, 첫 번째 사유는 링크 신원 테이블로 해결됐다 — §Decision 참조.

**기각(당시)**: `st_nlink`가 1로 남아 dpkg의 무결성 검사가 실패하고, `git clone --local`의 디스크 절약이 사라지며(대형 저장소에서 심각), pnpm의 저장소 모델이 무너진다. 그리고 한쪽을 수정하면 다른 쪽에 반영되지 않아 **조용한 데이터 불일치**를 만든다.

향후 `--l2s=copy` 옵션으로는 제공할 수 있다 (단순함을 원하는 사용자용).

### (B) PRoot의 `--link2symlink`(`-l`) 확장 이식

동작이 검증된 알고리즘이다. **참고하되 소스는 복사하지 않는다** ([00-product.md §6.5](../00-product.md) 클린룸 규칙). 알고리즘을 이해하고 재구현한다.

### (C) 아무것도 하지 않고 KNOWN_FAIL로 두기

**기각**: `apt install`(G2)과 `git clone --local`이 목표 워크로드다. 둘 다 깨지면 제품이 성립하지 않는다.
