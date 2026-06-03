# Android Audio Sink for ALR Linux Guests (PulseAudio-over-UNIX → AudioTrack)

Status: DESIGN (host-only, not yet wired). Owner of the wiring: MAIN session
(`MainActivity.kt`) + WS-1 (`runtime_report.cpp` guest-env) + an overlay builder
under `tools/`. Branch: `auto/host-audio`.

Compat-matrix today marks **ALSA = RED (no sink)**: a glibc guest that opens
`/dev/snd/*` finds nothing (no ALSA kernel device is visible to the unprivileged
app, and we will never get one without root/SELinux changes). Guests therefore
get silence or `snd_pcm_open` failures. This doc specifies the most-compatible
**output** path: an in-app **PulseAudio-protocol server** (a tiny native server
the app runs on a UNIX socket) that hands decoded PCM to an Android `AudioTrack`.

---

## 0. TL;DR decision

**Build an in-app PulseAudio-native-protocol server, not an ALSA plugin or PipeWire.**

Reasoning (the honest comparison is in §1):

| option | guest coverage | rootfs staging | host complexity | verdict |
|---|---|---|---|---|
| **PulseAudio server (our choice)** | **Highest** — Firefox/Chromium, GTK/GStreamer, SDL2, Qt, mpv, ffmpeg, VLC, libcanberra all speak libpulse natively or via a pulse backend | client libs (`libpulse0` + `libpulse-mainloop-glib0`) + a 1-line `client.conf` | medium: ~1 native protocol endpoint, PCM-only path | **DO THIS** |
| ALSA plugin (`libasound_module_pcm_*`) | medium — only apps that go through `libasound`; pulse-only apps (Chromium sandbox, many SDL2 builds) bypass it | `libasound2` + a custom `.so` PCM plugin we must compile per-arch + `asound.conf` | high: must build & ship a guest-arch ALSA external-PCM plugin | fallback / complement |
| PipeWire | low here — adds pipewire+wireplumber daemon closure, more sockets, no guest already needs it | huge closure, a session manager daemon | high | **NO** (overkill; pulse server already gives us pipewire-pulse-equivalent coverage) |

PulseAudio wins because **libpulse coverage is the highest single denominator**:
the same backend that pipewire-pulse / pulseaudio-on-the-desktop expose is the one
Linux audio apps assume. We implement only the *server* half of the wire protocol,
PCM playback only, and translate to `AudioTrack`. An ALSA plugin can be added later
as a complement (§7) for the minority of pure-ALSA apps, but it is not the primary path.

---

## 1. Why PulseAudio-protocol server (honest comparison)

### 1a. libpulse coverage is the widest
Almost every modern Linux desktop app reaches audio through one of:
GStreamer `pulsesink`, SDL2 (`SDL_AUDIODRIVER=pulseaudio`), Qt multimedia, mpv/VLC
(`--ao=pulse`), Firefox/Chromium (cubeb/pulse), libcanberra (event sounds), and
ffmpeg/ffplay (`-f pulse`). All of these dlopen/link **`libpulse.so.0`** and connect
to the server at `$PULSE_SERVER` (or the default `unix:$XDG_RUNTIME_DIR/pulse/native`).
If we are that server, all of them play with **zero per-app config** beyond one
`client.conf`.

ALSA-only apps (raw `snd_pcm_open("default")`) are a small minority on the desktop,
and most of *those* are routed to pulse anyway by the distro's `asound.conf`
(`pcm.default → pulse`). So a pulse server transitively covers the ALSA apps too,
**as long as we also ship the ALSA→pulse glue** (`libasound2-plugins`'
`libasound_module_pcm_pulse.so` + an `asound.conf` with `pcm.!default { type pulse }`).
That glue is a *stock package*, not code we write — see §3b.

### 1b. We implement only a thin slice of the protocol
The PulseAudio native protocol is large, but a **playback-only sink server** needs
only a handful of commands. libpulse negotiates capabilities, so unknown/optional
commands can be NAK'd and clients fall back gracefully. The minimum viable command
set (tag/opcode names from PulseAudio `src/pulsecore/native-common.h`):

- Handshake / auth: `PA_COMMAND_AUTH` (we accept any cookie — same UID, local
  socket), `PA_COMMAND_SET_CLIENT_NAME`.
- Introspection the client needs to pick a sink:
  `PA_COMMAND_GET_SERVER_INFO`, `PA_COMMAND_GET_SINK_INFO` /
  `PA_COMMAND_GET_SINK_INFO_LIST` — report exactly **one** sink ("alr-android",
  the device's native rate/channels), `PA_COMMAND_GET_SINK_INPUT_INFO[_LIST]`.
- Playback stream lifecycle: `PA_COMMAND_CREATE_PLAYBACK_STREAM` (carries the
  `pa_sample_spec`: format/rate/channels + buffer attrs `tlength/minreq/prebuf`),
  `PA_COMMAND_DELETE_PLAYBACK_STREAM`, `PA_COMMAND_CORK`, `PA_COMMAND_FLUSH`,
  `PA_COMMAND_TRIGGER`, `PA_COMMAND_DRAIN`.
- Flow control: server→client `PA_COMMAND_REQUEST` ("send me N bytes"),
  client→server `PA_COMMAND_*` for `UNDERFLOW`/`STARTED` notifications, and the
  **memblock** data frames (a stream-index-tagged frame carrying raw PCM that
  rides the same socket; descriptor header `[length][channel][offset_hi]
  [offset_lo][flags]`).
- Timing for A/V sync: `PA_COMMAND_GET_PLAYBACK_LATENCY` — we answer from the
  `AudioTrack` head position (`getTimestamp()` / `getPlaybackHeadPosition()`),
  see §5.

Everything else (modules, source/record, volume DB curves beyond a linear
`SET_SINK_INPUT_VOLUME`) is optional and can be stubbed/NAK'd in v1.

**Protocol-version pragmatism:** announce a *modest* protocol version (e.g. 13)
in the AUTH reply. Older versions have a simpler memblock framing (no memfd/shm
negotiation, no `srbchannel`), which is exactly what we want — we force the
**socket-copy** data path and never advertise `PA_PROTOCOL_FLAG_SHM`/memfd, so
all PCM arrives as plain socket writes we can read with `recv()`. This is the
single most important simplification and it is fully spec-compliant (libpulse
downgrades to the announced version).

### 1c. Why not ALSA-plugin as primary
An ALSA external-PCM plugin (`libasound_module_pcm_alr.so` exporting
`SND_PCM_PLUGIN_DEFINE_FUNC(alr)`) is *less* code conceptually but worse in practice:
- It only catches apps that use `libasound`. Chromium's sandbox and several SDL2
  builds talk pulse directly and would still be silent.
- We must **cross-compile a guest-arch (`aarch64` glibc) `.so`** against
  `alsa-lib` headers and ship it in the rootfs — a build/ABI burden the pulse path
  avoids (the pulse path ships only *stock* `libpulse0` from the Ubuntu mirror).
- Per §1a, ALSA apps are already coverable *through* the pulse server via the
  stock `libasound2-plugins` pulse module.

We therefore make the ALSA plugin an **optional complement (§7)**, not the trunk.

### 1d. Why not PipeWire
PipeWire would mean shipping `pipewire` + `wireplumber` (a session-manager daemon)
into the rootfs and running them, plus a `pipewire-pulse` shim — i.e. we'd *still*
end up speaking the pulse protocol, but with a giant extra daemon closure and more
moving parts inside the guest. No guest in our matrix *requires* the native PipeWire
API. Rejected.

---

## 2. Architecture & data flow

```
 ┌────────────────────── Android app process (one Linux UID) ──────────────────────┐
 │                                                                                  │
 │  GUEST (in-process fork, traced)         HOST (app threads)                      │
 │  ┌───────────────────────────┐           ┌──────────────────────────────────┐   │
 │  │ Firefox / mpv / SDL2 app   │           │  AlrPulseServer (native, C++)    │   │
 │  │   └ libpulse.so.0          │  AF_UNIX  │   - listens on                   │   │
 │  │       connect(PULSE_SERVER)│◀─stream──▶│     $XDG_RUNTIME_DIR/pulse/native │   │
 │  │       writes PCM frames    │  (SOCK_   │   - 1 epoll thread, N clients     │   │
 │  └───────────────────────────┘   STREAM) │   - parses native protocol       │   │
 │         guest_env:                        │   - per-stream ring buffer (PCM) │   │
 │           PULSE_SERVER=unix:.../native    │   - resample to device rate (§6) │   │
 │           (+ pulse/client.conf in rootfs) │   - JNI upcall: feed PCM ─────────┐  │
 │                                           └──────────────────────────────────┘  │ │
 │                                                          │ JNI (one of two)     │ │
 │                                            ┌─────────────▼──────────────────┐   │ │
 │                                            │ AndroidAudioSink (Kotlin)      │   │ │
 │                                            │   AudioTrack(PCM_16/FLOAT,     │◀──┘ │
 │                                            │     deviceRate, stereo,        │     │
 │                                            │     MODE_STREAM, LOW_LATENCY)  │     │
 │                                            │   .write(pcm, blocking)        │     │
 │                                            └────────────────────────────────┘     │
 └──────────────────────────────────────────────────────────────────────────────────┘
```

Key properties:
- **Same UID, local socket** → auth is trivial (accept any cookie). No network.
- **Audio is NOT Wayland.** The compositor (`alr_compositor.cpp`) is *uninvolved*.
  This is a standalone server + a JNI bridge. Minimal compositor footprint = none.
- The socket lives in the **same `XDG_RUNTIME_DIR`** the compositor already created
  and the guest already inherits (§4), so no new directory plumbing is required.

### 2a. Two JNI shapes (pick one) — both feed the SAME `AudioTrack`
- **Option A (recommended): host C++ owns AAudio directly.** API 26+ ships
  **AAudio** (`<aaudio/AAudio.h>` in the NDK). The native `AlrPulseServer` opens an
  `AAudioStream` (`AAUDIO_PERFORMANCE_MODE_LOW_LATENCY`, `SHARING_MODE_SHARED`,
  `FORMAT_PCM_FLOAT` or `_I16`) and writes PCM with `AAudioStream_write()` straight
  from the epoll thread / a dedicated writer thread. **No JNI for the audio data
  path at all** — Kotlin only grants the (none-needed) permission and starts/stops
  the server. This is the lowest-latency, simplest wiring and the recommendation.
- **Option B: Kotlin `AudioTrack`, fed over JNI.** If the owner prefers Java-side
  control (routing, device selection, volume UI), the native server calls up via a
  registered callback: `void feedPcm(short[]/float[] frame, int frames)` →
  `AudioTrack.write(...)`. Slightly more copies + a JNI boundary per buffer.

Both end at an Android PCM sink; choose A unless the product wants Java-side audio
routing. The rest of this doc is written so either works (the server's PCM ring is
identical; only the final ~10 lines differ).

---

## 3. Rootfs staging (the guest's client half)

The guest needs (a) the **pulse client library** and (b) a **client.conf** that
points libpulse at our socket. Optionally (c) the ALSA→pulse bridge.

### 3a. libpulse client (REQUIRED)
Stage these stock Ubuntu **noble** arm64 packages (base = Ubuntu 24.04, glibc 2.39
— see MEMORY `base-is-ubuntu-noble`; build from `ports.ubuntu.com` noble
`main`+`universe`, UA-gated `.zst` debs, via the existing `tools/deb_closure.py`):

- `libpulse0` — `/usr/lib/aarch64-linux-gnu/libpulse.so.0` (+ `pulsecommon-*.so`)
- `libpulse-mainloop-glib0` — for GLib/GTK apps that use the glib mainloop binding
- (transitive) `libsndfile1`, `libasyncns0`, `libapparmor1`, `libsystemd0` if the
  closure pulls them — let `deb_closure` resolve. These are **client** libs only;
  we do **not** stage the `pulseaudio` server package (we are the server).

These go to `/usr/lib/aarch64-linux-gnu/...`, the rootfs's normal lib dir. The
guest's existing `LD_LIBRARY_PATH` (runtime_report.cpp) already covers it.

### 3b. client.conf (REQUIRED — points libpulse at us)
libpulse reads, in order, `$PULSE_CLIENTCONFIG`, `$XDG_CONFIG_HOME/pulse/client.conf`,
then `/etc/pulse/client.conf`. We ship a system one at **`/etc/pulse/client.conf`**:

```ini
# /etc/pulse/client.conf  (staged by build_pulse_overlay.py)
default-server = unix:/run/user/0/pulse/native   ; matches $XDG_RUNTIME_DIR/pulse/native
autospawn = no                                   ; never try to fork a server
daemon-binary = /bin/true                        ; belt-and-suspenders: no autospawn
enable-shm = no                                  ; force socket-copy PCM (no memfd/shm)
enable-memfd = no
```

`enable-shm/enable-memfd = no` is the client-side guarantee of §1b's socket-copy
path. `autospawn = no` stops libpulse from trying to start a real pulse daemon when
it can't reach us during a race. The `default-server` value is belt-and-suspenders;
the authoritative selector is the **`PULSE_SERVER` env** (§4), which overrides
`client.conf` and is set per-launch by the host.

> **Path note:** the guest's `XDG_RUNTIME_DIR` value is whatever
> `runtime_report.cpp` exports (today a host cache subdir, presented to the guest).
> `/run/user/0` above is illustrative — the overlay builder should template the
> `default-server` line, OR (simpler) rely solely on the `PULSE_SERVER` env and ship
> `client.conf` with only `autospawn=no`+`enable-shm=no`. **Recommendation: drive the
> address purely via `PULSE_SERVER` env; keep `client.conf` address-free.**

### 3c. (OPTIONAL, §7) ALSA→pulse bridge for pure-ALSA apps
- `libasound2` + `libasound2-plugins` (stock) → `libasound_module_pcm_pulse.so`
- `/etc/asound.conf`:
  ```
  pcm.!default { type pulse }
  ctl.!default { type pulse }
  ```
This makes `snd_pcm_open("default")` route to our pulse server transitively. Ship it
in the same overlay (it's small and stock) so ALSA apps light up for free.

### 3d. Overlay builder (NEW, host-only) — `tools/build_pulse_overlay.py`
Mirror `tools/build_locale_overlay.py` exactly (same `_add_dir_tree`,
`stage_tar_spec.validate_stage_tar`, `deb_closure` plumbing). It:
1. Resolves the `libpulse0` (+ optional ALSA-plugins) closure from noble via
   `deb_closure`, extracts with `build_stage_tar.extract_deb`.
2. Emits the lib `.so`s under `./usr/lib/aarch64-linux-gnu/...`.
3. Emits `./etc/pulse/client.conf` (address-free, §3b) and optional
   `./etc/asound.conf`.
4. Validates against the base with `validate_stage_tar` (no base-lib downgrade —
   passes the WS-4 `overlay_guard` since all paths are new), prints a manifest.

Output: a `./`-rooted stage tar consumed by the **already-wired, guarded**
`extractOverlayTar` slot in `MainActivity` (same mechanism as
`xkb-gegl-stage.tar`/`gtk3demo-stage.tar`). I do **not** edit MainActivity; the
owner adds one staging block (§9). I provide the builder as a NEW file only if asked;
this doc fully specifies it.

---

## 4. Guest env (WS-1 / `runtime_report.cpp`)

libpulse picks the server from **`PULSE_SERVER`** before any config file. The host
already builds `guest_env` (runtime_report.cpp ~L1518-1601) and already exports
`XDG_RUNTIME_DIR=<dir>` (the same dir `alr_compositor.cpp::make_socket()` uses).
Add **one** line, pointing at the pulse socket inside that dir:

```cpp
// runtime_report.cpp, in the guest_env block (next to XDG_RUNTIME_DIR / WAYLAND_DISPLAY)
guest_env.push_back("PULSE_SERVER=unix:" + xdg_runtime_dir + "/pulse/native");
// optional, makes the address explicit for tools that read it:
guest_env.push_back("PULSE_CLIENTCONFIG=/etc/pulse/client.conf");
```

`xdg_runtime_dir` is already in scope there (it's the var used for the existing
`XDG_RUNTIME_DIR=` push_back). No other guest env is required. (For the ALSA bridge,
no env is needed — `asound.conf` is file-driven.)

**Socket location contract:** the server (§5) MUST `bind()` its listening UNIX
socket at exactly `${XDG_RUNTIME_DIR}/pulse/native` (creating the `pulse/`
subdir, mode 0700). This is the conventional libpulse path, so even a guest with no
`PULSE_SERVER` env would still find it via `client.conf`'s default.

---

## 5. The host server — `AlrPulseServer` (NEW native, host-only)

A self-contained C++ component (proposed `app/src/main/cpp/alr_audio/alr_pulse_server.{h,cpp}`,
plus a small JNI shim). It does NOT touch the compositor or any owned file.

### 5a. Listen / accept
- `socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK)` →
  `bind()` to `${XDG_RUNTIME_DIR}/pulse/native` (mkdir `pulse/` 0700; unlink stale
  socket first) → `listen()`.
- One `epoll` loop on its own `std::thread` (mirrors the compositor's
  `g_thread = std::thread([]{ comp->run(); })` pattern at alr_compositor.cpp:2419,
  but **independent** — separate thread, separate fd set). A `wakeup_fd_`
  (eventfd) lets `stop()` break the loop cleanly, exactly like the compositor.

### 5b. Per-client state machine
Each accepted fd → a `Client { read_buf, sample_spec, stream_index, ring }`.
Drive the native protocol per §1b:
1. `AUTH` → reply OK + announced protocol version (≤14, no SHM flag).
2. `SET_CLIENT_NAME` → reply with a client index.
3. `GET_SERVER_INFO` / `GET_SINK_INFO[_LIST]` → report one sink "alr-android" at
   the **device's native rate** (query once via AAudio/`AudioManager`
   `PROPERTY_OUTPUT_SAMPLE_RATE`, typically 48000) / stereo / `PA_SAMPLE_S16LE` or
   `FLOAT32LE`. Reporting our *native* rate lets well-behaved clients pre-resample
   and skip §6.
4. `CREATE_PLAYBACK_STREAM` → read the client's `pa_sample_spec` + buffer attrs,
   allocate a PCM ring sized from `tlength`, reply with negotiated
   `tlength/minreq/prebuf/maxlength` + a stream index, and immediately send a
   `PA_COMMAND_REQUEST` granting the client `minreq` bytes of credit.
5. **Data frames**: the client writes memblock frames (descriptor header + raw PCM)
   on the same socket. Append PCM to that stream's ring; when consumed, send another
   `REQUEST` to refill credit (this *is* the back-pressure — see §5d).
6. `CORK`/`FLUSH`/`DRAIN`/`TRIGGER`/`DELETE_PLAYBACK_STREAM` → ring start/clear/
   drain/teardown.
7. `GET_PLAYBACK_LATENCY` → answer from the sink's playout position (§5e).

### 5c. Mixing
v1: support N streams by **summing** (`int32` accumulate → clip to `int16`/clamp
float) into a single device buffer, or — simplest — allow only one active playback
stream and NAK a second `CREATE_PLAYBACK_STREAM` (most single-app guests open one).
Recommend: simple N-stream summing mixer, ~30 lines, no per-stream volume curve in
v1 (linear `SET_SINK_INPUT_VOLUME` optional).

### 5d. The sink: AAudio (Option A)
- Open once on first stream: `AAudioStreamBuilder` →
  `setPerformanceMode(LOW_LATENCY)`, `setSharingMode(SHARED)`,
  `setFormat(I16 or FLOAT)`, `setSampleRate(deviceRate)`, `setChannelCount(2)`,
  `setDataCallback(...)` **or** blocking `AAudioStream_write()`.
- **Recommended: callback mode.** AAudio pulls `numFrames` from the mixer ring in
  its realtime callback. If the ring underruns, write silence (and notify clients
  with `PA_COMMAND_*` underflow). Callback mode keeps latency tight and decouples
  socket jitter from the device clock.
- The **credit/REQUEST** mechanism (§5b.5) is the flow control: we only grant the
  client more bytes as the ring drains, so a fast producer (mpv decoding ahead)
  naturally blocks in `pa_stream_write` rather than ballooning memory. This is the
  whole back-pressure story; no extra throttling needed.

### 5e. Latency reporting (A/V sync)
`GET_PLAYBACK_LATENCY` must return a credible playout delay or video will drift.
Compute it from AAudio's presentation timestamp:
`AAudioStream_getTimestamp(&framePosition, &nanoseconds)` → frames-not-yet-played =
(frames_written − framePosition); latency_usec = frames_pending / rate * 1e6 +
ring_backlog/rate. (Option B: `AudioTrack.getTimestamp()`.) Report this in the
`pa_timing_info` so `pulsesink`/cubeb sync correctly.

### 5f. JNI surface (Kotlin ↔ C++) — mirror the existing `nativeWayland*` externals
Add to MainActivity (owner edit, §9) externals shaped exactly like
`nativeWaylandCompositorStart/Status/Stop` (MainActivity.kt:2787-2800):

```kotlin
// Option A (AAudio in C++): Kotlin only starts/stops; no data crosses JNI.
private external fun nativeAudioSinkStart(xdgRuntimeDir: String, deviceRate: Int): String
private external fun nativeAudioSinkStatus(): String
private external fun nativeAudioSinkStop(): String
```

```cpp
// JNI impl (alr_audio/alr_pulse_jni.cpp), registered in the same .so (alr_loader):
extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAudioSinkStart(
    JNIEnv* env, jobject, jstring xdgRuntimeDir, jint deviceRate);  // bind+listen+spawn thread
// ...Status returns "ALR AUDIO SINK: <state> streams=<n> rate=<r>"; Stop joins the thread.
```

For **Option B** add `feedPcm`-style callbacks instead (server holds a global
`JavaVM*` + a `MainActivity` weak ref + cached `AudioTrack.write` methodID, calls up
per buffer). Option A avoids all of that.

---

## 6. Resampling

- Negotiation-first: we advertise the **device native rate** (§5b.3). Apps that
  honor the sink's preferred spec (GStreamer, SDL2, mpv) resample on their side and
  hand us native-rate PCM → **no host resampling** for the common case.
- Apps that insist on their own rate (e.g. 44100 into a 48000 device) need host
  resampling. v1: a small **linear interpolation** resampler in the server (cheap,
  acceptable for speech/UI sounds). v2: drop in a quality resampler — either stage
  `libsamplerate`/`libspeexdsp` and `dlopen` it, or use Android's
  `AudioTrack`/AAudio built-in SRC by simply opening the device at the *client's*
  rate when only one stream exists (AAudio will resample to the HAL). The cleanest:
  **open AAudio at the device native rate and resample in-server**, since with the
  mixer (§5c) multiple clients may have different rates.
- Format conversion (S16↔FLOAT, channel up/down-mix mono→stereo) is trivial and
  done in the same per-stream convert step before mixing.

---

## 7. Optional complement: ALSA external-PCM plugin

For the minority of apps that ignore pulse and call `snd_pcm_open` directly **and**
where the stock `libasound_module_pcm_pulse` route (§3c) is undesirable, a native
ALSA plugin can talk to the **same** AlrPulseServer-or-a-sibling socket:

- A guest-arch `.so` exporting `SND_PCM_IOPLUG` (`snd_pcm_ioplug_create`) +
  `SND_PCM_PLUGIN_DEFINE_FUNC(alr)`; `pointer/transfer/start/stop/prepare` callbacks
  push PCM over a UNIX socket to a host PCM endpoint.
- Shipped via `/etc/asound.conf`: `pcm.!default { type alr }` and the `.so` under
  `/usr/lib/aarch64-linux-gnu/alsa-lib/`.
- Cost: we must cross-compile this `.so` for `aarch64` glibc against `alsa-lib`
  headers (an extra build target). **Defer** unless a target app needs it; §3c's
  stock pulse-plugin already covers ALSA→pulse with zero custom code.

Recommendation: **start with §3c (stock plugin) only**; write the custom plugin only
if a real app is proven to fail through it.

---

## 8. Input (mic) — STRETCH GOAL (note only)

Symmetric but gated on a runtime permission:
- Implement `PA_COMMAND_CREATE_RECORD_STREAM` + report one source "alr-mic".
- Host side: `AAudioStreamBuilder` with `DIRECTION_INPUT` (or `AudioRecord`), which
  **requires `android.permission.RECORD_AUDIO`** (dangerous → runtime
  `requestPermissions`) declared in `AndroidManifest.xml`. Push captured PCM to the
  record stream's ring; emit memblock frames to the client.
- Most guests don't need capture; **defer**. Output is the value here. Note this so
  the owner can scope the manifest/permission work separately.

---

## 9. ownerWiring (the precise minimal edits the owners make)

Three small edits, each in an owner-owned file. **None touch the compositor's
protocol code** (audio is not Wayland).

**(A) `tools/` — add the overlay builder (host, no app change).** Drop in
`tools/build_pulse_overlay.py` (a clone of `tools/build_locale_overlay.py`, §3d).
Run it to emit `pulse-stage.tar` (libpulse0 + `/etc/pulse/client.conf`, address-free,
`autospawn=no enable-shm=no`; optional `libasound2-plugins` + `/etc/asound.conf`).

**(B) `MainActivity.kt` — stage the overlay + start the sink.**
1. Add a staging block next to the existing `xkb-gegl-stage.tar` block
   (MainActivity.kt ~L210-220), guarded `extractOverlayTar`:
   ```kotlin
   if (java.io.File("/data/local/tmp/pulse-stage.tar").isFile) Thread {
       val t = java.io.File("/data/local/tmp/pulse-stage.tar")
       // marker keyed on size, exactly like the other overlays
       val ovr = RootfsInstaller(this@MainActivity)
           .extractOverlayTar(t, rootfsStatus.rootfsDir)
       android.util.Log.i("alr_loader", "pulse-stage: overlay done (extracted=${ovr.extracted})")
   }.start()
   ```
2. After the Wayland compositor is started (same `XDG_RUNTIME_DIR`), start the sink
   once (Option A):
   ```kotlin
   val rate = (getSystemService(AUDIO_SERVICE) as android.media.AudioManager)
       .getProperty(android.media.AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)?.toIntOrNull() ?: 48000
   nativeAudioSinkStart(xdgRuntimeDir, rate)   // xdgRuntimeDir = the dir passed to the compositor
   ```
   and `nativeAudioSinkStop()` in `onDestroy`. Declare the three externals (§5f).
   **No new permission for output** (AudioTrack/AAudio playback needs none on
   API 26+). Mic (§8) would add `RECORD_AUDIO`.

**(C) `runtime_report.cpp` (WS-1) — one guest-env line.**
   In the `guest_env` block (~L1534, next to `XDG_RUNTIME_DIR`):
   ```cpp
   guest_env.push_back("PULSE_SERVER=unix:" + xdg_runtime_dir + "/pulse/native");
   ```

**(D) Build wiring (owner): add `alr_audio/alr_pulse_server.cpp` +
`alr_pulse_jni.cpp` to the `alr_loader` target's `CMakeLists.txt`** so the new JNI
symbols land in the same `.so` MainActivity already loads (`System.loadLibrary("alr_loader")`,
MainActivity.kt:28). Link `aaudio` (`find_library(aaudio-lib aaudio)`); AAudio is
in the NDK at API 26+, matching `minSdk = 26`.

That's the whole wire-up: 1 builder, ~12 lines of Kotlin, 1 C++ env line, 2 new
native source files + a CMake entry. The protocol server is self-contained.

---

## 10. Host-verifiability & risks (honest)

- **Cannot device-verify here** (host-only). The protocol server's framing can be
  unit-tested host-side against the real `libpulse` (link a host `libpulse-dev`,
  run a `pa_simple_write` of a sine into the server, assert PCM bytes arrive) — a
  good follow-up host gate, but the real sink (AAudio→speaker) needs the device.
- **Protocol-version drift:** if libpulse on noble negotiates a version whose
  memblock framing differs from our assumption, audio garbles. Mitigation: announce
  a *low* version (§1b) and parse the descriptor header defensively; add the
  host-side libpulse round-trip test above before trusting it on-device.
- **Latency tuning** (§5e) is the thing most likely to need device iteration
  (buffer sizes, AAudio performance mode fallbacks on this Mali/mt6878 platform).
- **Autospawn footgun:** if `client.conf`'s `autospawn` isn't `no`, libpulse may
  try to fork a real pulse daemon (which we don't ship) and stall — hence §3b nails
  it off explicitly.

---

## 11. Summary

Run an **in-app PulseAudio-native-protocol server** on a UNIX socket inside the
guest's existing `XDG_RUNTIME_DIR`, translate playback streams to an Android
**AAudio/AudioTrack** PCM sink, and stage the **stock `libpulse0` client +
`client.conf`** into the rootfs (plus optional stock ALSA→pulse plugin for the
pure-ALSA minority). This gives the widest guest coverage (everything that links
libpulse, transitively including ALSA-default apps) for the least new code, with
audio kept entirely **off** the Wayland/compositor path. Owner wiring is one overlay
builder, ~12 Kotlin lines, one guest-env line in `runtime_report.cpp`, and two new
self-contained native files added to the `alr_loader` CMake target. This flips the
compat-matrix **ALSA = RED → audio = supported (pulse sink)**.
```
