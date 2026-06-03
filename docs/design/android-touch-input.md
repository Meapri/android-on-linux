# Design: Real Android touch -> guest `wl_touch` (stop synthesizing mouse)

Status: DESIGN (host-only, not device-verified). Branch `auto/host-touch`.
Owner-to-wire: WS-3 owns `app/src/main/cpp/alr_wayland/alr_compositor.cpp`; the MAIN
session owns `app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt` and the JNI
bridge in `app/src/main/cpp/runtime_report.cpp`. This doc is the spec; the precise
minimal edits each owner makes are in **§7 ownerWiring**.

---

## 1. Symptom & root cause ("feels like a mouse click, not touch")

Android touch is delivered to guests today, and apps *do* react — but it behaves like
a single mouse cursor: multitouch collapses to one point, two-finger gestures don't
work, and toolkits route everything through their pointer/hover code path instead of
their touch path. The cause is **NOT** missing protocol. The compositor already:

- advertises `WL_SEAT_CAPABILITY_TOUCH` on the seat
  (`alr_compositor.cpp` `Compositor::bind_seat`, ~line 1792), and
- implements the full `wl_touch` stream with correct grab semantics: `TouchDown` /
  `TouchMotion` / `TouchUp` / `TouchFrame` in `drain_input_queue`
  (`alr_compositor.cpp` ~lines 2147-2202), per-sequence implicit grab via
  `g_touch_target` / `g_touch_active`, stable `touch_id`, hit-testing at `down`, and
  `wl_touch.cancel` on surface teardown (`cancel_touch_if_targeting`, ~line 400).

The defect is in **two thin layers above the compositor**:

### 1a. JNI co-injects a synthetic mouse on EVERY touch (primary bug)

`runtime_report.cpp` `Java_..._nativeWaylandInjectTouch` (~line 6592) does, for every
single touch event:

```cpp
alr::wayland::alr_wayland_inject_pointer_motion(x, y);          // <-- always
if (phase == 0) alr_wayland_inject_pointer_button(0x110, 1);    // BTN_LEFT down
else if (phase == 2) alr_wayland_inject_pointer_button(0x110, 0); // BTN_LEFT up
alr::wayland::alr_wayland_inject_touch(id, x, y, phase);        // the real touch
```

So for a 2-finger gesture, BOTH fingers' moves are also fed as `wl_pointer.motion` to
the *same* single pointer — the pointer teleports between fingers, and a `BTN_LEFT`
press/release fires for each finger's down/up. Toolkits that bind both `wl_pointer`
and `wl_touch` (GTK, Qt, SDL2, Chromium/Ozone) generally **prefer the pointer stream
for hit-testing/hover and selection**, and the touch stream is contradicted by a
pointer that is simultaneously somewhere else. Result: it "feels like a mouse." The
synthetic pointer also makes `id` (the second finger's pointer-id) irrelevant because
the pointer has no concept of multiple contacts.

### 1b. MainActivity reports only ONE pointer per MotionEvent (multitouch loss)

`MainActivity.kt` `setOnTouchListener` (~line 1253) injects exactly one contact per
callback:

```kotlin
nativeWaylandInjectTouch(ev.getPointerId(ev.actionIndex), ev.x, ev.y, phase)
```

- `ev.actionIndex` is only meaningful for `ACTION_*_DOWN` / `ACTION_*_UP`. For
  `ACTION_MOVE` (`actionMasked == ACTION_MOVE`) `actionIndex` is **always 0**, and a
  single MOVE callback batches movement for **all** current contacts. So when two
  fingers move, only finger-0's position is forwarded; finger-1 is frozen.
- `ev.x` / `ev.y` are the coordinates of pointer index 0, not of `actionIndex`. On a
  `ACTION_POINTER_DOWN` for the 2nd finger this reports the WRONG coordinate (it sends
  finger-1's id with finger-0's position).
- `ACTION_CANCEL` is mapped to the same `phase = 2` as `UP`, so a gesture aborted by
  the system (e.g. a parent intercepting) reports a normal lift instead of a cancel.
- No `MotionEvent.getHistorical*` draining — fast drags drop intermediate samples
  (acceptable for v1, noted in §6).

**Conclusion.** The fix is entirely in 1a + 1b plus a new "touch-only" JNI entry and a
small policy for when a single finger should *also* emit pointer (so legacy
pointer-only apps still work). **No new Wayland global, no new protocol XML, no change
to the `wl_touch` grab logic.** The compositor work is: (i) add a touch entry that does
NOT co-inject pointer, (ii) add an explicit cancel injector, (iii) optionally a
single-finger pointer-emulation policy. All are small, additive C++ functions next to
the existing `alr_wayland_inject_*`.

---

## 2. Wayland protocol (already present — for reference, no change)

Global: `wl_seat` (advertised at v7, `kSeatVersion`; GTK negotiates MIN(v,5)).
Capability: `WL_SEAT_CAPABILITY_TOUCH` already OR'd in `bind_seat`.
Touch object: `wl_seat.get_touch` -> `wl_touch` (`seat_get_touch`, ~line 1651;
`kTouchImpl` = `{touch_release}`).

Events the compositor already emits (keep as-is):

| Event              | Args                                              | Emitted in |
|--------------------|---------------------------------------------------|------------|
| `wl_touch.down`    | serial, time, surface, id, x_fp, y_fp             | `TouchDown` case |
| `wl_touch.motion`  | time, id, x_fp, y_fp                              | `TouchMotion` case |
| `wl_touch.up`      | serial, time, id                                  | `TouchUp` case |
| `wl_touch.frame`   | (none)                                            | `TouchFrame` case |
| `wl_touch.cancel`  | (none)                                            | `cancel_touch_if_targeting` (teardown only today) |

`x_fp`/`y_fp` are `wl_fixed_t` surface-local coords from `map_input_to_surface`.

**Grab rule (already correct):** the surface named at `down` owns the whole sequence
for that `id`; later `motion`/`up` for that `id` stay with it regardless of where the
finger moves (`g_touch_target`). `cancel` drops all points and is the only correct way
to end a grab when no `up` will arrive.

**The one protocol gap to close:** `wl_touch.cancel` is currently reachable only from
surface-teardown. A user-initiated `ACTION_CANCEL` (gesture stolen by the system) has
no path to it. §3 adds an injector for that.

---

## 3. New compositor entry points (additive — the WS-3 edit)

Add three functions beside the existing injectors (`alr_compositor.cpp`
`alr_wayland_inject_touch`, ~line 2480) and declare them in `alr_compositor.hpp`
(beside line 148). They reuse the existing `enqueue_inject` + `InjectKind` machinery;
no new state.

### 3a. `alr_wayland_inject_touch_only` — touch WITHOUT synthetic pointer

Identical body to today's `alr_wayland_inject_touch` (enqueue `TouchDown`/`Motion`/`Up`
+ a `TouchFrame`). It exists so the JNI layer can stop co-injecting pointer. (Today's
`alr_wayland_inject_touch` already does NOT inject pointer — the pointer co-injection
lives in the JNI wrapper, §1a — so functionally `inject_touch_only` can just BE the
existing `alr_wayland_inject_touch`. We still add a clearly-named alias + a batched
variant so the JNI contract is explicit and frame-coalescing is correct, §3c.)

### 3b. `alr_wayland_inject_touch_cancel` — drive `wl_touch.cancel` from the UI

New `InjectKind::TouchCancel`. Drain case:

```cpp
case InjectKind::TouchCancel:
    for (auto* t : g_touches) {
        wl_touch_send_cancel(t);
        if (wl_resource_get_version(t) >= WL_TOUCH_FRAME_SINCE_VERSION)
            wl_touch_send_frame(t);
    }
    g_touch_target = nullptr;
    g_touch_active = 0;
    break;
```

(Per spec, `cancel` ends ALL active touch points; no per-id arg. It is followed by a
frame.) Injector:

```cpp
void alr_wayland_inject_touch_cancel() {
    InjectEvent e{}; e.kind = InjectKind::TouchCancel; e.time_ms = now_ms();
    enqueue_inject(e);
}
```

### 3c. Frame coalescing for multi-contact batches (correctness)

Today `alr_wayland_inject_touch` enqueues a `TouchFrame` after EVERY single contact.
A `wl_touch.frame` means "this atomic set of touch changes is complete" — a multi-finger
MOVE should be ONE frame covering all moved contacts, not N frames. Add a batched entry
so the JNI layer can send all contacts of one `MotionEvent` then one frame:

```cpp
// Enqueue a down/motion/up for one contact WITHOUT a trailing frame.
void alr_wayland_inject_touch_point(int32_t id, double x, double y, int32_t phase) {
    InjectEvent e{};
    e.kind = phase == 0 ? InjectKind::TouchDown
           : phase == 2 ? InjectKind::TouchUp : InjectKind::TouchMotion;
    e.touch_id = id; e.x = x; e.y = y; e.time_ms = now_ms();
    enqueue_inject(e);
}
// Close the atomic set (call once per MotionEvent after all points).
void alr_wayland_inject_touch_frame() {
    InjectEvent f{}; f.kind = InjectKind::TouchFrame; f.time_ms = now_ms();
    enqueue_inject(f);
}
```

The legacy `alr_wayland_inject_touch(id,x,y,phase)` (point + frame) is kept for the
self-test and back-compat.

### 3d. (Optional, recommended) single-finger pointer emulation INSIDE the compositor

Many simple apps (xterm, some SDL games, legacy GTK2) bind only `wl_pointer`. To keep
them working without the broken per-event co-injection, emulate pointer **only for a
lone single finger** and **only when no second finger is/was down in the sequence**.
This is the standard "touch -> pointer" fallback (weston/libinput do the same). Policy:

- On `TouchDown` with `g_touch_active == 0` (this is the FIRST and only contact):
  remember `g_touch_emulate_pointer = true` and the down id; also emit
  `wl_pointer.enter`+`motion` then `button(BTN_LEFT, pressed)` to the grabbed surface.
- On a SECOND `TouchDown` (`g_touch_active >= 1`): if we were emulating, send
  `wl_pointer.button(BTN_LEFT, released)` + cancel emulation for the rest of the
  sequence (the gesture became multitouch; pointer must not interfere).
- On `TouchMotion` for the emulated id while emulating: emit `wl_pointer.motion`.
- On `TouchUp` of the emulated id while emulating: emit
  `wl_pointer.button(BTN_LEFT, released)`; clear `g_touch_emulate_pointer`.

This keeps the pointer **consistent with a single finger** (never teleporting) and
**absent during real multitouch** — the opposite of today's bug. Gate it behind a flag
`g_touch_pointer_emulation` (default true) so it can be disabled per-client later.

NOTE: emulation lives compositor-side (single source of grab/active state), NOT in the
JNI layer — that is the key architectural correction. The JNI layer must feed **touch
only**; the compositor decides whether to also emit pointer.

---

## 4. Android side: MotionEvent -> wl_touch mapping (the MAIN-session edit)

### 4a. Action -> phase mapping table

| `ev.actionMasked`        | Contacts to forward                         | phase | Notes |
|--------------------------|---------------------------------------------|-------|-------|
| `ACTION_DOWN`            | the one at `actionIndex` (==0, first finger)| 0 down| starts sequence |
| `ACTION_POINTER_DOWN`    | the one at `actionIndex`                     | 0 down| Nth finger joins |
| `ACTION_MOVE`            | ALL pointer indices `0..pointerCount-1`     | 1 move| batched; one frame |
| `ACTION_UP`              | the one at `actionIndex` (last finger)       | 2 up  | ends sequence |
| `ACTION_POINTER_UP`      | the one at `actionIndex`                      | 2 up  | one finger lifts |
| `ACTION_CANCEL`          | (none — send cancel)                          | cancel| `nativeWaylandInjectTouchCancel()` |

Critical correctness fixes vs. today:
- For DOWN/POINTER_DOWN/UP/POINTER_UP use **`actionIndex`** for both the id AND the
  coordinate: `ev.getPointerId(i)`, `ev.getX(i)`, `ev.getY(i)` where `i = actionIndex`.
  (Today's code passes `ev.x`/`ev.y` = pointer index 0's coords — wrong for the 2nd
  finger.)
- For MOVE iterate **every** pointer index `i in 0 until ev.pointerCount` and forward
  `getPointerId(i)`, `getX(i)`, `getY(i)`. (Today only index 0 moves.)
- Map `ACTION_CANCEL` to a distinct cancel call, not `up`.
- After forwarding all contacts of a MotionEvent, call the frame once
  (`nativeWaylandInjectTouchFrame()`), matching §3c.

### 4b. Stable touch ids

Android `getPointerId(index)` is already a stable id for the lifetime of a contact
(0..N), exactly what `wl_touch` needs (`int32_t id`, stable within a sequence). Pass it
through unchanged. No remapping needed; do NOT use `actionIndex` as the id (it is a
positional slot, not stable). The compositor's `g_touch_active` counts downs vs ups, so
ids must be balanced down/up — Android guarantees this per contact.

### 4c. Coordinate scaling (SurfaceView px -> wl_output logical)

`ev.getX/getY` are in **SurfaceView-local px**. The compositor's `output_size` /
`surface_screen_rect` / `map_input_to_surface` already convert output px -> surface
buffer coords, and the output is sized in px (`config_.output_width/height`). So pass
**raw SurfaceView px** through the JNI; do NOT pre-scale in Kotlin.

One caveat to flag for WS-3: `drain_input_queue` computes
`sc = config_.output_scale` (~line 2044) but the touch cases pass `e.x,e.y` straight
into `map_input_to_surface` **without dividing by `sc`** (the pointer-motion case is
the same). On this device `output_scale == 1` (Mali tablet, integer scale), so it is a
no-op and touch is correct today. If a HiDPI integer-scale (scale>1) device is ever
targeted, the surface-local result would be off by `sc`. RECOMMENDATION (WS-3): apply
`x/sc, y/sc` uniformly for pointer AND touch in `map_input_to_surface`'s callers, or
fold `sc` into `map_input_to_surface`. This is pre-existing and out of scope for the
touch fix, but documented here so it isn't lost. **Do not change Kotlin for this.**

### 4d. Long-press / right-click emulation

Two cases, both Android-side (no protocol change):

- **Soft keyboard on text fields:** keep today's behaviour — `setOnLongClickListener`
  raises the IME via `showSoftKeyboard()`. A long-press that is meant as "type here"
  should NOT also be sent as a touch-up that the guest sees as a tap; today the
  `OnTouchListener` already forwarded the down/move, and the long-click is an
  *additional* gesture. Recommended: when a long-press fires, inject
  `nativeWaylandInjectTouchCancel()` to abort the in-progress touch sequence before
  popping the IME, so the guest doesn't see a stray half-tap.
- **Right-click (context menu) for pointer-only apps:** a long-press with a SINGLE
  finger can be surfaced to pointer-only guests as a `BTN_RIGHT` click. Because §3d
  emulation owns pointer, add a compositor entry
  `alr_wayland_inject_pointer_button(0x111 /*BTN_RIGHT*/, 1/0)` driven from Kotlin's
  long-press (only when `ev.pointerCount == 1`). This is optional polish; the touch
  stream itself does not need it (touch-native apps open context menus via long-press
  in their own code). Gate it so it doesn't fire during the IME long-press above —
  pick ONE long-press meaning per app mode, or use a UI toggle.

### 4e. Do we keep `wl_pointer` at all? (policy answer)

YES, keep the `wl_pointer` object advertised (capability unchanged) — many toolkits
require a pointer to exist to bind hover/cursor. But STOP feeding it from raw touch.
Pointer is now driven by:
1. real mouse/trackpad (`setOnGenericMotionListener` -> `nativeWaylandInjectScroll`,
   already present) and a future BT-mouse path, and
2. the compositor's single-finger emulation (§3d) for pointer-only apps.

Real multitouch flows **only** through `wl_touch`. This is the weston/desktop-Linux
model and is what GTK/Qt/Chromium expect. Per-client detection ("does this client bind
wl_touch?") is possible (count `g_touches` resources from that client) but unnecessary:
emitting both touch (always) and single-finger-emulated pointer (when not multitouch)
is well-defined and is what real compositors do.

---

## 5. JNI boundary (Kotlin <-> C++ signatures)

The MAIN session owns these. New/changed `extern "C" JNIEXPORT` in
`runtime_report.cpp` and matching `external fun` in `MainActivity.kt`.

### 5a. Replace the body of the existing `nativeWaylandInjectTouch`

Keep the signature `(jint id, jfloat x, jfloat y, jint phase)` for ABI stability, but
**remove the pointer co-injection** — forward touch only:

```cpp
// runtime_report.cpp  Java_..._nativeWaylandInjectTouch  (~line 6592)
extern "C" JNIEXPORT void JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeWaylandInjectTouch(
    JNIEnv*, jobject, jint id, jfloat x, jfloat y, jint phase) {
#ifdef ALR_HAVE_WAYLAND
    // touch ONLY; pointer (if any) is emulated compositor-side for a lone finger.
    alr::wayland::alr_wayland_inject_touch_point(id, x, y, phase);
#endif
}
```

### 5b. New JNI: frame + cancel

```cpp
extern "C" JNIEXPORT void JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeWaylandInjectTouchFrame(JNIEnv*, jobject){
#ifdef ALR_HAVE_WAYLAND
    alr::wayland::alr_wayland_inject_touch_frame();
#endif
}
extern "C" JNIEXPORT void JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeWaylandInjectTouchCancel(JNIEnv*, jobject){
#ifdef ALR_HAVE_WAYLAND
    alr::wayland::alr_wayland_inject_touch_cancel();
#endif
}
```

### 5c. Kotlin `external fun` declarations (beside line 2804)

```kotlin
private external fun nativeWaylandInjectTouch(id: Int, x: Float, y: Float, phase: Int)
private external fun nativeWaylandInjectTouchFrame()
private external fun nativeWaylandInjectTouchCancel()
```

`nativeWaylandInjectScroll` / `nativeWaylandInjectKey` / `nativeWaylandInjectSelfTest`
are unchanged.

### 5d. Symbol/name contract

JNI name = `Java_dev_chanwoo_androlinux_MainActivity_<method>`. The C++ helpers live in
`namespace alr::wayland` and are declared in `alr_compositor.hpp` so `runtime_report.cpp`
(which already `#include`s it and calls `alr::wayland::alr_wayland_inject_*`) links
without further wiring.

---

## 6. Data flow (end to end)

```
finger on glass
  -> SurfaceView.OnTouchListener (UI thread, MainActivity.kt)
       ACTION_*  ->  per §4a:
         POINTER_DOWN/UP/DOWN/UP: nativeWaylandInjectTouch(getPointerId(actionIndex),
                                    getX(actionIndex), getY(actionIndex), phase)
         MOVE: for i in 0..pointerCount-1:
                 nativeWaylandInjectTouch(getPointerId(i), getX(i), getY(i), 1)
         CANCEL: nativeWaylandInjectTouchCancel()
       then (non-cancel): nativeWaylandInjectTouchFrame()
  -> JNI (runtime_report.cpp, UI thread): alr::wayland::alr_wayland_inject_touch_point /
       _touch_frame / _touch_cancel  (touch ONLY — no synthetic pointer)
  -> enqueue_inject() pushes InjectEvent onto g_inject_queue (mutex), c->wake()
  -> compositor reactor thread: drain_input_queue()
       TouchDown : hit-test (input_target_at) -> grab surface; map_input_to_surface;
                   wl_touch.down(serial,time,surface,id,x_fp,y_fp); ++g_touch_active;
                   [§3d: if first&only finger, emulate wl_pointer.enter/motion/button]
       TouchMotion: map into grabbed surface; wl_touch.motion(time,id,x_fp,y_fp);
                    [§3d: if emulating that id, wl_pointer.motion]
       TouchUp   : wl_touch.up(serial,time,id); --g_touch_active (==0 -> release grab);
                   [§3d: if emulating, wl_pointer.button(BTN_LEFT, released)]
       TouchFrame: wl_touch.frame  (one per MotionEvent, atomic set complete)
       TouchCancel: wl_touch.cancel + frame; drop grab  (NEW)
  -> guest toolkit (GTK/Qt/SDL2/Chromium-Ozone) sees a real multi-contact touch stream
```

Known v1 limitation (acceptable, document in §6): `MotionEvent.getHistorical*` samples
are not drained, so very fast drags lose intermediate points (coalesced by Android +
our ~60Hz). Adding history is a pure Kotlin loop over
`getHistoricalX(i,h)`/`getHistoricalEventTime(h)` if smoother drags are needed later;
it requires no protocol/JNI change (each historical sample is just another
`nativeWaylandInjectTouch(id, hx, hy, 1)` before the final sample, then one frame).

---

## 7. ownerWiring (the precise minimal edits)

### 7a. WS-3 — `app/src/main/cpp/alr_wayland/alr_compositor.cpp` (+ `.hpp`)

1. **`enum class InjectKind`** (~line 263): add `TouchCancel,` to the enum list.
2. **`drain_input_queue` switch** (~line 2074): add the `case InjectKind::TouchCancel`
   block from §3b (send `wl_touch_send_cancel` + frame to all `g_touches`, clear
   `g_touch_target`/`g_touch_active`).
3. **Injectors** (beside `alr_wayland_inject_touch`, ~line 2480): add
   `alr_wayland_inject_touch_point`, `alr_wayland_inject_touch_frame`,
   `alr_wayland_inject_touch_cancel` from §3b/§3c.
4. **(Recommended, §3d)** add `static bool g_touch_pointer_emulation = true;`,
   `bool g_touch_emulating = false; int32_t g_touch_emul_id = -1;` near the touch grab
   state (~line 358), and the emulation emits in the `TouchDown`/`TouchMotion`/`TouchUp`
   cases as specified. Reuse the existing `g_pointers`, `wl_pointer_send_*`,
   `retarget_pointer` helpers.
5. **`alr_compositor.hpp`** (beside line 148): declare the three new
   `alr_wayland_inject_touch_*` functions in `namespace alr::wayland`.

No change to `bind_seat` (TOUCH cap already advertised) and no new global.

### 7b. MAIN — `app/src/main/cpp/runtime_report.cpp`

Replace the body of `Java_..._nativeWaylandInjectTouch` (~line 6592) with the
touch-only call (§5a — delete the 3 pointer co-injection lines 6601/6603/6605). Add the
two new JNI functions `nativeWaylandInjectTouchFrame` / `nativeWaylandInjectTouchCancel`
(§5b) right after it.

### 7c. MAIN — `app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt`

Replace the `setOnTouchListener` body (~lines 1253-1273) with the multi-contact mapping:

```kotlin
setOnTouchListener { v, ev ->
    when (ev.actionMasked) {
        android.view.MotionEvent.ACTION_CANCEL -> {
            nativeWaylandInjectTouchCancel()
        }
        android.view.MotionEvent.ACTION_MOVE -> {
            for (i in 0 until ev.pointerCount) {
                nativeWaylandInjectTouch(ev.getPointerId(i), ev.getX(i), ev.getY(i), 1)
            }
            nativeWaylandInjectTouchFrame()
        }
        android.view.MotionEvent.ACTION_DOWN,
        android.view.MotionEvent.ACTION_POINTER_DOWN -> {
            val i = ev.actionIndex
            nativeWaylandInjectTouch(ev.getPointerId(i), ev.getX(i), ev.getY(i), 0)
            nativeWaylandInjectTouchFrame()
            v.requestFocus(); v.performClick()   // reclaim key focus on first contact
        }
        android.view.MotionEvent.ACTION_UP,
        android.view.MotionEvent.ACTION_POINTER_UP -> {
            val i = ev.actionIndex
            nativeWaylandInjectTouch(ev.getPointerId(i), ev.getX(i), ev.getY(i), 2)
            nativeWaylandInjectTouchFrame()
        }
    }
    true
}
```

(`requestFocus`/`performClick` only on the first DOWN, not POINTER_DOWN, is a refinement
— guard with `ev.actionMasked == ACTION_DOWN` if desired.) Add the two `external fun`
decls from §5c beside line 2804. The `setOnLongClickListener` IME path is unchanged;
optionally prepend `nativeWaylandInjectTouchCancel()` before `showSoftKeyboard(v)`
(§4d).

---

## 8. Android API & permissions

- Input source: `android.view.SurfaceView.setOnTouchListener` /
  `View.OnTouchListener.onTouch(View, MotionEvent)`. **No permission required.**
- `MotionEvent`: `getActionMasked()`, `getActionIndex()`, `getPointerCount()`,
  `getPointerId(int)`, `getX(int)`, `getY(int)`, optional
  `getHistoricalX/Y/EventTime`. API 8+ (multitouch); all present on Android 16.
- Soft keyboard: `InputMethodManager.showSoftInput(View, int)` (existing). **No
  permission.**
- Right-click long-press: pure local synthesis, no API/permission.

---

## 9. Test / verification plan (device-pending — cannot run host-only)

1. `nativeWaylandInjectSelfTest` already exercises one touch down/up + key; extend the
   self-test to a synthetic 2-contact sequence (two ids down, both move, both up, one
   frame per step) and assert via logcat `drain_input n=...` that `wl_touch_send_down`
   fires for two distinct ids and that NO `wl_pointer_send_button` fires (emulation off
   for multitouch). HOST cannot run this; WS-3/MAIN drains on device.
2. Device: in GIMP, two-finger pan/zoom on canvas should pan/zoom (touch), not
   rubber-band-select (the mouse-drag symptom). A single tap on a menu still works
   (single-finger emulation -> pointer click OR native touch, both land).
3. Device: a `wl_touch`-native app (e.g. a GTK touch demo) receives `down/motion/up`
   with stable ids; logcat shows `g_touch_active` rising to 2 and back to 0.

This doc is host-only design; no device run was performed. The mapping table, JNI
signatures, and the two-line root cause are verified against the current source
(`alr_compositor.cpp`, `runtime_report.cpp`, `MainActivity.kt` at this commit).
