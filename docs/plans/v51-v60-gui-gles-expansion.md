# V51–V60 GUI/GLES Expansion Plan

This plan starts after the V40 clean GUI GPU proof is device-verified. It is intentionally a planning/test guardrail document only: it must not require changes to the current V40 runtime code path.

## Scope

V51–V60 expands the proven Android Surface/EGL/GLES bridge from synthetic Wayland/X11-style frame smoke tests into two concrete tracks:

- V51–V55: shape a minimal Wayland/X11 proxy boundary that can accept simple guest GUI app frame commits and translate them into Android host compositor commands.
- V56–V60: expand the guest-side EGL/GLES shim enough for a tiny Linux GL clear/swap path to render through the Android host Surface.

## Guardrails

- This is not full compositor/server compatibility yet.
- V51–V60 must not claim full Wayland compositor compatibility.
- V51–V60 must not claim full X11 server compatibility.
- V51–V60 must not claim broad desktop environment, Chromium, Wine, DXVK, GLX, or Vulkan compatibility.
- Android Surface/EGL hardware proof is required before any GUI/GLES acceptance is marked PASS.
- Renderer evidence must reject software renderers such as SwiftShader, llvmpipe, and softpipe.
- Device logs remain the source of truth; an APK build or host pytest result alone is not acceleration proof.
- Keep the V40 runtime proof stable while this plan is implemented; do not modify current V40 runtime code unless a later implementation task explicitly scopes that change.

## V51–V55: Wayland/X11 Proxy Shape

Goal: Replace raw synthetic GUI frame smoke with a minimal protocol-shaped ingress layer while preserving the proven Android host GPU Surface renderer.

### V51: Guest display endpoint contract

Implementation notes:

- Provide a guest `WAYLAND_DISPLAY` endpoint for Wayland-shaped clients.
- Provide a guest `DISPLAY` endpoint for X11-shaped clients.
- The endpoints may be minimal proxy sockets; they are not a complete compositor or X server.
- Log endpoint paths, connection attempts, protocol family, and accepted client count.

Acceptance lines:

```text
WAYLAND DISPLAY SOCKET AVAILABLE: PASS
X11 DISPLAY SOCKET AVAILABLE: PASS
GUI PROXY ENDPOINTS ARE MINIMAL SHAPE ONLY: PASS
NOT FULL COMPOSITOR/SERVER COMPATIBILITY YET: PASS
```

### V52: Minimal Wayland message/frame shape

Implementation notes:

- Accept a constrained Wayland-like object/message envelope sufficient for a tiny test client.
- Translate commit-like events into the existing host GUI frame command format.
- Preserve frame sequence IDs and report gaps, duplicates, and out-of-order delivery.

Acceptance lines:

```text
WAYLAND PROXY MINIMAL MESSAGE SHAPE: PASS
WAYLAND FRAME COMMIT TRANSLATED TO HOST SURFACE COMMAND: PASS
WAYLAND PROXY SEQ LOSSLESS: PASS
ANDROID SURFACE/EGL HARDWARE PROOF REQUIRED: PASS
```

### V53: Minimal X11 message/frame shape

Implementation notes:

- Accept a constrained X11-like connection/update envelope sufficient for a tiny test client.
- Translate drawable/window update events into the existing host GUI frame command format.
- Keep X11 proxy work limited to frame/update extraction; do not claim full X server behavior.

Acceptance lines:

```text
X11 PROXY MINIMAL MESSAGE SHAPE: PASS
X11 DRAWABLE UPDATE TRANSLATED TO HOST SURFACE COMMAND: PASS
X11 PROXY SEQ LOSSLESS: PASS
ANDROID SURFACE/EGL HARDWARE PROOF REQUIRED: PASS
```

### V54: Host compositor command path

Implementation notes:

- Feed Wayland and X11 proxy outputs into the Android host Surface renderer.
- Report per-protocol submitted/rendered/dropped frame counts.
- Report the actual Android GL renderer string.

Acceptance lines:

```text
GUEST GUI APP FRAME COMMIT EXECUTION: PASS
ANDROID GPU SURFACE COMPOSITOR EXECUTION: PASS
SURFACE WAYLAND PROXY FRAMES RENDERED: PASS
SURFACE X11 PROXY FRAMES RENDERED: PASS
SURFACE GUI PROXY FRAMES DROPPED: 0
ANDROID SURFACE/EGL HARDWARE RENDERER: PASS
```

### V55: GUI proxy bundle delivery

Implementation notes:

- Build and device-test a V55 APK only after V51–V54 evidence is present.
- Keep V40 clean GUI proof tests passing.
- Document any protocol messages intentionally not implemented.

Acceptance lines:

```text
V55 GUI PROXY SHAPE APK BUILT: PASS
V40 GUI GPU PROOF REGRESSION: PASS
WAYLAND/X11 PROXY SHAPE DEVICE EVIDENCE COLLECTED: PASS
NOT FULL COMPOSITOR/SERVER COMPATIBILITY YET: PASS
```

## V56–V60: EGL/GLES Shim Expansion

Goal: Make a small Linux GL app use guest-side `libEGL`/`libGLES` shim entry points that forward a known, constrained command subset to the Android host Surface/EGL/GLES renderer.

### Required GLES shim subset

The V56–V60 shim subset is deliberately small and must be listed in tests/docs:

- `eglGetDisplay`
- `eglInitialize`
- `eglChooseConfig`
- `eglCreateContext`
- `eglMakeCurrent`
- `glViewport`
- `glClearColor`
- `glClear`
- `eglSwapBuffers`
- `eglDestroyContext`
- `eglTerminate`

Out of scope for V56–V60:

- Full EGL ABI compatibility.
- Full GLES2/GLES3 command coverage.
- GLX compatibility.
- Vulkan ICD/proxy compatibility.
- Zero-copy buffer transport.
- Real desktop compositor compatibility.

### V56: Shim library/export audit

Implementation notes:

- Package guest-visible `libEGL`/`libGLES` shim names or loader redirects.
- Verify the required subset symbols are exported or deliberately routed.
- Ensure the shim reports its constrained subset clearly.

Acceptance lines:

```text
GUEST EGL/GLES SHIM LIBRARIES VISIBLE: PASS
GLES SHIM REQUIRED SUBSET LISTED: PASS
GLES SHIM SCOPE IS CONSTRAINED: PASS
```

### V57: EGL lifecycle forwarding

Implementation notes:

- Forward `eglGetDisplay`, `eglInitialize`, `eglChooseConfig`, `eglCreateContext`, and `eglMakeCurrent` to the host renderer session model.
- Return enough EGL-like status for the tiny test app to proceed.

Acceptance lines:

```text
GUEST EGL INIT VIA SHIM: PASS
GUEST EGL CONTEXT VIA SHIM: PASS
ANDROID SURFACE/EGL HARDWARE PROOF REQUIRED: PASS
```

### V58: GLES clear command forwarding

Implementation notes:

- Forward `glViewport`, `glClearColor`, and `glClear` from the guest shim to Android host GLES.
- Render a deterministic clear color and report the command sequence.

Acceptance lines:

```text
GUEST GLES VIEWPORT VIA SHIM: PASS
GUEST GLES CLEAR VIA SHIM: PASS
GUEST GLES COMMAND SEQUENCE LOSSLESS: PASS
```

### V59: Swap/present and hardware evidence

Implementation notes:

- Forward `eglSwapBuffers` into the Android Surface present path.
- Report frames submitted, rendered, dropped, renderer string, and software-renderer rejection.

Acceptance lines:

```text
GUEST EGL SWAP VIA ANDROID SURFACE: PASS
GUEST GLES HARDWARE RENDER: PASS
ANDROID SURFACE/EGL HARDWARE RENDERER: PASS
SOFTWARE GLES RENDERER REJECTED: PASS
SURFACE GLES SHIM FRAMES DROPPED: 0
```

### V60: GLES shim bundle delivery

Implementation notes:

- Build and device-test a V60 APK only after V56–V59 evidence is present.
- Keep V40 and V55 regression checks passing.
- Treat the shim as a proof subset, not a general GL compatibility layer.

Acceptance lines:

```text
V60 GLES SHIM EXPANSION APK BUILT: PASS
V40 GUI GPU PROOF REGRESSION: PASS
V55 GUI PROXY SHAPE REGRESSION: PASS
GLES SHIM SUBSET DEVICE EVIDENCE COLLECTED: PASS
NOT FULL EGL/GLES COMPATIBILITY YET: PASS
```

## Evidence Required Before PASS Claims

Each device PASS claim for this bundle must include:

```text
android surface available=true
android egl initialized=true
android gl renderer=<non-software renderer string>
software renderer rejected=true
surface frames submitted=<n>
surface frames rendered=<n>
surface frames dropped=0
```

Do not mark V51–V60 complete unless Android Surface/EGL hardware proof is present in the same evidence bundle as the guest proxy/shim result.
