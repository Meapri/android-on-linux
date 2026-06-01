# Prompt: Hermes Optional Backend and Device Evidence Workstream

Use this prompt for Hermes or another external agent doing device evidence, APK smoke, and optional low-overhead backend probing.

```text
You are working in /Users/naen/Documents/Plib/androlinux-runtime-lab.

Mission:
Collect real Android device evidence and optional low-overhead backend A/B results while preserving the clean-room boundary for the open ALR runtime.

Collaboration cadence:
- Work in large batches.
- Do not post frequent status pings.
- Do not create recurring coordination loops unless explicitly asked by the user.
- Stay on your branch until the evidence bundle is complete, then open or update one PR.
- Rebase the evidence PR on current main before review so Codex implementation files are not accidentally removed.

Read first:
- README.md
- docs/product-requirements.md
- docs/clean-room-protocol.md
- docs/android-graphics-bridge-spec.md
- docs/plans/implementation-milestones.md
- docs/plans/parallel-workstreams.md
- docs/agent-coordination.md
- docs/agent-sync.md

Clean-room rules:
- You may observe optional proroot-class binaries as black boxes.
- Record command inputs, stdout, stderr, exit codes, elapsed time, device model, Android version, and logs.
- Do not disassemble, decompile, or reconstruct proprietary runtime algorithms.
- Do not edit ALR clean-room implementation code based on proprietary behavior.
- If you find important behavior, write it as black-box evidence, not implementation instructions.

Ownership:
- You own device evidence and optional backend probe notes.
- Prefer writing under docs/evidence/ and docs/research/.
- Do not edit app/src/main/cpp/alr_runtime/ unless explicitly assigned.
- Update docs/agent-sync.md at bundle start and bundle completion only, or when a real blocker changes the plan.

Immediate target:
Implement Bundle B from docs/plans/implementation-milestones.md: Optional proroot A/B Probe and device evidence.

Deliverables:
1. Confirm current PRoot baseline on device:
   - static hello
   - shell script
   - dynamic glibc hello
   - id/fakeroot
   - dpkg/apt preflight
2. Capture GPU/GUI bridge evidence:
   - host EGL/GLES renderer
   - Surface frame count/drop/lossless status
   - guest GPU IPC bridge
   - Wayland-style and X11-style GUI smoke
3. If adding optional proroot-class backend:
   - preserve upstream filenames in an optional directory
   - record source URL, version/tag, sha256, license note
   - do not modify the binaries
   - absence should report SKIP, not FAIL
4. Run A/B commands through identical rootfs:
   - /bin/hello
   - /bin/sh -c "echo shell-c ok; /bin/hello"
   - /bin/glibc-hello
   - /usr/bin/id with fake root mode if supported
   - /usr/bin/dpkg --version
   - /usr/bin/dpkg --print-architecture
   - /usr/bin/apt --version
   - local deb install smoke if available

Expected report strings:
PROOT BACKEND EXECUTION: PASS
OPTIONAL LOW-OVERHEAD BACKEND AVAILABLE: PASS/FAIL/SKIP
OPTIONAL LOW-OVERHEAD VERSION EXECUTION: PASS/FAIL/SKIP
OPTIONAL LOW-OVERHEAD ROOTFS EXECUTION: PASS/FAIL/SKIP
OPTIONAL LOW-OVERHEAD DPKG LOCAL INSTALL: PASS/FAIL/SKIP
HOST GPU EGL/GLES EXECUTION: PASS
GUEST GUI GPU SURFACE EXECUTION: PASS or KNOWN_FAIL:<reason>

Evidence format:
Create a dated markdown file under docs/evidence/, for example:
docs/evidence/2026-05-15-device-proroot-ab-smoke.md

Include:
- device model
- Android version and SDK
- APK version/build
- git commit
- backend names
- exact commands or app report sections
- stdout/stderr summaries
- elapsed time where available
- PASS/FAIL/SKIP/KNOWN_FAIL classification
- screenshots or log paths if available

Verification:
- Run python3 -m pytest tests -q if available.
- Build APK if Android build environment is present.
- Install and run on device if available.
- If a step is not available, mark SKIP with reason.

Handoff:
When the whole evidence bundle is ready, append one final entry to docs/agent-sync.md with:
- files touched
- device evidence path
- commands/tests run
- PASS/FAIL/SKIP results
- blockers
- next recommended action for Codex
```
