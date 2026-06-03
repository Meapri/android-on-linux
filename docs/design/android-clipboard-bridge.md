# Android ↔ Linux-guest clipboard bridge (design)

Status: DESIGN (host-only; no device verification). Owned NEW path. Branch `auto/host-clipboard`.

Goal: bidirectional clipboard between the Android host and the in-app Wayland guest,
so "copy in Chromium/GIMP → paste in an Android app" and "copy in an Android app →
paste in the guest" both work. Text first (`text/plain;charset=utf-8`, `text/html`),
then `image/png`.

This doc is a ready-to-wire spec. It cites the exact compositor functions and
MainActivity callbacks the owners (WS-3 for `alr_compositor.cpp`, MAIN for
`MainActivity.kt`) extend, the Wayland wire calls (all already present in the
vendored `third_party/wayland_generated/wayland-server-protocol.h`), the Android
API + permission, the JNI boundary, and the async pipe data flow.

The design adds NO new Wayland protocol and NO new generated glue: the standard
core `wl_data_device_manager` / `wl_data_device` / `wl_data_source` / `wl_data_offer`
interfaces (and their `wl_data_*_send_*` server functions) are already vendored and
already bound by GTK/GDK and Chromium. The current compositor implementation of
those interfaces is a deliberate no-op stub (see `alr_compositor.cpp` §
`wl_data_device_manager (clipboard/DnD stub)`, lines ~1720–1761). This design
turns the stub into a real selection (copy/paste) implementation. Drag-and-drop is
explicitly out of scope here (the `start_drag` / DnD `action` machinery stays
stubbed); only the *selection* (Ctrl-C / Ctrl-V) clipboard is wired.

---

## 0. Background — what already exists (verified by reading the code)

Compositor (`app/src/main/cpp/alr_wayland/alr_compositor.cpp`):

- `g_data_device_manager_` global is created at v3 in `Compositor::register_globals()`
  (line ~1933) and bound in `Compositor::bind_data_device_manager()` (line ~1871).
- The three interfaces are stubbed:
  - `kDataSourceImpl` = `{data_source_offer (no-op), data_source_destroy, data_source_set_actions (no-op)}` (line ~1732).
  - `kDataDeviceImpl` = `{data_device_start_drag (no-op), data_device_set_selection (no-op), data_device_release}` (line ~1741).
  - `kDataDeviceManagerImpl` = `{ddm_create_data_source, ddm_get_data_device, ddm_release}` (line ~1760).
  - `ddm_get_data_device()` creates a `wl_data_device` resource but never sends a
    selection / data_offer ("an empty clipboard is valid").
- Keyboard focus is tracked in `g_focus_surface` (the focused toplevel) with
  `g_keyboard_entered`; `g_keyboards` holds the per-client `wl_keyboard` resources.
  The serial source is `wl_display_next_serial(comp->display())`.
- The cross-thread pattern is established: UI/JNI thread enqueues onto a
  `std::mutex`-guarded queue + `Compositor::wake()` (writes the `wakeup_fd_`
  eventfd); the compositor thread drains it inside the epoll reactor
  (`drain_input_queue()` / `drain_gpu_queue()`, dispatched in the epoll loop at
  line ~2321 where `events[i].data.fd == wakeup_fd_`). **All `wl_resource` sends
  must happen on the compositor thread** — this is the load-bearing constraint for
  the whole design.

Android (`app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt`):

- `import android.view.inputmethod.InputMethodManager` already present;
  `ClipboardManager` is NOT yet imported.
- Native methods are `private external fun nativeWayland*` (lines ~2787–2807),
  resolved by JNI name mangling to `Java_dev_chanwoo_androlinux_MainActivity_*` in
  `runtime_report.cpp` (e.g. `nativeWaylandInjectKey` →
  `Java_..._nativeWaylandInjectKey`, runtime_report.cpp line ~6620). There is NO
  `RegisterNatives` table — methods are matched by mangled name, so adding a method
  is: declare `external fun` in Kotlin + add the matching `extern "C" JNIEXPORT`
  in runtime_report.cpp.
- The SurfaceView is built in `onCreate` (line ~1232); touches/keys are forwarded
  via `nativeWaylandInjectTouch` / `nativeWaylandInjectKey`.

JNI helper bridge file: `runtime_report.cpp` already owns the Wayland JNI shims
(lines ~6481–6651) and calls into `alr::wayland::*` free functions. New clipboard
JNI shims go there too (MAIN/integration owns runtime_report.cpp — see Ownership).

---

## 1. Architecture overview

Two independent, asynchronous flows share one compositor-side clipboard model.

```
GUEST → ANDROID  (guest copies, Android pastes)
  guest wl_data_source.offer(mimes) + wl_data_device.set_selection
    → compositor records the guest source as the current selection owner
    → compositor (lazily, on demand) pipes the data out of the guest:
         wl_data_source.send(mime, write_fd) → guest writes → compositor reads read_fd
    → bytes handed to Kotlin via JNI callback → ClipboardManager.setPrimaryClip()

ANDROID → GUEST  (Android copies, guest pastes)
  ClipboardManager.OnPrimaryClipChangedListener fires
    → Kotlin reads the clip (text/html/png) → pushes to native (JNI)
    → compositor creates a SERVER-OWNED wl_data_offer on each client's wl_data_device,
      sends wl_data_device.data_offer + offer(mime)* + selection(offer)
    → guest pastes: wl_data_offer.receive(mime, write_fd)
         → compositor writes the Android bytes into write_fd on a worker, closes it
```

The compositor is the single source of truth for "who owns the selection":

```
enum class SelOwner { None, Guest, Android };
SelOwner   g_sel_owner = SelOwner::None;
```

A selection is either Guest-owned (a live `wl_data_source` from a client) or
Android-owned (a server-synthesized offer fed from `ClipboardManager`). Whenever
one side takes ownership, the other side's offer is replaced/cancelled. This mirror
of "last writer wins" is exactly how a real Wayland compositor (weston/mutter)
bridges X11/GTK selections, so guest toolkits behave correctly.

---

## 2. MIME negotiation

Canonical MIME set, in preference order, shared by both directions:

| Wayland MIME                  | Android `ClipDescription` MIME / `ClipData.Item` | Notes |
|-------------------------------|--------------------------------------------------|-------|
| `text/plain;charset=utf-8`    | `text/plain` (`coerceToText`)                     | primary text path |
| `text/plain`                  | `text/plain`                                      | alias offered for older clients |
| `UTF8_STRING`                 | `text/plain`                                      | offered as alias; some GTK apps request it |
| `text/html`                   | `text/html` (`ClipData.Item.getHtmlText()`)       | rich text, optional |
| `image/png`                   | `image/png` (content-URI item, see §6)            | phase 2 |

Negotiation rules:

- **Guest → Android**: the compositor receives the full mime list from the guest's
  `wl_data_source.offer(mime)` calls (one call per mime, accumulated until
  `set_selection`). The compositor picks the *richest mime it can map to Android*,
  in this priority: `image/png` > `text/html` > `text/plain;charset=utf-8` >
  `text/plain` > `UTF8_STRING`. It requests exactly that one mime from the guest
  (one `wl_data_source.send`) and builds the matching `ClipData`.
- **Android → Guest**: the compositor advertises, on the synthesized
  `wl_data_offer`, the set of mimes it can satisfy from the current Android clip:
  always `text/plain;charset=utf-8` + `text/plain` + `UTF8_STRING` when the clip
  has text; additionally `text/html` if `ClipDescription.hasMimeType("text/html")`;
  additionally `image/png` if the clip is an image (phase 2). The guest's toolkit
  then `receive()`s whichever mime it prefers.
- Charset: all text is normalized to UTF-8 at the boundary. Android `CharSequence`
  is UTF-16 internally; convert with `toString().toByteArray(Charsets.UTF_8)`.
  Guest bytes for `text/plain;charset=utf-8` are taken verbatim; for bare
  `text/plain` / `UTF8_STRING` they are still treated as UTF-8 (the only encoding
  modern Linux toolkits emit).

---

## 3. Async pipe-based `wl_data` transfer

Wayland selection transfer is pull-based over an anonymous `pipe2()`. The receiver
passes the **write** end; the sender writes and closes; the receiver reads the
**read** end to EOF. Two cases:

### 3a. Reading OUT of the guest (guest owns selection, Android needs the bytes)

When Android requests the current clipboard (lazy; see §4 trigger), on the
compositor thread:

```c
int fds[2]; pipe2(fds, O_CLOEXEC | O_NONBLOCK);
wl_data_source_send_send(g_guest_source, chosen_mime, fds[1]);  // guest gets write end
close(fds[1]);                                                  // compositor keeps read end = fds[0]
wl_client_flush(client_of(g_guest_source));                    // push the event now
// hand fds[0] to a reader (see below)
```

The read side must NOT block the compositor thread (the guest writes from its own
thread and only after it next dispatches). Options:

- **Preferred**: register `fds[0]` with the compositor's own `wl_event_loop` via
  `wl_event_loop_add_fd(loop_, fds[0], WL_EVENT_READABLE, on_clip_readable, ctx)`.
  The handler appends to a `std::string` buffer until `read()` returns 0 (EOF) or
  `EAGAIN`; on EOF it removes the source, closes the fd, and posts the assembled
  bytes to Android (§5). This keeps everything on the compositor thread, no extra
  threads, and is exactly how the reactor already folds fds.
- A 256 KiB cap + a 2 s watchdog timer (reuse the existing frame-timer fd pattern)
  guards against a guest that opens the pipe but never writes; on timeout the
  source is torn down and an empty/oldvalue result is returned.

### 3b. Writing INTO the guest (Android owns selection, guest pastes)

When the guest calls `wl_data_offer.receive(mime, write_fd)` on a server-owned
offer, the request lands in the compositor's `data_offer_receive` handler (compositor
thread). The Android bytes for that mime are already cached in a compositor-owned
`std::string` (pushed by Kotlin, §5b). Write them to `write_fd`:

- If the payload is small (text, the common case ≤ a pipe buffer, 64 KiB), a single
  non-blocking `write()` usually completes; close the fd. 
- For correctness with large payloads (html/png) register `write_fd` with
  `wl_event_loop_add_fd(loop_, write_fd, WL_EVENT_WRITABLE, on_clip_writable, ctx)`
  and drain a per-transfer offset until done, then close. The cached bytes are
  ref-counted/copied per in-flight transfer so a clip change mid-paste can't free
  them underneath the writer.

No blocking, no busy-wait; both directions are driven by the existing epoll/wl_event_loop
reactor.

---

## 4. GUEST → ANDROID flow (detail)

### 4a. Compositor: make `wl_data_source` / `set_selection` real

Replace the no-op stubs. New compositor-thread state (file-scope, near the other
`g_*` clipboard state):

```c
struct wl_resource* g_guest_source   = nullptr;  // current guest-owned wl_data_source (or null)
std::vector<std::string> g_guest_mimes;          // mimes the guest source advertised
uint32_t g_sel_serial = 0;                        // serial of the owning set_selection
```

Handlers:

- `data_source_offer(client, src, mime)` — **was no-op**. Now: append `mime` to a
  per-source mime vector stored as the resource's user_data (allocate a small
  `DataSourceState{ std::vector<std::string> mimes; }` in `ddm_create_data_source`
  and set it as the source's user_data + a destroy listener that frees it and, if
  this source is `g_guest_source`, clears the selection).
- `data_device_set_selection(client, dev, source, serial)` — **was no-op**. Now:
  - If `source == nullptr` → guest cleared its selection: if `g_sel_owner==Guest`,
    set `g_sel_owner=None`, `g_guest_source=nullptr`, and notify Android the guest
    cleared (optional — we generally do NOT clobber the Android clipboard on guest
    clear; see §7 loop-guard).
  - Else record `g_guest_source = source`, copy its mimes into `g_guest_mimes`,
    `g_sel_owner = Guest`, `g_sel_serial = serial`. Then notify Android there is a
    new guest selection (`alr_clip_on_guest_selection(mimes)` JNI up-call, §5a) so
    Android can pull lazily — OR pull eagerly here (simpler; see trade-off §7).
  - Cancel any previously-owning guest source: `wl_data_source_send_cancelled(old)`.

The `wl_data_source` resource lives in the guest client; never deref it after its
`destroy` listener fires — match the project's UAF discipline (the destroy listener
nulls `g_guest_source`).

### 4b. Pull bytes (lazy, on Android demand) — §3a pipe read.

The chosen-mime selection logic (§2) runs here; the assembled `std::string` is
handed to Kotlin.

### 4c. Kotlin: write to the Android clipboard

`runOnUiThread { clipboard.setPrimaryClip(ClipData.newPlainText("ALR", text)) }`
(or `newHtmlText` / image item). `ClipboardManager` is a UI-thread-affine system
service; the JNI up-call (§5) marshals to the main thread.

---

## 5. JNI boundary (the new bridge)

JNI shims live in `runtime_report.cpp` (same place as the existing
`nativeWayland*`), calling new `alr::wayland::*` free functions declared in
`alr_compositor.hpp` and implemented in `alr_compositor.cpp`. Up-calls
(native→Kotlin) use a cached `jclass`/`jmethodID` + `JavaVM*` (the loader already
holds a `JavaVM*`; if not cached, capture it in `JNI_OnLoad` or on first
`nativeWaylandCompositorStart`).

### 5a. native → Kotlin (guest selection available)

Kotlin callback methods on `MainActivity` (called from native; must be
non-`private` or have a stable signature for `GetMethodID`):

```kotlin
// Called by native (compositor thread → marshalled) when the GUEST sets a new
// selection. mimes is a comma-joined list. Kotlin decides whether/what to pull.
@Keep fun onGuestClipboardOffer(mimes: String)

// Called by native after it has pulled the guest bytes for `mime`.
// `utf8` for text mimes; `pngBytes` (ByteArray) for image/png.
@Keep fun onGuestClipboardText(mime: String, utf8: String)
@Keep fun onGuestClipboardImage(pngBytes: ByteArray)
```

Native side (in `alr_compositor.cpp`, marshalled via `JavaVM->AttachCurrentThread`
if the post happens off the JVM-attached compositor thread — the compositor thread
is a raw pthread, so it MUST attach; cache the `jobject` MainActivity global ref):

```c
// in alr_compositor.hpp:
namespace alr::wayland {
  // Owner installs these so the compositor can reach Android. Each takes ownership
  // of nothing; strings are copied. Called on the compositor thread.
  using GuestOfferCb = std::function<void(const std::vector<std::string>& mimes)>;
  using GuestTextCb  = std::function<void(const std::string& mime, const std::string& utf8)>;
  using GuestImageCb = std::function<void(const std::string& png_bytes)>;
  void alr_wayland_set_clipboard_sink(GuestOfferCb, GuestTextCb, GuestImageCb);
}
```

The JNI shim in runtime_report.cpp installs lambdas that JNI-call the Kotlin
`onGuestClipboard*` methods (attaching the thread, building the `jstring`/`jbyteArray`,
calling, detaching). This keeps `alr_compositor.cpp` free of any JNI/`<jni.h>`
dependency — it only knows `std::function`s, exactly like `present` / `present_list`
already do in `CompositorConfig`.

### 5b. Kotlin → native (Android clip changed; push the offer)

```kotlin
// MainActivity → native: Android primary clip changed. Pass the mimes Kotlin can
// satisfy + the actual payloads (so native can answer wl_data_offer.receive without
// re-entering the JVM). Empty/clear => pass empty mimes.
external fun nativeWaylandClipboardSetAndroid(
    mimes: Array<String>,      // e.g. ["text/plain;charset=utf-8","text/plain","UTF8_STRING"]
    utf8Text: String?,         // for text mimes (null if none)
    htmlText: String?,         // for text/html (null if none)
    pngBytes: ByteArray?,      // for image/png (null if none)
)
```

JNI shim → `alr::wayland::alr_wayland_set_android_selection(mimes, text, html, png)`,
which enqueues onto a clipboard queue + `wake()` (mirroring `enqueue_inject`). On the
compositor thread it caches the payloads (one `std::string` per mime) and, for each
client that has a `wl_data_device`, synthesizes a server offer (§4d below). The cached
payloads are what §3b writes into the guest's pipe on `wl_data_offer.receive`.

### 5c. JNI method-name mangling (concrete)

Declared `private external fun nativeWaylandClipboardSetAndroid(...)` →
`extern "C" JNIEXPORT void JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeWaylandClipboardSetAndroid(JNIEnv*, jobject,
jobjectArray mimes, jstring utf8Text, jstring htmlText, jbyteArray pngBytes)` in
runtime_report.cpp. The reverse callbacks (`onGuestClipboard*`) are resolved with
`env->GetMethodID(MainActivity_class, "onGuestClipboardText",
"(Ljava/lang/String;Ljava/lang/String;)V")` etc.

---

## 6. ANDROID → GUEST flow (detail)

### 6a. Kotlin: listen for clip changes

```kotlin
import android.content.ClipboardManager
import android.content.ClipData
import android.content.ClipDescription

private val clipboard by lazy { getSystemService(CLIPBOARD_SERVICE) as ClipboardManager }
private val clipListener = ClipboardManager.OnPrimaryClipChangedListener {
    if (suppressClipEcho) return@OnPrimaryClipChangedListener   // §7 loop-guard
    val clip = clipboard.primaryClip ?: run {
        nativeWaylandClipboardSetAndroid(emptyArray(), null, null, null); return@OnPrimaryClipChangedListener
    }
    val desc = clip.description
    val item = clip.getItemAt(0)
    val text = item.coerceToText(this).toString()
    val html = if (desc.hasMimeType(ClipDescription.MIMETYPE_TEXT_HTML)) item.htmlText else null
    val png  = readPngIfImage(item)   // §6c (phase 2)
    val mimes = buildList {
        if (text.isNotEmpty()) { add("text/plain;charset=utf-8"); add("text/plain"); add("UTF8_STRING") }
        if (html != null) add("text/html")
        if (png != null) add("image/png")
    }.toTypedArray()
    nativeWaylandClipboardSetAndroid(mimes, text.ifEmpty { null }, html, png)
}
```

Registered in `onCreate` (after the compositor is started) with
`clipboard.addPrimaryClipChangedListener(clipListener)` and removed in `onDestroy`
with `removePrimaryClipChangedListener`. On Android 10+ (the device is Android 16)
`primaryClip` reads are only allowed when the app has focus / is the default IME /
holds the clip — the app is foreground when its SurfaceView is up, so reads
succeed; clip-change events still fire while foregrounded. No special permission is
needed for the *primary* clipboard (see §8).

### 6b. Compositor: synthesize a server-owned offer + push selection

New compositor function (called from the clipboard-queue drain on the compositor
thread), for the focused client (or all clients with a `wl_data_device`):

```c
// pseudo — for the data_device 'dev' of client 'c':
struct wl_resource* offer = wl_resource_create(c, &wl_data_offer_interface,
    wl_resource_get_version(dev), 0 /*new id, server-allocated*/);
wl_resource_set_implementation(offer, &kDataOfferImpl, /*user_data tag = Android-offer*/, on_offer_destroy);
wl_data_device_send_data_offer(dev, offer);                // introduce the new offer
for (auto& m : android_mimes) wl_data_offer_send_offer(offer, m.c_str());
wl_data_device_send_selection(dev, offer);                 // make it the selection
wl_client_flush(c);
```

The `kDataOfferImpl` (NEW) implements the three `wl_data_offer` requests:
`accept` (no-op for selection), `receive(mime, fd)` → §3b write path,
`destroy`/`finish` → resource cleanup. `set_actions` is a no-op (selection, not DnD).

Tracking: keep `std::vector<wl_resource*> g_data_devices` (populated in
`ddm_get_data_device`, pruned by a destroy listener) so a new Android clip can be
broadcast to every connected guest's data_device. The current `ddm_get_data_device`
already creates the resource — add `g_data_devices.push_back(res)` + a destroy
listener there.

When to push: (1) on every `OnPrimaryClipChangedListener` while a guest is
connected, AND (2) whenever a client newly binds a `wl_data_device`
(`ddm_get_data_device`) if `g_sel_owner==Android` — so a guest started after the
copy still sees the current Android selection. Per Wayland, a fresh selection is
also re-announced to a client when its keyboard gains focus; we re-broadcast on
focus change for robustness (cheap: a few wire events).

### 6c. Image path (phase 2)

Android image clips are content-URI items (`item.uri`,
`desc.hasMimeType("image/*")`). Read the bytes via
`contentResolver.openInputStream(uri)`, re-encode to PNG if needed
(`Bitmap.compress(PNG)`), pass as `pngBytes`. Guest receives `image/png` over the
pipe unchanged. Cap at ~8 MiB; above that, advertise text only.

---

## 7. Loop-guard (the critical correctness issue)

Without a guard, Guest→Android (`setPrimaryClip`) fires
`OnPrimaryClipChangedListener`, which pushes back into the guest as a new Android
selection, which can re-trigger... a feedback loop and, worse, a fight between the
guest's own source and the echoed Android offer.

Guard design:

- **Kotlin echo suppression**: a `@Volatile var suppressClipEcho = false`. Before
  the JNI up-call result calls `setPrimaryClip`, set `suppressClipEcho = true`,
  call `setPrimaryClip`, then post `suppressClipEcho = false` to the end of the
  main looper (`Handler(mainLooper).post { suppressClipEcho = false }`). The
  listener early-returns while suppressed. This stops Guest→Android→Guest echo.
- **Compositor ownership guard**: when `g_sel_owner==Guest`, an incoming Android
  push is the *echo of the guest's own copy* — drop it (compare a content hash:
  the compositor stores `g_last_guest_hash` of what it sent to Android; if the
  Android push hashes equal, ignore). When the user genuinely copies in an Android
  app, the hash differs → ownership flips to Android, and the compositor sends
  `wl_data_source_send_cancelled(g_guest_source)` so the guest drops its stale
  selection.
- We do NOT mirror "guest cleared selection" → "clear Android clipboard" (a guest
  app losing focus often clears its selection; clearing the user's Android
  clipboard for that would be hostile). Clearing only propagates Android→guest.

---

## 8. Android API + permissions

| Concern | API | Permission |
|---|---|---|
| Read/write primary clip | `android.content.ClipboardManager` (`getSystemService(CLIPBOARD_SERVICE)`) | **none** — primary clipboard needs no manifest permission |
| Clip-change notifications | `ClipboardManager.OnPrimaryClipChangedListener` | none (only delivered while app foreground on API 29+) |
| Build text/html clip | `ClipData.newPlainText` / `newHtmlText` | none |
| Image clip read | `ContentResolver.openInputStream(uri)` | none for a URI the system granted the app via the clip; `READ_EXTERNAL_STORAGE` NOT required for clipboard content URIs |

No new manifest permission is required. (Foreground-only clip reads on API 29+ are
acceptable — the app is foreground whenever its compositor SurfaceView is shown.)
No SELinux bypass, public APIs only.

---

## 9. Data flow summary (sequence)

Guest copies "hello":
```
guest: wl_data_source.offer("text/plain;charset=utf-8") ; .offer("UTF8_STRING")
guest: wl_data_device.set_selection(source, serial)
  comp data_device_set_selection: g_guest_source=source; g_sel_owner=Guest; copy mimes
  comp → JNI up-call onGuestClipboardOffer("text/plain;charset=utf-8,UTF8_STRING")
  (lazy or eager) comp: pipe2; wl_data_source.send("text/plain;charset=utf-8", wfd); close wfd; flush
  guest writes "hello" to wfd; comp reads rfd to EOF via wl_event_loop_add_fd
  comp → JNI up-call onGuestClipboardText("text/plain;charset=utf-8","hello")
  Kotlin (UI): suppressClipEcho=true; clipboard.setPrimaryClip(newPlainText("ALR","hello"))
```

Android copies "world":
```
Android app copies → OnPrimaryClipChangedListener (suppressClipEcho=false)
  Kotlin: text="world"; nativeWaylandClipboardSetAndroid(["text/plain;charset=utf-8",...],"world",null,null)
  comp: cache payloads; g_sel_owner=Android; if g_guest_source: wl_data_source.send_cancelled
  comp: for each data_device: create offer; data_offer; offer(mime)*; selection(offer); flush
guest pastes (Ctrl-V):
  guest: wl_data_offer.receive("text/plain;charset=utf-8", wfd)
  comp data_offer_receive: write "world" to wfd (non-blocking / wl_event_loop_add_fd WRITABLE); close
```

---

## 10. Ownership & exact wiring (ownerWiring)

### 10a. `alr_compositor.cpp` (WS-3) — the data_device selection hooks

Replace the no-op stubs in the `wl_data_device_manager (clipboard/DnD stub)` block
(lines ~1720–1761) with real implementations, and add file-scope state + a NEW
`kDataOfferImpl`. Concretely the owner:

1. Adds clipboard state near the other `g_*` selection state (compositor thread only):
   `g_guest_source`, `g_guest_mimes`, `g_sel_owner`, `g_sel_serial`,
   `g_data_devices` (vector of `wl_data_device` resources),
   `g_android_text/html/png` caches, `g_last_guest_hash`.
2. `ddm_create_data_source`: allocate a `DataSourceState{vector<string> mimes}` as
   the source's user_data + a destroy listener that frees it and nulls
   `g_guest_source` if it matches.
3. `data_source_offer`: append `mime` to that source's mime vector (no longer no-op).
4. `ddm_get_data_device`: `g_data_devices.push_back(res)` + a destroy listener that
   removes it; if `g_sel_owner==Android`, immediately push the current offer to the
   new device.
5. `data_device_set_selection`: implement §4a (record/cancel/notify), call the
   installed `GuestOfferCb`. Add a NEW `data_offer_*` impl (`accept` no-op,
   `receive` → §3b write, `finish`/`destroy` cleanup, `set_actions` no-op) as
   `kDataOfferImpl`.
6. Add the pipe reader (`wl_event_loop_add_fd` on `loop_`, accessible via the
   `Compositor`), a `drain_clipboard_queue()` (mirror `drain_input_queue`) wired
   into the epoll loop where `wakeup_fd_` is handled (line ~2321), and the
   `wl_event_loop* loop()` accessor if not already exposed.
7. New public free functions in `alr_compositor.hpp` + impl:
   `alr_wayland_set_clipboard_sink(GuestOfferCb, GuestTextCb, GuestImageCb)` and
   `alr_wayland_set_android_selection(mimes, text, html, png)` (enqueue+wake), plus
   an optional `alr_wayland_request_guest_clipboard()` if pulls are lazy.

The minimal edit is: turn 3 no-op handlers into real ones, add 1 new offer impl,
add 2 public functions + a queue/drain pair following the existing
`enqueue_inject`/`drain_input_queue` template. No new protocol, no generator/CMake
changes (all `wl_data_*` symbols are already in the vendored generated header).

### 10b. `MainActivity.kt` (MAIN) — ClipboardManager

1. Add imports: `android.content.ClipboardManager`, `android.content.ClipData`,
   `android.content.ClipDescription`, `androidx.annotation.Keep`.
2. Add the `clipboard` lazy service + `suppressClipEcho` flag + `clipListener`
   (§6a) and the `onGuestClipboardOffer/Text/Image` callbacks (§5a, `@Keep`).
   `onGuestClipboardText` does `runOnUiThread { suppressClipEcho=true;
   clipboard.setPrimaryClip(ClipData.newPlainText("ALR", utf8));
   Handler(mainLooper).post { suppressClipEcho=false } }`.
3. In `onCreate`, after `nativeWaylandCompositorStart(...)` (line ~1376):
   `clipboard.addPrimaryClipChangedListener(clipListener)`.
4. In `onDestroy` (or the stop path that calls `nativeWaylandCompositorStop`):
   `clipboard.removePrimaryClipChangedListener(clipListener)`.
5. Declare the new native methods next to the other `nativeWayland*`
   (lines ~2802–2807):
   `private external fun nativeWaylandClipboardSetAndroid(mimes: Array<String>,
   utf8Text: String?, htmlText: String?, pngBytes: ByteArray?)`.

### 10c. `runtime_report.cpp` (MAIN/integration) — JNI shims

Next to the existing `nativeWayland*` JNI block (lines ~6481–6651), add:

- `Java_..._nativeWaylandClipboardSetAndroid(JNIEnv*, jobject, jobjectArray, jstring,
  jstring, jbyteArray)` → unpack to a `std::vector<std::string>` + payloads → call
  `alr::wayland::alr_wayland_set_android_selection(...)`.
- In `nativeWaylandCompositorStart`, after building the config, install the sink:
  `alr::wayland::alr_wayland_set_clipboard_sink(offerCb, textCb, imageCb)` where the
  lambdas `AttachCurrentThread` and call the `onGuestClipboard*` Kotlin methods via
  a cached `g_main_activity` global ref + cached `jmethodID`s (capture the
  `MainActivity` `jobject` here with `env->NewGlobalRef(thiz)`; release in stop).

---

## 11. Threading & lifetime invariants

- Every `wl_data_*` send/create/destroy happens on the compositor thread (queue +
  `wake()` for inbound from Kotlin; `wl_event_loop_add_fd` callbacks are already on
  the compositor thread).
- The compositor thread is a raw pthread → the up-call lambdas MUST
  `JavaVM->AttachCurrentThread` once (cache the `JNIEnv*` per call or attach/detach
  per call) before touching JNI. Simpler: marshal the bytes into the queue and let
  a *Kotlin Handler* own the JVM side — but since the up-call originates native, an
  attach is required; attach-once-on-first-use + detach-on-compositor-stop is fine.
- `wl_data_source` (guest-owned) and `wl_data_offer` (server-owned) resources are
  freed by the client/destroy listeners; never deref after the destroy listener.
  Match the project's "by key/value, never store a parent resource" UAF discipline:
  `g_guest_source`/`g_data_devices` entries are nulled/removed in their destroy
  listeners.
- In-flight pipe transfers hold a *copy* of the payload so a mid-paste clip change
  can't free it.

---

## 12. Test / verification plan (device-pending — cannot run host-only)

1. Guest→Android text: in Chromium/GIMP select+copy; `adb shell` read of
   `service call clipboard` (or a small Android paste target) shows the text.
2. Android→Guest text: copy in an Android app, Ctrl-V in foot/GIMP text field.
3. Loop-guard: copy in guest, confirm exactly ONE `setPrimaryClip` (no oscillation
   in logcat).
4. text/html round-trip (Chromium rich copy → Android `getHtmlText`).
5. image/png (phase 2): GIMP "copy" → Android image paste.
6. No-regression: existing input/present path unaffected (clipboard queue is
   independent of the input queue).

Host-side, the only checkable gate is "compiles + the new `alr_wayland_*` symbols
resolve"; this is a DESIGN deliverable, so no host gate is claimed.
