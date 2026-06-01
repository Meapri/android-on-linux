# Device Evidence Model

Device evidence is the rule that keeps the project honest. Source tests, host tests, and APK builds are necessary, but they cannot prove Android policy behavior, GPU driver behavior, process execution, or Surface lifecycle behavior on real devices.

## Evidence File Shape

Evidence should be stored under `docs/evidence/` as dated markdown or JSON-adjacent records. Each device run must include:

- device model,
- Android version and API level,
- GPU family,
- renderer string,
- APK path or sha256,
- report lines copied from the app,
- whether software renderers were rejected,
- whether vendor-private driver bundles or device nodes were used,
- screenshots or logs when available.

## GPU Family Buckets

The minimum broad-claim buckets are:

- `adreno`,
- `mali_immortalis`,
- `tensor_class`.

`xclipse`, `powervr`, desktop Android, and emulator/gfxstream evidence are useful additions, but they do not replace the three minimum buckets.

## Required PASS Lines

For manufacturer-independent GUI/GPU acceleration, every counted device must report:

```text
HOST GPU EGL/GLES EXECUTION: PASS
HOST GPU SURFACE EXECUTION: PASS
GUEST GPU IPC BRIDGE EXECUTION: PASS
GUEST GUI GPU SURFACE EXECUTION: PASS
```

GLES shim and Vulkan gates are narrower claims:

```text
GUEST GLES SHIM SMOKE EXECUTION: PASS
GUEST GLES CLEAR VIA SHIM: PASS
GUEST GLES HARDWARE RENDER: PASS
ANDROID HOST VULKAN SURFACE EXECUTION: PASS
ANDROID HOST VULKAN SURFACE PROBE EXECUTION: PASS
host vulkan surface hardware render=true
guest vulkan clear present hardware render=true
GUEST VULKAN ICD SMOKE EXECUTION: PASS
GUEST VULKAN CLEAR PRESENT EXECUTION: PASS
```

## Claim Policy

- One device can prove a prototype, not manufacturer independence.
- Emulator/gfxstream can prove protocol shape, not physical-device coverage.
- Software renderers are diagnostic only; a software renderer means the GPU claim fails even if frames appear.
- Any use of vendor-private bundles or private Linux GPU nodes makes the result device-specific.
- A `KNOWN_FAIL` can be useful evidence, but it cannot satisfy a PASS gate.
