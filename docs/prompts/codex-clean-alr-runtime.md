# Prompt: Codex Clean ALR Runtime Workstream

Use this prompt for an agent implementing the open ALR runtime path.

```text
You are working in /Users/naen/Documents/Plib/androlinux-runtime-lab.

Mission:
Build the clean-room ALR execution backend for a non-root Android APK that runs Linux/glibc arm64 apps from an app-private rootfs with lower overhead than PRoot.

Read first:
- README.md
- docs/product-requirements.md
- docs/clean-room-protocol.md
- docs/alr-execution-backend-spec.md
- docs/plans/implementation-milestones.md
- docs/agent-coordination.md
- docs/agent-sync.md

Clean-room rules:
- Do not copy or reconstruct closed/proprietary runtime internals.
- Do not use proprietary disassembly/decompilation as implementation source.
- Optional proroot-class binaries are black-box benchmark/probe targets only.
- Implement from project specs, tests, public Linux/Android/glibc docs, and compatible open-source references.

Ownership:
- You own ALR specs/tests/source.
- Prefer editing app/src/main/cpp/alr_runtime/ and ALR-specific tests.
- Do not edit Hermes device evidence or optional backend artifact notes unless explicitly asked.
- Update docs/agent-sync.md at start and end of your session.

Immediate target:
Implement Bundle C from docs/plans/implementation-milestones.md: ALR Launcher Skeleton.

Deliverables:
1. Add an ALR runtime launcher skeleton build target or source stub.
2. Add launch-plan/report plumbing for ALR backend availability.
3. Add tests that assert:
   - ALR launcher/config report exists.
   - writable app-data rootfs binaries are not direct entrypoints.
   - PRoot fallback remains available.
4. Do not claim guest execution yet.

Expected report strings:
ALR RUNTIME LAUNCHER AVAILABLE: PASS
ALR RUNTIME CONFIG BUILD: PASS
ALR RUNTIME DIRECT APP-DATA EXEC POLICY: PASS

Verification:
- Run scripts/test-native-core.sh.
- Run python3 -m pytest tests -q if pytest is available.
- If pytest is missing, report SKIP and do not pretend it passed.

Handoff:
Append a final entry to docs/agent-sync.md with:
- files touched
- commands/tests run
- PASS/FAIL/SKIP results
- blockers
- next recommended action
```

