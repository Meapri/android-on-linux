# Parallel Workstreams

## Goal

Run two agents in parallel without contaminating the clean-room implementation or fighting over the same files.

## Workstream A: Clean ALR Runtime

Recommended agent:

- Codex

Mission:

- Turn the ALR execution backend specification into tests and implementation.
- Keep implementation clean-room.
- Preserve PRoot fallback.

Primary docs:

- `docs/product-requirements.md`
- `docs/clean-room-protocol.md`
- `docs/alr-execution-backend-spec.md`
- `docs/plans/implementation-milestones.md`

Write scope:

- `app/src/main/cpp/alr_runtime/`
- future `app/src/main/cpp/alr_runtime_launcher*`
- future `app/src/main/cpp/alr_runtime_hook*`
- ALR-specific host/native tests
- ALR-specific documentation updates

Do not touch by default:

- Optional proprietary backend artifacts.
- Device evidence logs.
- Hermes-owned smoke reports.

Current large-batch assignment:

1. Own the next ALR implementation bundle end to end on a Codex branch.
2. Add source, host/native tests, Android packaging coverage, and report strings in the same PR.
3. Keep guest execution claims conservative until a real guest process path is verified.
4. Hand off to Hermes only at PR-ready or merged-bundle boundaries.

Recent completed acceptance:

```text
ALR RUNTIME LAUNCHER AVAILABLE: PASS
ALR RUNTIME CONFIG BUILD: PASS
ALR RUNTIME DIRECT APP-DATA EXEC POLICY: PASS
ALR HOOK LOAD: PASS
ALR HOOK CONFIG BUILD: PASS
ALR STAT ROOTFS FILE: PASS
ALR OPEN ROOTFS FILE: PASS
```

## Workstream B: Device Evidence and Optional Backend Probe

Recommended agent:

- Hermes

Mission:

- Keep device evidence real.
- Compare PRoot with optional proroot-class backend through black-box behavior only.
- Maintain GPU/GUI proof reports.

Primary docs:

- `docs/clean-room-protocol.md`
- `docs/android-graphics-bridge-spec.md`
- `docs/plans/implementation-milestones.md`
- `docs/plans/2026-05-15-zero-overhead-linux-gpu-runtime-roadmap.md`

Write scope:

- `docs/evidence/`
- `docs/research/`
- optional backend metadata docs
- APK smoke result notes
- narrow Android report fixes only when assigned

Do not touch by default:

- `app/src/main/cpp/alr_runtime/`
- ALR clean-room implementation files.
- Clean-room specs except by proposing changes in `docs/agent-sync.md`.

Current large-batch assignment:

1. Rebase or recreate the evidence PR on current `main` so implementation files are never deleted by an old baseline.
2. Own one complete device/evidence bundle at a time.
3. Capture PRoot baseline, optional backend status, and GUI/GPU bridge evidence in one dated evidence PR.
4. Include all current Codex report strings in the same evidence pass.
5. Avoid recurring status comments unless blocked or ready for review.

Acceptance for first bundle:

```text
PROOT BACKEND EXECUTION: PASS
OPTIONAL LOW-OVERHEAD BACKEND AVAILABLE: PASS/FAIL/SKIP
OPTIONAL LOW-OVERHEAD ROOTFS EXECUTION: PASS/FAIL/SKIP
OPTIONAL LOW-OVERHEAD DPKG LOCAL INSTALL: PASS/FAIL/SKIP
HOST GPU EGL/GLES EXECUTION: PASS
GUEST GUI GPU SURFACE EXECUTION: PASS or known-fail with evidence
```

## Shared Contract

Both workstreams must preserve:

- Safe rootfs extraction.
- No direct writable app-data executable entrypoint policy.
- Clean guest environment.
- PRoot fallback.
- Deterministic report strings.
- Clean-room boundary.

## Merge Order

Prefer large complete bundles, not small alternating sync commits:

1. Codex implementation bundle with tests and APK build proof.
2. Hermes evidence bundle rebased on current `main`.
3. Integration review for clean-room boundaries and accidental deletions.
4. Merge the complete bundle.
5. Open the next large bundle.

## Cross-Agent Questions

Questions should be written into `docs/agent-sync.md` as:

```markdown
Question for <agent>:
- <specific question>

Needed by:
- <date or milestone>

Why it matters:
- <one sentence>
```

The other agent answers in a new entry. This is slower than live chat, but far less fragile.

Use this only for blockers. Normal progress should stay inside the agent's branch and PR until the bundle is ready.
