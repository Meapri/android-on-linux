# 온디바이스 개발 루프 부트스트랩

`alr doctor`를 **유효한 컨텍스트**에서 돌리기 위한 최소 설정. 한 번만 하면 된다.

## 왜 `adb shell`로는 안 되는가

Android 앱 seccomp 필터는 **zygote가 uid ≥ 10000인 앱을 fork할 때만** 설치한다. 따라서:

| 컨텍스트 | uid | SELinux | Seccomp | 측정 가능? |
|---|---|---|---|---|
| `adb shell` | 2000 (shell) | `u:r:shell:s0` | **0 (없음)** | ❌ 전부 ALLOWED로 보임 |
| `run-as com.termux` | 앱 uid | `u:r:runas_app:s0` | **0 (없음)** | ❌ 동일한 함정 |
| **Termux 세션에서 fork된 프로세스** | 앱 uid | `u:r:untrusted_app_27:s0` | **2 (필터)** | ✅ **유일하게 유효** |

`run-as`가 debug 빌드에서 동작한다고 해서 쓰면 안 된다 — zygote를 거치지 않아 필터가 없다. `alr doctor`의 P1이 이걸 감지하고 거부한다.

이 기기에서 실측 확인 (2026-08-02, SM-X236N, Android 16 / SDK 36):
```
adb shell: uid=2000(shell) context=u:r:shell:s0  Seccomp: 0  Seccomp_filters: 0
```

## 1단계 — Termux 설치 (targetSdk 28 계열이어야 함)

```bash
curl -sLO https://github.com/termux/termux-app/releases/download/v0.118.3/termux-app_v0.118.3+github-debug_sha256sums
curl -sLO "https://github.com/termux/termux-app/releases/download/v0.118.3/termux-app_v0.118.3+github-debug_arm64-v8a.apk"
shasum -a 256 -c <(grep arm64-v8a termux-app_v0.118.3+github-debug_sha256sums)
adb install -r "termux-app_v0.118.3+github-debug_arm64-v8a.apk"
```

> ⚠️ 반드시 **`termux/termux-app`** 레포에서 받는다. Play 스토어 빌드는 `termux/termux-apps`라는 **다른 레포**이고 targetSdk 37이라 [ADR 0005](../docs/adr/0005-play-store-unsupported.md)에 따라 지원하지 않는다. `sharedUserId` 때문에 한 기기의 모든 Termux APK는 한 출처에서 와야 한다.

### ⚠️ Play Protect가 이 설치를 차단한다 — 기기에서 직접 눌러야 한다

GMS가 있는 기기에서 `adb install`은 **완료되지 않고** 아래 대화상자에서 멈춘다 (Android 16 / SM-X236N 실측):

```
Google Play 프로텍트
안전하지 않은 앱 차단됨 — Termux
이 앱은 Android 이전 버전에 맞게 개발되었으며 최신 개인 정보 보호 기능을 포함하지 않습니다.
[세부정보 더보기]  [확인]
```

targetSdk 28이라서 나오는 경고다. **`확인`을 누르면 설치가 취소된다.** 진행하려면 기기 화면에서:

**`세부정보 더보기` → `무시하고 설치`**

`adb install`은 그동안 대기 상태로 있다가, 승인하면 완료된다. 설치 확인:

```bash
adb shell pm path com.termux    # 비어 있으면 아직 미설치
```

> 이건 `alr`이 고칠 수 있는 문제가 아니다 (Termux 앱의 targetSdk에 달렸다). 최종 사용자 설치 안내에 반드시 포함해야 한다 — [RISKS.md R2b](../docs/RISKS.md).
>
> Play Protect 검증을 adb로 끄는 방법이 있지만 **보안 설정 변경**이므로 사용자가 스스로 판단할 일이다. 자동화가 필요하면 그 트레이드오프를 명시적으로 승인받고 진행한다.

## 2단계 — 첫 실행 (부트스트랩 압축 해제)

앱을 한 번 띄워 `$PREFIX`가 만들어지게 한다.

```bash
adb shell monkey -p com.termux -c android.intent.category.LAUNCHER 1
```

터미널이 프롬프트를 띄울 때까지 기다린다 (부트스트랩 해제에 수십 초).

## 3단계 — sshd 기동 (여기서부터 스크립트 가능)

Termux 터미널에 직접 입력하거나, `adb shell input`으로 타이핑한다:

```
pkg install -y openssh && passwd && sshd
```

`input`으로 넣을 때 (따옴표·공백 주의):

```bash
adb shell input text 'pkg%sinstall%s-y%sopenssh%s&&%ssshd'   # %s = 공백
adb shell input keyevent 66                                   # Enter
```

## 4단계 — 개발 머신에서 접속

```bash
adb forward tcp:8022 tcp:8022
ssh -p 8022 localhost
```

이 ssh 세션은 Termux 앱이 fork한 sshd의 자식이므로 **seccomp 필터를 상속**한다 → 측정 유효.

확인:
```bash
ssh -p 8022 localhost 'id; grep Seccomp /proc/self/status; cat /proc/self/attr/current'
```
`uid≥10000`, `Seccomp: 2`, `u:r:untrusted_app_27:s0` 세 가지가 모두 나와야 한다.

## 5단계 — doctor 빌드 및 실행

```bash
scp -P 8022 src/cli/doctor.c localhost:~/
ssh -p 8022 localhost 'pkg install -y clang && clang -O1 -o alr-doctor doctor.c && ./alr-doctor'
```

`alr doctor`는 마지막에 이 기기의 **SIGSYS 에뮬레이션 테이블을 C 소스로 출력**한다. 그것을 `src/supervisor/alr_sigsys_table.h`에 붙여 넣는다.

> 온디바이스 clang은 **이너 루프 전용**이다. 릴리스 아티팩트는 NDK 29 / `--target=aarch64-linux-android24`로 크로스 빌드한다 ([01-platform-facts.md §F1](../docs/01-platform-facts.md)).
