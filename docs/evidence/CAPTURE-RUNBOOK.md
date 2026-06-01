<!-- WS-5 device-evidence CAPTURE RUNBOOK.
Short, ordered checklist for safely capturing evidence on the SHARED physical device.
Read this before touching the device. -->

# 디바이스 증거 캡처 런북 (Device Evidence Capture Runbook)

대상 디바이스 / Target device: adb serial `R5KL20B6S3X` (SM-X236N).
applicationId: `dev.chanwoo.androlinux` (app/build.gradle.kts에서 확인 / verified in build.gradle.kts).

> 이 디바이스는 5개 세션이 공유합니다. WS-5는 자기 worktree에서 APK를 빌드/설치하지
> 않습니다(다른 세션의 디바이스 테스트를 덮어쓰게 됨). WS-5 evidence는 항상
> **머지된 통합(integration) 빌드**에 대해 캡처합니다.
>
> This device is SHARED across 5 sessions. WS-5 does NOT build or install an APK from
> its own worktree (that would clobber other sessions' device tests). WS-5 captures
> evidence only against a MERGED integration build.

## 0. 소유권 조율 / Coordinate ownership (FIRST)
- [ ] 디바이스를 만지기 전에 현재 누가 점유 중인지 확인하고 소유권을 넘겨받는다.
      Confirm no other session currently owns the device; claim ownership before touching it.
- [ ] WS-5 worktree에서 절대 `git push`/`adb install`/APK 빌드를 하지 않는다.
      Never push, install an APK, or build from the WS-5 worktree.

## 1. 디바이스 연결 확인 / Verify device
- [ ] `adb -s R5KL20B6S3X get-state` → `device` 인지 확인.

## 2. 콜드 스타트 / Force-stop then cold-start (REQUIRED)
warm resume는 `onCreate`(오버레이+프로브)를 건너뛰므로 반드시 force-stop 후 콜드 스타트.
A warm resume skips `onCreate` (overlays + probes), so always force-stop first.
- [ ] `adb -s R5KL20B6S3X shell am force-stop dev.chanwoo.androlinux`
- [ ] `adb -s R5KL20B6S3X shell logcat -c`  (이전 로그 비우기 / clear old logs)
- [ ] `adb -s R5KL20B6S3X shell monkey -p dev.chanwoo.androlinux -c android.intent.category.LAUNCHER 1`
      (또는 `am start -n dev.chanwoo.androlinux/.MainActivity`)
- [ ] 앱이 프로브를 실행할 시간을 준다 (몇 초 대기). Let the app finish its probes.

## 3. 리포트 캡처 / Capture the report
앱 태그로 필터링한 logcat 덤프를 파일로 저장.
Save the app-tag-filtered logcat dump to a file.
- [ ] `adb -s R5KL20B6S3X logcat -d | grep -i androlinux > /tmp/alr-report-$(date +%Y%m%d).txt`
      (앱 로그 태그에 맞춰 grep 패턴 조정. Adjust the grep tag to the app's log tag.)
- [ ] 저장한 파일에 `build:`, `gimp-probe guest=... exit=...` 줄들,
      `all: pcgate=1 interpose=1 traps=0 rewrites=0` 가 포함됐는지 눈으로 확인.

## 4. 회귀 게이트 실행 / Run the regression gate
worktree 루트에서 실행하고 출력을 그대로 붙여넣는다.
Run from the worktree root and paste the output verbatim.
- [ ] `cd /Users/naen/Documents/alr-ws5 && python -m bench gate /tmp/alr-report-YYYYMMDD.txt`
- [ ] exit code 0 = PASS, 1 = FAIL. 출력 markdown을 증거 파일에 붙여넣는다.

## 5. 증거 파일 작성 / Fill the evidence template
- [ ] `docs/evidence/_TEMPLATE.md` 를 새 날짜 파일로 복사:
      `docs/evidence/YYYY-MM-DD-device-SM-X236N-<short-slug>.md`
- [ ] 실제 리포트 줄(붙여넣기, 의역 금지), §3의 캡처 결과, §4의 게이트 출력,
      해당되면 `python -m bench overhead ...` 결과를 채운다.
      Fill REAL report lines (paste, do not paraphrase), the gate output, and the
      overhead result if applicable.
- [ ] Honest scope 섹션에 증명하지 못한 것/알려진 벽(syscall-storm ptrace 등)을 적는다.
