# Device Evidence — drain #5: CP-4 dmabuf present device-verified (단일 통합 게이트 첫 적용)

단일 통합 게이트(§6.5)로 ws-3(CP-4: AHB→external-OES zero-copy present + M3 입력) + ws-5 merge → `git merge-tree` CLEAN → host `uvx pytest tests/ -q` **398 passed** → `merge --no-ff` → push(`c6fdb7d`). 빌드(v127, JAVA_HOME openjdk@17) → `install -r -d`(versionCode 127) → force-stop cold start. Device SM-X236N / Mali-G615.

## CP-4 (ws-3 M2 dmabuf/AHB zero-copy present): DEVICE-VERIFIED
- `ALR AHB ZEROCOPY IMPORT: PASS` — AHardwareBuffer → EGLImage → external-OES import.
- present 모델: `in-process producer thread + GL executor thread, decode→AHB-FBO then cross-context external-OES present to ANativeWindow`.
- `gtkdemo-result: rendered=true frames=12→13` — gtk3demo GUI가 ws-3 WaylandPresenter→컴포지터→present로 화면 갱신. **단일 게이트 merge 후 회귀 0.**
- external-oes self-test `pixel=0,0,0` = info-only(v114가 external-OES display를 별도 검증).

## CP-2 (glmark2): 변동 없음(예상대로)
- EGL dlopen 해결(libpthread 제거 재빌드) 유지. `eglChooseConfig() didn't return any configs` = WS-2 gfxstream shim의 EGL config/surface/GLES 구현 대기. WS-2가 ws-2 브랜치에 올리면 단일 게이트로 merge 후 재drain(DEVICE-REQ).

## 단일 통합 게이트 절차 검증 (사용자 결정 적용)
merge-tree 충돌 검수 → host pytest 게이트(398) → merge --no-ff → push → 빌드 → device drain. ws-3 CP-4가 회귀 없이 device 동작 = 게이트 정상. 이제 모든 세션은 ws-N 브랜치만 push, 통합 세션이 단독 merge([[merge-gate-single-integration]]).
