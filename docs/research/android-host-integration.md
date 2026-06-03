# Android-host ⇄ Linux-guest integration — SSOT / roadmap

Status: HOST-authored design SSOT (no device verification). Owners execute the
`ownerWiring` rows. Branch of record: `auto/host-ssot`.

This is the single map of EVERY Android-host ⇄ Linux-guest integration the
product needs, with honest per-row status. The engine is proven (GIMP usable by
touch, Chromium renders on-screen, in-app Wayland compositor presents guest
frames on a SurfaceView, v2 apt + Compose product UI merged). What follows is the
prioritized backlog the compositor (WS-3, `alr_compositor.cpp`) and the Android
(MAIN, `MainActivity.kt`) sessions execute.

---

## 0. The two seams every row crosses

There are exactly two integration seams. Each row below is implemented by
extending one or both:

* **Android seam** — `app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt`.
  The `SurfaceView` (built at ~L1232) carries `setOnTouchListener` (L1253),
  `setOnGenericMotionListener` (L1240), `setOnKeyListener` (L1286),
  `setOnLongClickListener` (L1276). Permissions live in
  `app/src/main/AndroidManifest.xml` (today only `INTERNET` +
  `ACCESS_NETWORK_STATE`). `external fun native…` declarations are at L2669–2814.

* **Compositor / guest seam** — `app/src/main/cpp/alr_wayland/alr_compositor.cpp`.
  `wl_seat` advertises `POINTER|KEYBOARD|TOUCH` (L1792). Input is delivered via an
  `InjectEvent` queue (`g_inject_queue`, drained on the compositor reactor thread
  at L2037) fed by the C-ABI `alr_wayland_inject_*` entry points (L2462–2509,
  declared in `alr_compositor.hpp` L145–152). The **JNI bridge** that turns
  `nativeWayland*` Kotlin calls into those entry points lives in
  `runtime_report.cpp` L6572–6651 (NOT in the compositor — important: the bridge
  is the MAIN/lead's file, the entry points are WS-3's).

The **JNI bridge is the contract** between the two seams. New integrations add a
new `Java_…native…` function in `runtime_report.cpp` + a matching
`alr_wayland_inject_*` (or a new helper module) + a Kotlin `external fun`.

### Current input data flow (today, end-to-end)

```
Android MotionEvent (SurfaceView.onTouch, L1262)
  -> nativeWaylandInjectTouch(id, x, y, phase)              [Kotlin, MainActivity]
  -> Java_..._nativeWaylandInjectTouch                      [JNI, runtime_report.cpp:6592]
       *synthesizes BOTH*: alr_wayland_inject_pointer_motion + _button (BTN_LEFT)
                           AND alr_wayland_inject_touch
  -> enqueue_inject -> g_inject_queue -> reactor drain      [alr_compositor.cpp]
  -> wl_pointer.* AND wl_touch.* to the focused surface     [guest sees a MOUSE]
```

The `_button(BTN_LEFT)` synthesis is the crux of the "feels wrong" complaint:
multi-finger touch collapses to a single mouse cursor, no pinch/rotate, and
toolkits that branch on `wl_touch` vs `wl_pointer` (GTK, Chromium/Ozone) get a
contradictory dual stream.

---

## 1. The integration table (the backlog)

Legend — **Status**: DONE / CRUDE (works but wrong feel or partial) / MISSING.
**MP?**: needs the CR-5 multiprocess model (a per-guest process, not the current
single in-process guest) to be correct/safe. **Pri**: P0 (next), P1, P2, P3.

| # | Integration | Status | Android API + permission | Guest-side path (Wayland global / dev node / lib) | Difficulty | Pri | MP? |
|---|-------------|--------|--------------------------|---------------------------------------------------|-----------|-----|-----|
| 1 | **Touch (real)** | CRUDE | `MotionEvent` on `SurfaceView.onTouch` — no permission | `wl_seat` `wl_touch` (down/motion/up/frame/cancel) — already advertised+handled | Low | **P0** | No |
| 2 | **Soft-keyboard / IME (text)** | CRUDE | `InputConnection` + `InputMethodManager`; `onCreateInputConnection` — no permission | `text-input-unstable-v3` global (NEW) → `commit_string`; fallback synthetic `wl_keyboard` | High | **P0** | No |
| 3 | **Hardware keyboard** | CRUDE | `KeyEvent` on `onKey` (L1286) — no permission | `wl_keyboard` (key/modifiers) — advertised; keymap = built-in US (`alr_xkb_keymap_us.h`) | Med | P1 | No |
| 4 | **Mouse / trackpad** | CRUDE | `onGenericMotion` (scroll, L1240) + `onTouch` for pos — no permission | `wl_pointer` (motion/button/axis) + `relative-pointer-v1`+`pointer-constraints-v1` (NEW) for FPS/CAD | Med | P1 | No |
| 5 | **USB host (devices)** | MISSING | `UsbManager`, `USB_PERMISSION` runtime grant; `<uses-feature usb.host>` | `/dev/bus/usb/*` is BLOCKED (no root, SELinux). Bridge per-class: HID→`wl_*`/uinput-style, serial→pty, storage→SAF | High | P2 | **Yes** |
| 6 | **Clipboard (copy/paste)** | CRUDE (stub) | `ClipboardManager` (`CLIPBOARD_SERVICE`) — no permission | `wl_data_device` selection — exists but `set_selection` is a **no-op** (L1738); needs real offer/selection plumbing | Med | **P0** | Partial |
| 7 | **Audio out (playback)** | MISSING | `AudioTrack` / `AAudio` (`AAudioStreamBuilder`) — no permission | guest `/dev/snd` blocked → ship a **PulseAudio/PipeWire shim** the guest connects to over a unix socket; host pump → `AudioTrack` | High | P1 | **Yes** |
| 8 | **Audio in / mic** | MISSING | `AudioRecord` / `AAudio` input, `RECORD_AUDIO` (runtime) | same PulseAudio shim, source side; host `AudioRecord` → socket → guest | High | P3 | **Yes** |
| 9 | **Camera** | MISSING | CameraX / `Camera2`, `CAMERA` (runtime); `<uses-feature camera>` | guest `/dev/video*` blocked → **v4l2loopback unavailable** w/o root → libcamera/pipewire shim or MJPEG-over-socket → guest libcamera/GStreamer | Very High | P3 | **Yes** |
| 10 | **GPS / location** | MISSING | `FusedLocationProviderClient` / `LocationManager`, `ACCESS_FINE_LOCATION` (runtime) | guest `gpsd`/GeoClue over D-Bus, or a small NMEA pty the host feeds | Med | P3 | Partial |
| 11 | **Sensors (accel/gyro/…)** | MISSING | `SensorManager` `TYPE_ACCELEROMETER`/`GYROSCOPE`/… — no permission (body sensors gated) | guest `/dev/iio:*` blocked → IIO-style values pushed over a socket; or a custom Wayland-adjacent protocol | Med | P3 | Partial |
| 12 | **Notifications** | MISSING | `NotificationManager` + `POST_NOTIFICATIONS` (runtime, API 33+) | guest D-Bus `org.freedesktop.Notifications` (libnotify) → host bridge → Android notification | Med | P2 | Partial |
| 13 | **Share / Intent (out+in)** | MISSING | `Intent.ACTION_SEND` (out) / intent-filter + `onNewIntent` (in) — no permission | xdg-desktop-portal-style bridge, or a guest CLI (`xdg-open`) interposed → `startActivity` | Med | P2 | Partial |
| 14 | **File access (SAF)** | DONE (host) | `ACTION_OPEN_DOCUMENT` / tree, `takePersistableUriPermission` — product-ux merged it | guest sees rootfs paths via ALR path mediation; SAF `content://` materialized into rootfs by the SAF layer | — | — | No |
| 15 | **Display** | DONE | `SurfaceView` + `WindowManager` metrics; device-exact | `wl_output` device-exact mode (1200×1920@90Hz advertised), `wl_surface` present to SurfaceView | — | — | No |
| 16 | **Network** | DONE | `INTERNET`+`ACCESS_NETWORK_STATE` (declared) | guest sockets pass through; NETLINK/DNS handled in interpose layer (CR-2) | — | — | No |
| 17 | **Battery / power** | MISSING | `BatteryManager` / `ACTION_BATTERY_CHANGED` sticky broadcast — no permission | guest UPower (D-Bus) / `/sys/class/power_supply/*` synthesized from Android battery state | Low | P2 | Partial |
| 18 | **Vibration / haptics** | MISSING | `Vibrator` / `VibratorManager`, `VIBRATE` (normal, auto-grant) | guest has no standard node; expose via the input/feedback path — a tiny custom request or libei-style channel | Low | P2 | Partial |
| 19 | **Wake-lock / keep-screen-on** | MISSING (impl) | `WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON` / `WAKE_LOCK` | guest `org.freedesktop.ScreenSaver` inhibit (D-Bus) → host sets the flag | Low | P3 | Partial |
| 20 | **Stylus / pen (pressure/tilt)** | MISSING | `MotionEvent` `TOOL_TYPE_STYLUS`, `getPressure`/`getAxisValue(AXIS_TILT)` — no permission | `tablet-unstable-v2` (NEW) `zwp_tablet_tool` (pressure/tilt/distance) | Med | P2 | No |

Rows 14–16 are DONE and listed only to keep the map complete (do not re-litigate).

---

## 2. P0 detail — the three the user actually feels

These three turn "renders on-screen" into "usable Linux app." Each is specified
to ready-to-wire depth.

### 2.1 Touch (real, not synthesized) — row #1

**Problem.** `nativeWaylandInjectTouch` (runtime_report.cpp:6592) emits BOTH a
synthetic `wl_pointer` BTN_LEFT click AND `wl_touch`. The guest sees a mouse.
The compositor ALREADY advertises `WL_SEAT_CAPABILITY_TOUCH` (L1792) and has full
`wl_touch` implicit-grab + cancel handling (`g_touch_target`/`g_touch_active`,
L352–411). So the engine is ready; the wrongness is purely in the JNI synthesis.

**Design — multi-touch, no pointer synthesis.**

* **Android side** (`MainActivity.kt`, `setOnTouchListener`, L1253). Stop
  forwarding a single pointer index. Iterate ALL pointers in the `MotionEvent`
  for the historical + current samples and forward each as a touch with its
  `pointerId`. Phase per-pointer: `ACTION_DOWN`/`ACTION_POINTER_DOWN` → down for
  that pointer only; `ACTION_MOVE` → motion for every pointer; `ACTION_UP`/
  `ACTION_POINTER_UP` → up for that pointer; `ACTION_CANCEL` → cancel all.

  ```kotlin
  setOnTouchListener { v, ev ->
      when (ev.actionMasked) {
          MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
              val i = ev.actionIndex
              nativeWaylandTouchDown(ev.getPointerId(i), ev.getX(i), ev.getY(i))
              if (ev.actionMasked == MotionEvent.ACTION_DOWN) v.requestFocus()
          }
          MotionEvent.ACTION_MOVE ->
              for (i in 0 until ev.pointerCount)
                  nativeWaylandTouchMotion(ev.getPointerId(i), ev.getX(i), ev.getY(i))
          MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
              val i = ev.actionIndex
              nativeWaylandTouchUp(ev.getPointerId(i))
          }
          MotionEvent.ACTION_CANCEL -> nativeWaylandTouchCancel()
      }
      nativeWaylandTouchFrame()   // one frame per MotionEvent dispatch
      true
  }
  ```

* **JNI bridge** (`runtime_report.cpp`, NEW functions). Replace the dual-emit
  `nativeWaylandInjectTouch` callers; expose split entry points that map 1:1 to
  the existing C-ABI so NO pointer is synthesized:

  | Kotlin `external fun` | JNI `Java_…` | calls (alr_compositor.hpp) |
  |---|---|---|
  | `nativeWaylandTouchDown(id:Int,x:Float,y:Float)` | new | `alr_wayland_inject_touch(id,x,y,0)` |
  | `nativeWaylandTouchMotion(id:Int,x:Float,y:Float)` | new | `alr_wayland_inject_touch(id,x,y,1)` |
  | `nativeWaylandTouchUp(id:Int)` | new | needs `alr_wayland_inject_touch_up(id)` (see below) |
  | `nativeWaylandTouchCancel()` | new | needs `alr_wayland_inject_touch_cancel()` |
  | `nativeWaylandTouchFrame()` | new | needs `alr_wayland_inject_touch_frame()` |

  Note the existing `alr_wayland_inject_touch` auto-appends a `TouchFrame` after
  EVERY event (L2487–2489) — wrong for multi-touch, which wants ONE frame after a
  batch. The owner should make `inject_touch` NOT auto-frame and expose an
  explicit `inject_touch_frame`. Up needs no coords (compositor remembers the
  grabbed surface per `touch_id`), hence a `_up(id)` variant.

* **Compositor side** (WS-3, `alr_compositor.cpp`). Minimal: split the auto-frame
  out of `alr_wayland_inject_touch` (L2480) and add `…_touch_up(id)` /
  `…_touch_frame()` / `…_touch_cancel()` C-ABI shims that enqueue
  `InjectKind::TouchUp`/`TouchFrame`/`TouchCancel`. The reactor's `wl_touch.*`
  emission already exists; only the enqueue helpers are new. **Do NOT remove the
  pointer-synthesis from `nativeWaylandInjectTouch` itself if other callers (the
  self-test bursts, L1454–1489) rely on it — instead make the production listener
  use the new split path and leave the old combined entry for the self-test.**

**Difficulty: Low.** No new protocol, no new permission, the hard part
(implicit grab, cancel-on-unmap) is already done. The whole change is "stop
lying to the guest that touch is a mouse."

**Caveat (honest):** some Linux toolkits still expect a pointer for hover/menus.
Keep a per-app policy flag (default: real touch; opt-in pointer-emulation for
apps that need a cursor) rather than hard-removing pointer synthesis globally.

### 2.2 Soft-keyboard / IME — row #2

**Problem.** Today: `showSoftInput` (L2366) raises the IME, but there is NO
`InputConnection`, so the IME has nowhere to commit. Keys only reach the guest
through `onKey` → `androidKeyToEvdev` (L2375), a hand-rolled keycode table that
is **US-ASCII only** — no CJK/IME composition, no autocorrect, no emoji, no
accented input. The guest applies the built-in US keymap (`alr_xkb_keymap_us.h`),
so even the evdev path can't represent non-US layouts.

**Design — real IME via text-input + an InputConnection.**

* **Android side.** Make the `SurfaceView` (or a 0-px overlay `View`) return a
  custom `InputConnection` from `onCreateInputConnection(outAttrs)`, with
  `outAttrs.inputType = TYPE_CLASS_TEXT` and `imeOptions = IME_FLAG_NO_FULLSCREEN`.
  Override:
  - `commitText(text, newCursorPosition)` → `nativeWaylandImeCommit(text.toString())`
  - `setComposingText(text, …)` → `nativeWaylandImePreedit(text.toString(), cursor)`
  - `finishComposingText()` → `nativeWaylandImePreedit("", 0)`
  - `deleteSurroundingText(before, after)` → `nativeWaylandImeDelete(before, after)`
  - `sendKeyEvent(KeyEvent)` → existing evdev path (for Enter/Backspace from IME)

  This is the standard pattern a `BaseInputConnection` subclass uses to host a
  remote text field.

* **Guest-side protocol — `text-input-unstable-v3`** (NEW global; the protocol
  XML must be vendored under `third_party/protocols/text-input/` and generated
  into `wayland_generated/`). The compositor advertises
  `zwp_text_input_manager_v3`; the focused client creates a
  `zwp_text_input_v3` and `enable`s it. On Android commit/preedit the compositor
  sends:
  - `commit_string` ← `nativeWaylandImeCommit`
  - `preedit_string(text, cursor_begin, cursor_end)` ← `nativeWaylandImePreedit`
  - `delete_surrounding_text(before, after)` ← `nativeWaylandImeDelete`
  - then `done(serial)` to flush.

  The client's `enable`/`set_cursor_rectangle` is the signal Android should
  `showSoftInput` (text field focused) vs `hideSoftInput` (unfocused). Bridge
  that back with `nativeWaylandImeActive(active:Boolean, x,y,w,h:Int)` →
  `MainActivity` shows/hides the IME and (optionally) positions a candidate
  window. **This is the proper fix for the "IME pops up on every tap / covers
  menus" hack** — the IME is raised only when the GUEST says a text field is
  focused, not on tap heuristics.

* **JNI bridge.** New `Java_…_nativeWaylandImeCommit(jstring)`,
  `…ImePreedit(jstring,jint,jint)`, `…ImeDelete(jint,jint)` in
  `runtime_report.cpp`, each converting the UTF-8 jstring and calling new C-ABI
  `alr_wayland_text_input_commit(const char*)` etc. in the compositor. The
  compositor→Android direction (`ImeActive`) is a JNI **up-call**: the compositor
  keeps a cached `JavaVM*` + `MainActivity` global ref (the GPU/present path
  already up-calls for vsync — reuse that mechanism) and calls
  `MainActivity.onGuestImeActive(...)` on the UI thread via `runOnUiThread`.

* **Fallback.** Keep the evdev path for hardware keyboards and for guests that do
  NOT bind `text-input-v3` (older GTK builds). Detect: if no `zwp_text_input_v3`
  is active for the focused surface, route IME `commitText` through a
  char→keysym→evdev synthesis (best-effort, ASCII).

**Difficulty: High.** New vendored protocol + generated bindings + a real
`InputConnection` + a bidirectional JNI bridge. But it is THE feature that makes
text entry (search bars, terminals, editors) actually work, including CJK.

### 2.3 Clipboard (copy/paste both directions) — row #6

**Problem.** `wl_data_device_manager` IS advertised (so GTK/Chromium bind
`wl_seat`), but `data_device_set_selection` is a **no-op** (L1738) and no
`data_offer`/`selection` is ever sent (L1757). Result: guest↔guest copy/paste is
dead and there is zero bridge to the Android `ClipboardManager`.

**Design — bridge `wl_data_device` selection to Android `ClipboardManager`.**

* **Guest → Android (copy in guest).** Implement `set_selection`: when the
  focused client offers a selection, the compositor becomes the selection owner.
  On a guest copy, read the offered `text/plain;charset=utf-8` MIME by creating a
  pipe, `wl_data_source.send(mime, fd)`, draining the fd on a worker thread, then
  up-call `MainActivity.onGuestClipboardCopy(text)` →
  `ClipboardManager.setPrimaryClip(ClipData.newPlainText(...))`.

* **Android → guest (paste from Android).** On Android clipboard change
  (`ClipboardManager.addPrimaryClipChangedListener`, or lazily on guest paste),
  the compositor must own a `wl_data_source` advertising `text/plain;charset=utf-8`
  and send `wl_data_device.selection(offer)` to the focused client. When the guest
  pastes, it requests the MIME on a pipe fd; the compositor writes the Android
  clipboard text into it. Provide the text via
  `nativeWaylandSetClipboard(text:String)` (Kotlin → JNI → cached string the
  compositor serves on demand).

* **JNI bridge.** Down: `nativeWaylandSetClipboard(jstring)` →
  `alr_wayland_set_clipboard(const char*)`. Up: cached `JavaVM*` →
  `MainActivity.onGuestClipboardCopy(String)` on the UI thread.

* **Android API + permission.** `ClipboardManager` via `CLIPBOARD_SERVICE`. NO
  permission needed, BUT on Android 10+ background clipboard READ is restricted —
  reads only work while the app is foreground/focused, which is our case (the
  guest GUI is on-screen). Note this limitation in the row.

**Difficulty: Medium.** No new Wayland protocol (core `wl_data_device` suffices
for MVP; primary-selection/DnD are separate later rows). The work is real
selection plumbing in the compositor + the two-way clipboard JNI bridge.

---

## 3. P1 detail — audio out, mouse, hardware keyboard

### 3.1 Audio out (playback) — row #7 — needs CR-5 multiprocess

The guest expects ALSA (`/dev/snd`) or PulseAudio/PipeWire. `/dev/snd` is blocked
(no root, SELinux `media`/`audio` access). The only public path is to terminate
audio at a **userspace sound server the guest connects to over a unix socket**,
and pump its output to Android `AudioTrack`/`AAudio`.

* **Recommended:** ship a minimal **PulseAudio-protocol server** (or pipewire-pulse
  shim) inside the host process / a helper, listening on a unix socket the guest
  reaches at `$PULSE_SERVER=unix:/<rootfs>/run/pulse/native`. Guest apps
  (`libpulse`, or ALSA via the `pulse` plugin, or SDL/GStreamer pulse sink)
  connect normally. The server's sink callback feeds a ring buffer drained by an
  `AAudio` output stream (low-latency, API 26+) or `AudioTrack`.
* **JNI:** an audio module (NEW `alr_audio/` files, owner-wired) exposing
  `alr_audio_start()/stop()` + the `AAudioStream` lifecycle from Kotlin
  (`nativeAudioStart()`), plus the PA socket pump on a dedicated thread.
* **Permission:** none for output. `<uses-feature audio.output>` optional.
* **MP?** YES — the PA server owning the `AAudio` stream is cleanest as part of
  the CR-5 multiprocess broker so a crashing guest can't take the audio thread
  down. In the current single-process model it can live on a host thread, but
  per-guest mixing/volume wants the broker.

**Difficulty: High** (a PA-protocol shim is non-trivial), but `libpulse` clients
are ubiquitous so it unlocks most desktop audio at once.

### 3.2 Mouse / trackpad — row #4

Mostly DONE for scroll (`onGenericMotion`→`wl_pointer.axis`, L1240). Gaps:
(a) real relative-motion for FPS/CAD/games needs **`relative-pointer-v1`** +
**`pointer-constraints-v1`** (pointer lock) — NEW globals; (b) right/middle
button + hover from a real USB/Bluetooth mouse already flows as touch today but
should map true `BTN_RIGHT`/`BTN_MIDDLE` from `MotionEvent.getButtonState()` in
`onGenericMotion`. **Pri P1, difficulty Med, no permission, no MP.**

### 3.3 Hardware keyboard — row #3

Works via `onKey`→evdev but is US-only and table-limited. Fix shares 2.2's
keymap work: ship per-layout xkb keymaps and let the guest pick, OR (simpler)
forward the Android `KeyEvent.getUnicodeChar()` through the `text-input-v3` path
for printable keys and keep evdev only for non-text keys (arrows, F-keys, Ctrl
combos). **Pri P1, Med, no permission, no MP.**

---

## 4. P2/P3 detail — the long tail

* **USB host (#5).** `UsbManager` + runtime `USB_PERMISSION` grant gives a
  `UsbDeviceConnection`/`UsbEndpoint` (no root). `/dev/bus/usb` is unreachable by
  the guest. Bridge **per device class**: HID → synthesize into the seat or a
  uinput-style guest node; CDC-serial → a guest pty; mass-storage → mount via SAF.
  No generic passthrough is possible without root. **MP? YES** — the USB broker
  belongs in the CR-5 process owning the `UsbManager`. Very app-specific; P2.

* **Notifications (#12) / Share (#13) / Battery (#17) / Wake-lock (#19).** All
  four are best served by a **guest-side D-Bus broker** the host bridges:
  `org.freedesktop.Notifications` → `NotificationManager` (needs
  `POST_NOTIFICATIONS`, API 33+), `org.freedesktop.portal.*` for share/open,
  UPower over `/sys/class/power_supply` synthesized from `BatteryManager`,
  `org.freedesktop.ScreenSaver` inhibit → `FLAG_KEEP_SCREEN_ON`. Standing up a
  small D-Bus session bus in the rootfs + one host bridge process amortizes across
  all four. **MP? Partial** (the bridge is cleanest in the broker). P2.

* **Camera (#9) / Mic (#8) / Sensors (#11) / GPS (#10).** All require host
  capture (CameraX/`AudioRecord`/`SensorManager`/Fused location, with their
  runtime permissions) pumped over a socket into a guest userspace provider
  (libcamera/pipewire / pulse source / IIO-emulation / gpsd-NMEA). High effort,
  niche; P3. **MP? Partial/Yes** for the capture-owning broker.

* **Stylus (#20).** `tablet-unstable-v2` global + read pressure/tilt from
  `MotionEvent`. Self-contained, P2, no permission, no MP.

* **Vibration (#18).** `Vibrator` + `VIBRATE` (auto-granted normal permission).
  No standard guest node; expose a tiny custom request or piggyback a
  feedback channel. Low effort, P2.

---

## 5. Cross-references (sibling design docs being written in parallel)

This SSOT is the index; deep dives live in sibling `auto/*` branches. Keep them
in sync:

* **`auto/host-touch`** (sibling) — the deep design for row #1 (real multi-touch,
  no pointer synthesis). This SSOT's §2.1 is the summary; defer wire-level detail
  there.
* **`auto/host-audio`** (sibling) — deep design for rows #7/#8 (PulseAudio shim +
  `AAudio` pump). §3.1 here is the summary.
* **`auto/r12-g4-input`, `auto/r7-input`, `auto/r5-input`** (already in tree) —
  the existing compositor input work (wl_keyboard modifier fixes, focus-follow,
  v5 axis_source scroll). Rows #3/#4 build on these; do not duplicate.
* **product-ux SAF layer** (merged) — row #14 DONE; reuse its
  `takePersistableUriPermission` materialization for USB-storage (#5) and
  share-in (#13).

If a sibling doc and this table disagree on status, the sibling (deeper) doc
wins for its row; update this table to match.

---

## 6. Prioritized execution order (the backlog the owners pull from)

1. **#1 Touch (real)** — P0, Low. Biggest feel win for the least work; pure JNI +
   small compositor shim split. Do first.
2. **#6 Clipboard** — P0, Med. Unlocks copy/paste in/out; selection plumbing +
   2-way bridge, no new protocol.
3. **#2 IME / text-input-v3** — P0, High. The text-entry unlock (incl. CJK);
   biggest single chunk; vendoring + InputConnection + bidirectional JNI.
4. **#4 Mouse polish** + **#3 hardware-kbd via Unicode** — P1, Med.
5. **#7 Audio out (PulseAudio shim)** — P1, High; first CR-5 multiprocess
   consumer — co-design with the CR-5 broker.
6. **D-Bus broker bundle (#12/#13/#17/#19)** — P2; one broker, four features.
7. **#20 stylus**, **#18 vibration** — P2, small self-contained wins.
8. **#5 USB**, **#8 mic / #9 camera / #10 GPS / #11 sensors** — P2/P3, niche,
   capture-broker (CR-5) gated.

---

## 7. The minimal owner edits (ready-to-wire summary)

**MAIN owner (`MainActivity.kt` + `runtime_report.cpp` JNI bridge):**

1. (#1) Rewrite `setOnTouchListener` (L1253) to per-pointer multi-touch using the
   NEW split `nativeWaylandTouch{Down,Motion,Up,Cancel,Frame}` and STOP calling
   the pointer-synthesizing `nativeWaylandInjectTouch` on the production path. Add
   the matching `external fun` decls (near L2804) and the `Java_…` bridges in
   `runtime_report.cpp` (near L6592) that call `alr_wayland_inject_touch*`.
2. (#2) Add `onCreateInputConnection` returning a `BaseInputConnection` subclass
   that forwards `commitText`/`setComposingText`/`deleteSurroundingText` to NEW
   `nativeWaylandIme*` JNI; gate `showSoftInput`/`hideSoftInput` on the
   compositor up-call `onGuestImeActive(...)` instead of the long-press hack.
3. (#6) Add `ClipboardManager` get/set + `nativeWaylandSetClipboard` and the
   `onGuestClipboardCopy(String)` up-call handler.

**WS-3 owner (`alr_compositor.cpp`):**

1. (#1) Split the auto-`TouchFrame` out of `alr_wayland_inject_touch` (L2480) and
   add C-ABI `alr_wayland_inject_touch_up(id)`, `…_touch_frame()`,
   `…_touch_cancel()` enqueuing `InjectKind::TouchUp/TouchFrame/TouchCancel`
   (reactor emission already exists).
2. (#2) Vendor `text-input-unstable-v3`, advertise `zwp_text_input_manager_v3`,
   wire `commit_string`/`preedit_string`/`done` from new
   `alr_wayland_text_input_*` C-ABI; up-call Android on `enable`/`disable`.
3. (#6) Implement `data_device_set_selection` (L1738) for real: own the
   selection, send `data_offer`+`wl_data_device.selection` to the focused
   client, serve MIME over a pipe; bridge both directions to Android.

**Shared infra to stand up once (enables #2/#6/#7/#12…):** a compositor→Android
**JNI up-call channel** (cached `JavaVM*` + `MainActivity` global ref + a
`runOnUiThread` post). The present/vsync path likely already caches a `JavaVM*`;
factor it into a tiny helper the lead owns so every host-bound event (IME-active,
clipboard-copy, notification, battery) reuses one mechanism rather than each
feature re-deriving JNI attach/detach.
