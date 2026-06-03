# Android soft-keyboard ↔ Wayland guest text input (zwp_text_input_v3)

**Status:** DESIGN (host-only; no device). Branch `auto/host-ime`.
**Owner-wiring targets:** `app/src/main/cpp/alr_wayland/alr_compositor.cpp` (WS-3),
`app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt` (MAIN),
`app/src/main/cpp/runtime_report.cpp` (MAIN, JNI bridge).
**New assets delivered by this branch (self-contained, no existing file touched):**
- `app/src/main/cpp/third_party/protocols/text-input/text-input-unstable-v3.xml`
  (vendored canonical protocol, version 1, exact upstream text).
- `app/src/main/cpp/alr_wayland/alr_text_input.hpp`
  (the compositor↔Android IME boundary API; header-only, no wl/Android types).
- this document.

---

## 0. Problem & current state (grounded in the tree)

The input path already works but is a one-way street. Android key/touch events are
pushed into the compositor through `alr_wayland_inject_*` (compositor.hpp:145–152),
drained on the compositor thread in `Compositor::drain_input_queue()`
(alr_compositor.cpp:2034) and replayed as `wl_pointer` / `wl_touch` / `wl_keyboard`
protocol. The keyboard half is real: a self-contained US XKB keymap is shipped to the
guest (`alr_xkb_keymap_us.h`, sent in `seat_get_keyboard`, alr_compositor.cpp:1625–1648),
modifier tracking is correct (`evdev_to_mod_bit`, alr_compositor.cpp:311), and
`InjectKind::Key` sends `wl_keyboard.enter/key/modifiers` to `g_focus_surface`
(alr_compositor.cpp:2203–2244).

What is **missing** is the *trigger* and the *return text channel for an IME*:

1. **No focus signal.** The compositor has no idea when a guest widget is a text
   field. `MainActivity` raises the soft keyboard only on a manual long-press
   (`setOnLongClickListener` → `showSoftKeyboard`, MainActivity.kt:1276–1280,
   2366–2369). Tapping a GTK/Qt/chromium text entry does nothing.

2. **No IME path back.** Even with the keyboard up, the only return path is
   `setOnKeyListener` → `androidKeyToEvdev` → `nativeWaylandInjectKey`
   (MainActivity.kt:1286–1297, 2375). That hard-codes a US keymap and **drops every
   character a soft IME composes** (emoji, CJK, autocorrect, glide typing): soft
   keyboards deliver text via `InputConnection.commitText`, not `KeyEvent`, and the
   `SurfaceView` has no `InputConnection` (`onCreateInputConnection` is never
   overridden), so `commitText` is silently lost.

This doc closes both gaps with the standard Wayland IME protocol
**`zwp_text_input_v3`** plus a `BaseInputConnection` on the SurfaceView.

---

## 1. End-to-end data flow

```
 GUEST (GTK/Qt/chromium)                  COMPOSITOR (alr_compositor.cpp)             ANDROID (MainActivity.kt / runtime_report.cpp JNI)
 ───────────────────────                  ──────────────────────────────             ──────────────────────────────────────────────────
 focus a text entry
   zwp_text_input_v3.enable
   set_content_type(purpose,hint)
   set_cursor_rectangle(x,y,w,h)
   .commit  ───────────────────────────▶ text_input_commit():
                                            store enabled+purpose+hint+rect on the
                                            FOCUSED SurfaceState; send .done(serial)
                                            back; build AlrImeState; invoke
                                            g_ime_state_cb ───────────────────────────▶ (compositor thread) JNI upcall:
                                                                                          post to main looper →
                                                                                          imm.restartInput(surfaceView)
                                                                                          imm.showSoftInput(surfaceView)
                                                                                          set inputType from purpose/hint
 user types "한", "x", backspace                                                       Soft keyboard → InputConnection:
                                                                                          commitText("한")  / sendKeyEvent / deleteSurroundingText
                                          alr_ime_commit_text("한") ◀──────────────────  nativeWaylandImeCommitText(...)
   zwp_text_input_v3.preedit_string("")   text_input send commit_string + done
   commit_string("한") + done ◀─────────  (enqueued, drained on compositor thread)
 widget inserts "한"
                                          alr_ime_delete_surrounding(n,0) ◀──────────  nativeWaylandImeDeleteSurrounding(before,after)
   delete_surrounding_text + done ◀──────
 widget deletes
 blur / Esc
   zwp_text_input_v3.disable
   .commit  ───────────────────────────▶ text_input_commit(): enabled=false; .done;
                                            g_ime_state_cb(enabled=false) ─────────────▶ imm.hideSoftInputFromWindow(token)
```

Hardware-key path is untouched and coexists (see §6).

---

## 2. (a) Guest text-field focus detection — `zwp_text_input_v3` in the compositor

### 2.1 Why v3 and which toolkits use it

`zwp_text_input_v3` (text-input-unstable-v3) is the protocol GTK3/4, Qt6
(`QtWayland`), and Chromium/Ozone-Wayland all speak when the compositor advertises
`zwp_text_input_manager_v3`. They enable/disable it on caret focus and report a
cursor rectangle. SDL2 and `foot` (terminal) do **not** use it — they rely on raw
`wl_keyboard`; those are handled by the fallback (§2.6, §5).

> Version: we ship **v3 only** (not the older `zwp_text_input_v1`/`_v2`, which had a
> different, racy model). GTK/Qt/Chromium all prefer v3 when present and fall back to
> raw keyboard otherwise — exactly the behavior we want.

### 2.2 Protocol surface (from the vendored XML)

Global: **`zwp_text_input_manager_v3`** (version 1, singleton)
- request `get_text_input(new_id zwp_text_input_v3, object wl_seat)`
- request `destroy`

Object: **`zwp_text_input_v3`** (version 1), per seat. Requests we must handle:
- `enable` / `disable` — set pending enabled flag (double-buffered).
- `set_surrounding_text(string text, int cursor, int anchor)` — store; lets the IME
  see context (we forward to Android as `setComposingRegion`-less context; optional
  for v1 of this feature — store but may ignore initially).
- `set_text_change_cause(uint cause)` — store/ignore.
- `set_content_type(uint hint, uint purpose)` — drives the Android `inputType`.
- `set_cursor_rectangle(int x, int y, int w, int h)` — surface-local caret rect.
- `commit` — **atomically apply** pending → current; increment per-object serial;
  this is the trigger that fires the Android upcall and sends `.done(serial)`.
- `destroy` — destructor.

Events we send to the guest:
- `enter(wl_surface)` / `leave(wl_surface)` — follow keyboard focus.
- `commit_string(string)` — committed text from the IME.
- `preedit_string(string, int cursor_begin, int cursor_end)` — composing text.
- `delete_surrounding_text(uint before, uint after)`.
- `done(uint serial)` — apply the batched events; serial == #commit requests seen.

### 2.3 Generating / hand-rolling the server glue

There is **no `wayland-scanner` on the build host** (third_party/VENDORING.md:88).
Two options; the doc recommends **(A)** for the owner, with **(B)** as the no-toolchain
fallback.

**(A) Regenerate with the vendored scanner (matches how wayland/xdg-shell glue was
made).** Add to `app/src/main/cpp/CMakeLists.txt` next to the existing
`wayland-protocol.c` / `xdg-shell-protocol.c` lines (125–126):

```
    # (regenerate once, check the outputs into wayland_generated/, like the others)
    #   W=third_party/protocols/text-input/text-input-unstable-v3.xml
    #   ./wayland-scanner server-header $W text-input-unstable-v3-server-protocol.h
    #   ./wayland-scanner private-code  $W text-input-unstable-v3-protocol.c
    ${ALR_TP}/wayland_generated/text-input-unstable-v3-protocol.c   # add to wayland_server sources
```
(build-host recipe identical to VENDORING.md:98–113, just with the new `$W`.)

**(B) Hand-roll the server glue (no scanner needed).** The protocol is tiny: 2
interfaces, 2 + 9 requests, 5 events. The owner writes a ~120-line
`alr_text_input_protocol.c` (NEW, owner-created) declaring:
- the `wl_message` request/event arrays + `wl_interface zwp_text_input_v3_interface`
  and `zwp_text_input_manager_v3_interface` (signatures: `enable`/`disable`/`commit`
  = `""`, `set_content_type` = `"uu"`, `set_cursor_rectangle` = `"iiii"`,
  `set_surrounding_text` = `"sii"`, `get_text_input` = `"no"` with the right
  `wl_interface*` table; events `enter`/`leave` = `"o"`, `commit_string` = `"?s"`,
  `preedit_string` = `"?sii"`, `delete_surrounding_text` = `"uu"`, `done` = `"u"`).
- thin `static inline` senders mirroring `wl_keyboard_send_*`.

Either way the `*_interface` symbols + send helpers become available to
`alr_compositor.cpp`. The rest of this doc assumes those names
(`zwp_text_input_manager_v3_interface`, `zwp_text_input_v3_send_enter`,
`..._send_commit_string`, `..._send_done`, etc.).

### 2.4 Where it plugs into the existing compositor

- **Global registration** mirrors `wl_data_device_manager` exactly. Add a
  `g_text_input_manager_` member + a `bind_text_input_manager` static, created in
  `Compositor::register_globals()` (alr_compositor.cpp:1921) right after the
  `g_data_device_manager_` line (1933).
- **Per-object state** lives on the `SurfaceState` of the focused surface (struct at
  alr_compositor.cpp:91). Add the fields shown in §4 so enable/cursor-rect travel with
  the window and are cleared on unmap (the file already clears focus there).
- **Resource list:** keep a `std::vector<wl_resource*> g_text_inputs;` next to
  `g_keyboards` (alr_compositor.cpp:330) — there can be several text-input objects
  (one per client). `enter`/`leave`/`commit_string`/`done` are sent to the text-input
  objects **whose client owns the focused surface**.

### 2.5 Focus follow (reuse existing machinery)

`zwp_text_input_v3.enter`/`leave` must track keyboard focus. The compositor already
recomputes keyboard focus in exactly two spots; hook both:

- `focus_follow_to_toplevel()` (alr_compositor.cpp:1134) — when a tap raises a
  different toplevel. Where it sends `wl_keyboard_send_leave` to the old focus
  (1148–1152) and arms a fresh enter, also send `zwp_text_input_v3.leave(old)` to that
  client's text-inputs and (on the next commit) `enter(new)`. Because enable state is
  per-surface, a `leave` also forces an IME hide if the new surface has no enabled
  text input.
- the `InjectKind::Key` enter block (alr_compositor.cpp:2222–2227) already lazily
  sends `wl_keyboard.enter` on the first key after a focus change; mirror that for
  text-input: lazily send `zwp_text_input_v3.enter` to the focused client's
  text-inputs when focus changed. Simpler: send `enter`/`leave` from a single helper
  `ti_set_focus(wl_resource* new_surface)` called from both the keyboard focus paths
  and from `toplevel_destroy`/`popup_resource_destroy` (where focus is already
  reset — alr_compositor.cpp:1247, 1390).

### 2.6 Fallback heuristic (no toolkit support)

If a guest **never** binds `zwp_text_input_manager_v3` (SDL2, foot, raw apps) the
soft keyboard is still reachable two ways, in priority order:

1. The existing **manual long-press** (`setOnLongClickListener`, MainActivity.kt:1276)
   stays as the universal escape hatch.
2. **Heuristic auto-raise:** the compositor can offer
   `alr_ime_focus_has_text_input()` (false for these apps); MainActivity uses it to
   decide whether a tap should auto-raise the IME. For non-text-input clients we keep
   today's behavior (no auto-raise on tap, MainActivity.kt:1263–1271) to avoid the
   keyboard covering menus. Optionally a *terminal heuristic*: a client whose only
   toplevel has `content_purpose == terminal` (foot sets it via v3 when present)
   could opt in. Net: no regression for non-IME apps; IME apps light up
   automatically.

---

## 3. (b) Android side — InputConnection on the SurfaceView + show/hide

### 3.1 Make the SurfaceView an editor

The SurfaceView is already `isFocusable`/`isFocusableInTouchMode`
(MainActivity.kt:1235–1236). To receive soft-keyboard text it must return an
`InputConnection`. Override `onCreateInputConnection` in the `SurfaceView` subclass /
anonymous instance (MainActivity.kt:1232):

```kotlin
val surfaceView = object : SurfaceView(this) {
    override fun onCheckIsTextEditor(): Boolean = imeWanted   // gates whether the IME treats us as an editor
    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        outAttrs.inputType = imeInputType            // derived from guest content_purpose/hint (§3.3)
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN or
                              EditorInfo.IME_FLAG_NO_EXTRACT_UI or
                              EditorInfo.IME_ACTION_NONE
        // fullEditor=false: we have no local text buffer; we relay edits to the guest.
        return AlrInputConnection(this, /*fullEditor=*/false)
    }
}.apply { /* existing listeners unchanged */ }
```

`AlrInputConnection : BaseInputConnection` (NEW small class, MainActivity-owned; ≈40
lines) overrides:

| InputConnection method                         | Action |
|------------------------------------------------|--------|
| `commitText(text, newCursorPosition)`          | `nativeWaylandImeCommitText(text.toString())`; return true |
| `setComposingText(text, newCursorPosition)`    | `nativeWaylandImePreedit(text.toString(), cursor)`; return true |
| `finishComposingText()`                        | `nativeWaylandImePreedit("", 0)` (clear preedit); return true |
| `deleteSurroundingText(before, after)`         | convert char→UTF-8 byte counts; `nativeWaylandImeDeleteSurrounding(beforeBytes, afterBytes)`; return true |
| `sendKeyEvent(event)`                          | route HARDWARE-style keys (Enter/Backspace/arrows) via the existing `androidKeyToEvdev`+`nativeWaylandInjectKey` path (§6) |
| `performEditorAction(actionCode)`              | inject KEY_ENTER (evdev 28) down+up |

`super(targetView, false)` (a non-full editor) is correct: a soft keyboard that asks
for surrounding text gets an empty buffer, which IMEs handle (they just won't show
suggestions based on prior text — acceptable for v1; can be upgraded later by feeding
`set_surrounding_text` back, §7).

### 3.2 Show / hide driven by the guest

Replace the manual-only show path with one driven by the compositor upcall. Keep the
long-press as a manual override. The JNI registers a callback (§4) that arrives on the
**compositor thread**, so the handler must hop to the UI thread:

```kotlin
// called from JNI (compositor thread) — see runtime_report.cpp wiring (§4)
@Keep fun onGuestImeState(enabled: Boolean, purpose: Int, hint: Int,
                          curX: Int, curY: Int, curW: Int, curH: Int) {
    runOnUiThread {
        imeWanted = enabled
        imeInputType = imeInputTypeFor(purpose, hint)   // §3.3
        val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        if (enabled) {
            surfaceView.requestFocus()
            imm.restartInput(surfaceView)               // re-read inputType/editor flag
            imm.showSoftInput(surfaceView, InputMethodManager.SHOW_IMPLICIT)
        } else {
            imm.hideSoftInputFromWindow(surfaceView.windowToken, 0)
        }
    }
}
```

`restartInput` is essential: it makes the framework re-call `onCreateInputConnection`
so a new `inputType` (e.g. switching from a normal field to a password field) takes
effect, and re-evaluates `onCheckIsTextEditor`.

### 3.3 content_purpose/hint → Android inputType

```kotlin
private fun imeInputTypeFor(purpose: Int, hint: Int): Int {
    var t = when (purpose) {            // zwp_text_input_v3 content_purpose
        2, 9          -> InputType.TYPE_CLASS_NUMBER                       // digits, pin
        3             -> InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_SIGNED or InputType.TYPE_NUMBER_FLAG_DECIMAL
        4             -> InputType.TYPE_CLASS_PHONE                        // phone
        5             -> InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI            // url
        6             -> InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS  // email
        8             -> InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD       // password
        10,11,12      -> InputType.TYPE_CLASS_DATETIME                     // date/time/datetime
        else          -> InputType.TYPE_CLASS_TEXT                        // normal/alpha/name/url-less
    }
    if (hint and 0x200 != 0) t = t or InputType.TYPE_TEXT_FLAG_MULTI_LINE        // multiline
    if (hint and 0x80  != 0) t = t or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS    // sensitive_data
    if (hint and 0x4   != 0) t = t or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES     // auto_capitalization
    return t
}
```

### 3.4 Cursor rectangle (optional, keeps caret visible)

If `curW >= 0`, call `imm.updateCursorAnchorInfo` / set the view's
`cursorAnchorInfo` so the IME candidate bar doesn't cover the caret. v1 may skip this;
fields not covered by the keyboard (top half of the screen) work without it.

---

## 4. (c) Routing IME commits back — the JNI boundary

### 4.1 New native methods (MainActivity.kt `external fun`, next to line 2806)

```kotlin
private external fun nativeWaylandImeCommitText(text: String)
private external fun nativeWaylandImePreedit(text: String, cursorByte: Int)
private external fun nativeWaylandImeDeleteSurrounding(beforeBytes: Int, afterBytes: Int)
private external fun nativeWaylandImeFocusHasTextInput(): Boolean
private external fun nativeWaylandImeRegisterStateCallback()   // wires onGuestImeState upcall
```

### 4.2 JNI bridge (runtime_report.cpp — MAIN owner; mirrors nativeWaylandInjectKey at 6620)

The compositor↔Android boundary is the NEW header `alr_text_input.hpp` (this branch).
`runtime_report.cpp` includes it and adds, next to the existing inject bridges:

```cpp
#include "alr_wayland/alr_text_input.hpp"

extern "C" JNIEXPORT void JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeWaylandImeCommitText(
        JNIEnv* env, jobject, jstring jtext) {
#ifdef ALR_HAVE_WAYLAND
    const char* s = env->GetStringUTFChars(jtext, nullptr);   // modified UTF-8; OK for BMP+
    if (s) { alr::wayland::alr_ime_commit_text(s, (int32_t)std::strlen(s));
             env->ReleaseStringUTFChars(jtext, s); }
#endif
}
// nativeWaylandImePreedit -> alr_ime preedit (commit_string path uses preedit then commit)
// nativeWaylandImeDeleteSurrounding -> alr::wayland::alr_ime_delete_surrounding(before,after)
// nativeWaylandImeFocusHasTextInput -> return alr::wayland::alr_ime_focus_has_text_input();
```

> **UTF-8 caveat:** `GetStringUTFChars` returns *modified* UTF-8 (CESU-8 for
> astral-plane chars: emoji become surrogate-pair-encoded). For emoji-correct
> behavior the owner should instead `GetStringChars` (UTF-16) and convert to true
> UTF-8 in C++, or convert to a `ByteArray` in Kotlin (`text.toByteArray(UTF_8)`) and
> pass `jbyteArray`. The doc recommends the **Kotlin `ByteArray`** route to avoid the
> CESU-8 trap; the header's `alr_ime_commit_text(const char*, len)` takes real UTF-8
> either way.

### 4.3 State-callback upcall (compositor → Kotlin)

`nativeWaylandImeRegisterStateCallback` calls
`alr::wayland::alr_ime_set_state_callback(&ime_state_trampoline, g_jvm_ctx)`. The
trampoline (in runtime_report.cpp) is invoked **on the compositor thread**; it must
`AttachCurrentThread`, find the cached `MainActivity` jobject + `onGuestImeState`
methodID, and call it (the Kotlin side hops to the UI thread itself via
`runOnUiThread`). Caching: store the `JavaVM*` and a `NewGlobalRef` to the activity in
`nativeWaylandImeRegisterStateCallback`; `DeleteGlobalRef` on stop. This mirrors the
single-callback pattern in `alr_text_input.hpp` (`alr_ime_set_state_callback`).

---

## 5. Direct-text vs keymap tradeoff (the crux of routing)

Two ways to deliver typed characters to the guest. We use **both, by guest
capability**:

| | Direct text (`zwp_text_input_v3.commit_string`) | Keymap (`wl_keyboard.key` + xkb) |
|---|---|---|
| Works for | GTK/Qt/chromium (bound v3) | every guest with a keyboard, incl. SDL/foot/raw |
| Character set | **any Unicode** (emoji, CJK, autocorrect) verbatim | only what the shipped US keymap can produce (no IME, no CJK) |
| Preedit/composing | native (`preedit_string`) | none |
| Modifiers | N/A (text is literal) | must synthesize Shift/AltGr correctly |
| Used when | `alr_ime_focus_has_text_input()` == true | fallback / non-v3 guests / hardware keys |

**Decision:** when the focused guest has an enabled `zwp_text_input_v3`, the
`AlrInputConnection.commitText` path sends `commit_string` (Unicode-exact). When it
does **not** (SDL/foot, or a v3 client that disabled), `commitText` must be degraded
to key events:

- For ASCII/Latin-1 reachable on the shipped keymap, `alr_ime_inject_codepoint(cp)`
  finds the `(keycode, shift-level)` on `alr_xkb_keymap_us.h` and injects a synthetic
  `wl_keyboard.key` press+release (with a transient Shift modifier where needed),
  reusing `InjectKind::Key` + the modifier machinery (alr_compositor.cpp:2209–2241).
- For code points **not** on the US keymap (é, 한, emoji) with no text-input guest,
  there is no faithful key path. Options, in order of preference:
  1. Just drop to `commit_string` **anyway if any v3 object exists** (most GUI apps
     do bind it even when momentarily disabled).
  2. A future *dynamic keymap remap*: rewrite one spare keysym to the desired Unicode,
     re-send `wl_keyboard.keymap` (a new memfd), inject that key, then restore. This is
     the standard "wlroots virtual-keyboard / `xdotool type`" trick. **Out of scope
     for v1** (documented as the upgrade path); foot/SDL users type ASCII fine, which
     covers terminals and most games.

`set_cursor_rectangle` units: it is **surface-local** (logical px). The compositor
already maps output↔surface coordinates (`map_input_to_surface`,
alr_compositor.cpp:1107); to give Android a useful rect, transform the guest's
surface-local rect to SurfaceView px with the inverse of that mapping (multiply by the
on-screen rect / buffer ratio). All-`-1` when the client sent none.

---

## 6. IME composing text + hardware-keyboard coexistence

**Composing (preedit).** A soft IME (CJK, glide) calls
`InputConnection.setComposingText` repeatedly, then `commitText` once. Map:
`setComposingText` → `alr_ime` preedit → `zwp_text_input_v3.preedit_string(text,
0, len)` + `done`; `finishComposingText`/`commitText` → clear preedit
(`preedit_string("",0,0)`) then `commit_string(text)` + `done`. The guest widget shows
the underlined composing run live, exactly as on a Linux desktop. Per the protocol,
`preedit_string`/`commit_string`/`delete_surrounding_text` are double-buffered and
**applied on `done`** — always send `done` after a batch (the header's
`alr_ime_commit_text` does this internally; preedit + commit in one user action should
be coalesced into a single `done`).

**Hardware keyboard coexistence.** A physical/Bluetooth keyboard (or the soft
keyboard's Enter/Backspace/arrows) must still reach the guest as real keys:
- The existing `setOnKeyListener` → `androidKeyToEvdev` → `nativeWaylandInjectKey`
  path (MainActivity.kt:1286–1297) is **unchanged** and remains the route for
  hardware keys and navigation keys.
- `AlrInputConnection.sendKeyEvent` (which soft keyboards use for Enter/Del/arrows)
  forwards to the **same** `androidKeyToEvdev`+`nativeWaylandInjectKey` path, so those
  become genuine `wl_keyboard` keys (the guest gets KEY_ENTER/KEY_BACKSPACE, not a
  text edit). Printable characters that a hardware keyboard produces go through
  `wl_keyboard` (US keymap) as today; soft-IME text goes through `commit_string`.
  These are complementary streams to the same focused surface and do not conflict:
  `wl_keyboard` events carry the physical key; `commit_string` carries composed text.
- One ordering rule: when a `commit_string` is in flight, do **not** also synthesize a
  key for the same character. `AlrInputConnection` decides per-method: `commitText`
  → text path; `sendKeyEvent` → key path. They never fire for the same input.

---

## 7. Owner-wiring summary (exact minimal edits)

### 7.1 `alr_compositor.cpp` (WS-3)

1. `#include "alr_wayland/alr_text_input.hpp"` near the other includes (≈line 27),
   plus the generated/hand-rolled `text-input-unstable-v3-server-protocol.h`.
2. **SurfaceState** (struct at line 91): add
   ```cpp
   // zwp_text_input_v3 per-surface state (double-buffered: pending_* applied on commit)
   bool     ti_enabled = false;       bool ti_pending_enabled = false;
   uint32_t ti_purpose = 0, ti_hint = 0, ti_pending_purpose = 0, ti_pending_hint = 0;
   int32_t  ti_cur_x=-1, ti_cur_y=-1, ti_cur_w=-1, ti_cur_h=-1;   // surface-local
   uint32_t ti_serial = 0;            // #commit requests on the focused text-input
   ```
3. Add globals next to `g_keyboards` (line 330):
   `std::vector<struct wl_resource*> g_text_inputs;` and a
   `AlrImeStateFn g_ime_state_cb = nullptr; void* g_ime_state_ud = nullptr;`.
4. **Implement** the `zwp_text_input_v3` request handlers + the
   `bind_text_input_manager` global, modeled byte-for-byte on the
   `wl_data_device_manager` block (alr_compositor.cpp:1720–1761, 1871–1880). The
   `commit` handler:
   ```cpp
   void text_input_commit(wl_client*, wl_resource* ti) {
       auto* s = focused_surface_state();            // SurfaceState of g_focus_surface
       if (s) { s->ti_enabled = s->ti_pending_enabled;
                s->ti_purpose = s->ti_pending_purpose; s->ti_hint = s->ti_pending_hint; }
       zwp_text_input_v3_send_done(ti, ++s_serial_for(ti));
       AlrImeState st{ s && s->ti_enabled, s?s->ti_purpose:0, s?s->ti_hint:0,
                       /*cursor rect mapped to output px*/... };
       if (g_ime_state_cb) g_ime_state_cb(st, g_ime_state_ud);   // upcall (this thread)
   }
   ```
5. Register the global in `register_globals()` (line 1933, after data_device_manager):
   ```cpp
   g_text_input_manager_ = wl_global_create(display_, &zwp_text_input_manager_v3_interface,
                                             1, this, bind_text_input_manager);
   ```
   and add `g_text_input_manager_` to the null-check at line 1936 and the status string
   at 2028 (`",zwp_text_input_manager_v3"`).
6. Send `enter`/`leave` from the keyboard-focus transitions: in
   `focus_follow_to_toplevel` (1148), `toplevel_destroy` (1247), and the lazy enter in
   the `InjectKind::Key` block (2222) — via a `ti_set_focus(new_surface)` helper that
   also fires a synthesized disable upcall when focus leaves an enabled field.
7. **Implement** the `alr_text_input.hpp` C++ functions at the bottom near
   `enqueue_inject` (line 2450): `alr_ime_set_state_callback` (store cb+ud);
   `alr_ime_commit_text`/`alr_ime_delete_surrounding`/`alr_ime_inject_codepoint`
   enqueue onto `g_inject_queue` (extend `InjectKind` with `ImeCommit`,
   `ImeDelete`, `ImePreedit`) and `wake()`; `drain_input_queue` gets cases that send
   `zwp_text_input_v3_send_commit_string/preedit_string/delete_surrounding_text` +
   `..._send_done` to the focused client's text-inputs;
   `alr_ime_focus_has_text_input()` returns `focused_surface_state()->ti_enabled`.

### 7.2 `MainActivity.kt` (MAIN)

1. Make the SurfaceView an editor: override `onCheckIsTextEditor` +
   `onCreateInputConnection` (returning `AlrInputConnection`) on the instance at
   line 1232; add the small `AlrInputConnection : BaseInputConnection` class (§3.1).
2. Add `onGuestImeState(...)` (§3.2, `@Keep`, hops to UI thread) and the
   `imeInputTypeFor` mapper (§3.3).
3. Declare the new `external fun nativeWaylandIme*` (§4.1) next to line 2806; call
   `nativeWaylandImeRegisterStateCallback()` once, right after
   `nativeWaylandCompositorStart` succeeds in `surfaceCreated`
   (MainActivity.kt:1299, alongside the other native calls).
4. Keep `showSoftKeyboard`/`setOnLongClickListener` as the manual override (§2.6).

### 7.3 `runtime_report.cpp` (MAIN)

Add the five JNI bridges (§4.2) + the `ime_state_trampoline` (§4.3) next to
`nativeWaylandInjectKey` (line 6620). Cache `JavaVM*` + a global ref to the activity
in `nativeWaylandImeRegisterStateCallback`.

### 7.4 `CMakeLists.txt`

Add the generated `text-input-unstable-v3-protocol.c` (or the hand-rolled
`alr_text_input_protocol.c`) to the `wayland_server` sources (line 125) and the
`-server-protocol.h` to the include dir already at line 131.

---

## 8. Test / acceptance (device-pending — this worker cannot run it)

1. **GTK3 entry (gtk3-widget-factory / GIMP "Save As" filename field):** tap the
   field → soft keyboard rises automatically; type "abc한" → all four chars appear,
   "한" via composing; Backspace deletes; tap elsewhere → keyboard hides.
2. **Chromium URL bar:** tap omnibox → keyboard rises with URL inputType (no
   spellcheck, visible `.com`); typed text reaches the page; password fields mask.
3. **Qt6 demo line edit:** same enable/commit loop.
4. **foot terminal (no v3):** long-press raises keyboard; ASCII typing reaches the
   shell via the keymap path; emoji silently drop (documented limitation).
5. **No-regression:** non-text taps still don't raise the keyboard; hardware-key
   path and `nativeWaylandInjectSelfTest` still pass.

## 9. Risks / honest gaps

- **CESU-8/emoji** via `GetStringUTFChars` — must use the `ByteArray`/UTF-16 route
  (§4.2) or emoji corrupt. Flagged, not yet wired.
- **commit_string serial vs done serial** — `done.serial` must equal the count of the
  guest's own `commit` requests, **not** our event count. The compositor must track a
  per-`zwp_text_input_v3`-object request counter (incremented in the `commit`
  handler) and echo it; mismatches make GTK ignore the batch. Called out in §2.2/§7.1.
- **No surrounding-text feedback** in v1 — IMEs that rely on context (strong CJK
  prediction, autocorrect-on-prior-word) degrade gracefully but aren't optimal. Upgrade
  path: feed `set_surrounding_text` back into the `InputConnection` via
  `getTextBeforeCursor`/`getTextAfterCursor` overrides.
- **Keymap-only Unicode ceiling** for non-v3 guests (§5) — terminals/games type ASCII;
  no CJK/emoji without the dynamic-keymap upgrade (deferred).
- **Host-only:** none of the device acceptance (§8) is run here; the spec is
  implementable but unverified on SM-X236N.
