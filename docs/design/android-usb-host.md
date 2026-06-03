# Android USB Host → Linux guest USB access

Status: DESIGN (host-only, not yet wired). Owner-wiring section is precise and
ready to implement. Device verification is OUT OF SCOPE for this doc.

Goal: let an unmodified arm64 glibc Linux app running in-process via ALR talk to
a physical USB device plugged into the Android phone (USB-OTG / USB-C host mode).
The canonical guest entry point is **libusb** (`libusb_open`, control/bulk/
interrupt transfers); virtually every Linux USB userspace tool (lsusb, libftdi,
PTP/gphoto2, Arduino avrdude, libusb-based printer/scanner backends, WebUSB-style
tools) goes through it. So the design target is: *make guest libusb work without
recompiling the guest app.*

On non-root Android we cannot give the guest a real `/dev/bus/usb/<bus>/<dev>`
character device — that node does not exist for an unprivileged app and we cannot
`mknod` it, and the kernel `usbfs` ioctls would be `EACCES` even if it did. The
only public, unprivileged path to a USB device is `android.hardware.usb.*`:
`UsbManager` opens the device and hands us a **raw fd**
(`UsbDeviceConnection.getFileDescriptor()`) plus high-level
`controlTransfer/bulkTransfer/requestWait/claimInterface` methods. The job of
this design is to bridge that Android fd/API surface to the guest's libusb.

---

## 0. Background grounded in this repo

Two existing ALR mechanisms are the load-bearing precedents; the USB bridge is
built the same way as one of them, not invented from scratch:

1. **The LD_PRELOAD interposer** `app/src/main/cpp/alr_interpose/libalr_interpose.c`
   already wraps glibc path wrappers (`open`/`openat`/`access`/`readlink`/…),
   resolves the real libc symbol lazily via `dlsym(RTLD_NEXT, …)` (the `ALR_REAL`
   macro), and rewrites guest absolute paths under the rootfs prefix
   (`rw()` / `g_rootfs`). In PCGATE mode it routes the underlying syscall through
   a single trampoline `alr_tramp_syscall()`. **It does NOT currently wrap
   `ioctl`** — confirmed: `grep ioctl libalr_interpose.c` is empty. usbfs
   emulation (Option A) would require adding an `ioctl` wrapper from scratch.

2. **The guest↔Android UNIX-socket bridge** is an established, working pattern:
   `runGuestGpuIpcBridge` in
   `app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt` (and the in-app
   Wayland compositor over an `AF_UNIX` socket created at
   `<cacheDir>/alr-xdg/wayland-0`, see `nativeWaylandCompositorStart` in
   `runtime_report.cpp`) already does exactly the topology this design needs: a
   host-side thread/service that owns an Android resource (GPU executor / the
   compositor) and serves a guest client over a UNIX socket whose path is exported
   to the guest via env. **The libusb shim (Option B) is the same pattern as
   the GPU bridge.** That is the central reason this doc recommends Option B.

JNI convention in this repo: static `Java_dev_chanwoo_androlinux_MainActivity_native*`
functions in `runtime_report.cpp` (NOT `RegisterNatives`); Kotlin declares
`private external fun native…` and `System.loadLibrary("alr_loader")`. New native
methods follow that exact convention.

---

## 1. The two options, compared honestly

### Option A — usbfs emulation (interpose `open`/`ioctl` on `/dev/bus/usb/...`)

Make the guest believe a real Linux `usbfs` tree exists. The guest libusb does
`open("/dev/bus/usb/001/005", O_RDWR)` then drives the device with `USBDEVFS_*`
ioctls. We would:

* In the interposer, special-case `open`/`openat` of `/dev/bus/usb/**`: instead of
  the real syscall, hand back a *virtual fd* (e.g. a pipe/eventfd/`memfd` we own,
  or the Android `UsbDeviceConnection` fd itself) tracked in a small fd→device
  table.
* Add an **`ioctl` wrapper** (new — the interposer has none today) that recognizes
  that virtual fd and translates each `USBDEVFS_*` request to the Android
  `UsbDeviceConnection` method, marshalled over the bridge to MainActivity:
  - `USBDEVFS_CONTROL` → `UsbDeviceConnection.controlTransfer(reqType, req, value, index, buf, len, timeout)`
  - `USBDEVFS_BULK` → `bulkTransfer(UsbEndpoint, buf, len, timeout)`
  - `USBDEVFS_SUBMITURB` / `REAPURB` (async, what modern libusb actually uses by
    default) → `UsbRequest.queue()` + `UsbDeviceConnection.requestWait()`
  - `USBDEVFS_CLAIMINTERFACE` / `RELEASEINTERFACE` → `claimInterface()/releaseInterface()`
  - `USBDEVFS_GET_CAPABILITIES`, `USBDEVFS_RESET`, `USBDEVFS_CLEAR_HALT`,
    `USBDEVFS_SETCONFIGURATION`, `USBDEVFS_DISCONNECT/CONNECT`, `USBDEVFS_IOCTL`
    (the driver-detach passthrough) → mapped or `ENOTTY`/best-effort.
* Synthesize the sysfs/usbfs descriptor reads libusb does on enumeration:
  `read()` on the device node returns the device + config descriptors;
  `/sys/bus/usb/devices/**` attributes (`idVendor`, `idProduct`, `busnum`,
  `devnum`, `speed`, …) — libusb's Linux backend reads these too. We'd have to
  emulate a chunk of sysfs as well, or compile-time depend on libusb being built
  with the older descriptor-from-node path.

**Cost / risk:** HIGH. `USBDEVFS_*` is a large, quirky ABI; the async URB
reaping model (`SUBMITURB`/`REAPURB`/`REAPURBNDELAY` + the signal/`poll` wakeup
on the fd) is genuinely hard to emulate over a request/response bridge because
libusb expects the *fd itself* to become readable when a URB completes (it
`poll()`s the usbfs fd in its event thread). We'd have to make our virtual fd an
`eventfd`/pipe that we signal from the Android transfer-completion callback, and
keep a URB id table. Plus sysfs emulation. This is the maximum-completeness,
maximum-effort route. Its one advantage: **zero guest changes, zero env, works
with the exact distro libusb.so already in the rootfs** — if (and only if) we
emulate enough of usbfs+sysfs that that libusb is satisfied.

### Option B — libusb backend shim (LD_PRELOAD a thin libusb that forwards to a UNIX-socket bridge) — RECOMMENDED

Don't emulate the kernel ABI at all. Replace libusb's *backend* with one that
speaks our own tiny protocol to MainActivity's `UsbManager`. Two sub-variants:

* **B1 (preferred): ship a small drop-in `libusb-1.0.so.0`** built against the
  public libusb-1.0 API (the ~40 exported `libusb_*` functions the guest app
  links). Internally it does NOT open `/dev/bus/usb`; it connects to
  `AF_UNIX` socket `$ALR_USB_SOCK` and forwards open / get-descriptor /
  control / bulk / interrupt / claim / hotplug as length-prefixed messages. The
  guest app is `LD_PRELOAD`ed / rootfs-overlaid so this `.so` shadows the distro
  libusb. Because we control the SONAME (`libusb-1.0.so.0`) and the public ABI is
  stable and small, an unmodified app binds to it transparently.
* **B2: a `libusb_set_option(LIBUSB_OPTION_..)` / `LIBUSB_DEBUG`-free custom
  backend** is not exposed by upstream libusb (the backend table `usbi_backend`
  is private), so B2 reduces to B1 in practice — we build our own libusb-shaped
  `.so`. (We are NOT patching upstream libusb's internal backend; we provide a
  parallel implementation of the public surface.)

**Cost / risk:** MEDIUM-LOW. We implement only the ~30–40 public `libusb_*`
functions the apps use (open, close, get_device_list, get_device_descriptor,
get_config_descriptor, claim/release_interface, control/bulk/interrupt_transfer,
the async `libusb_submit_transfer`/`libusb_handle_events` path, and
`libusb_hotplug_register_callback`). That public API is documented, stable, far
smaller and saner than `USBDEVFS_*`, and maps almost 1:1 onto the Android
`UsbDeviceConnection` methods. The async event model is also *easier*: libusb's
public API lets the shim own its own `libusb_handle_events` loop (we wake it from
the bridge), so we are not forced to make a kernel fd magically `poll`-readable.

The cost: it only helps apps that go through **libusb**. A program that opens
`/dev/bus/usb` directly (raw, no libusb) gets nothing. In practice that set is
near-empty (almost everything uses libusb or libusb-via-hidapi); the few raw
users are niche. For ALR's "run real Linux apps" goal, libusb coverage is the
right 95%.

### Verdict

**Recommend Option B1 (libusb backend shim over a UNIX-socket bridge).** It is
less work, reuses the *already-proven* guest↔Android socket-bridge pattern
(`runGuestGpuIpcBridge` / the Wayland socket), avoids reimplementing the gnarly
usbfs URB ABI and sysfs, and maps cleanly onto the Android `UsbManager` API it
must ultimately call. Option A is strictly more compatible *in theory* (covers
non-libusb raw users) but costs far more and its hardest part (async URB reaping
made `poll`-visible on a fake fd) is exactly the part Option B avoids. Keep
Option A documented as a future "raw usbfs" fallback for the rare non-libusb app;
do not build it first.

The rest of this doc specs Option B1 concretely.

---

## 2. Android side — the UsbManager bridge (MainActivity)

### 2.1 Permissions + manifest (REQUIRED)

`app/src/main/AndroidManifest.xml` currently declares only INTERNET +
ACCESS_NETWORK_STATE. Add:

```xml
<!-- USB Host: enumerate + open physical USB devices over OTG/USB-C. -->
<uses-feature android:name="android.hardware.usb.host" android:required="false" />
```

No runtime "dangerous" permission is needed for USB host: access is granted
*per-device* by the system USB-permission dialog (see 2.3). `required="false"`
keeps the APK installable on phones without OTG. (No `<uses-permission>` entry
exists for generic host-mode access; do not invent one.)

OPTIONAL auto-launch / auto-grant on plug-in — attach an intent filter +
device-filter resource to the activity that owns the guest
(`.ui.RunningSurfaceActivity`):

```xml
<activity android:name=".ui.RunningSurfaceActivity" ...>
    <intent-filter>
        <action android:name="android.hardware.usb.action.USB_DEVICE_ATTACHED" />
    </intent-filter>
    <meta-data android:name="android.hardware.usb.action.USB_DEVICE_ATTACHED"
               android:resource="@xml/usb_device_filter" />
</activity>
```

with `app/src/main/res/xml/usb_device_filter.xml` listing the
vendor/product/class to auto-grant (granting `USB_DEVICE_ATTACHED` via the
manifest *auto-approves* permission for matching devices, skipping the dialog —
nice UX for known peripherals). This is optional polish; the dialog path works
without it.

### 2.2 The bridge service (new Kotlin)

A new self-contained class — proposed `dev.chanwoo.androlinux.usb.UsbHostBridge`
(NEW file, not an edit to MainActivity) — that:

1. `getSystemService(Context.USB_SERVICE) as UsbManager`.
2. Owns an `AF_UNIX` `LocalServerSocket` (abstract-namespace or a filesystem
   socket under `cacheDir`, mirroring the Wayland socket at
   `<cacheDir>/alr-xdg/wayland-0`; propose `<cacheDir>/alr-usb/usbd.sock`). Its
   path is exported to the guest as `$ALR_USB_SOCK`.
3. Accepts the guest shim connection and serves the protocol in §4, translating
   each request into `UsbManager`/`UsbDeviceConnection` calls on a dedicated USB
   I/O thread (never the UI thread; transfers block).
4. Tracks open `UsbDeviceConnection`s and `UsbInterface`/`UsbEndpoint` handles in
   a small table keyed by a connection id the protocol assigns.

Core Android API surface used:

| Need | Android API |
|---|---|
| enumerate | `UsbManager.getDeviceList(): HashMap<String, UsbDevice>` |
| identity | `UsbDevice.vendorId/productId/deviceClass/manufacturerName/productName/serialNumber`, `UsbDevice.getConfigurationCount()/getInterfaceCount()`, `UsbInterface`, `UsbEndpoint.address/attributes/maxPacketSize/interval` |
| permission | `UsbManager.hasPermission(dev)`, `UsbManager.requestPermission(dev, PendingIntent)` |
| open | `UsbManager.openDevice(dev): UsbDeviceConnection` |
| raw fd (Option A only) | `UsbDeviceConnection.getFileDescriptor(): Int` and `.getRawDescriptors(): ByteArray` (the cached device+config descriptor blob — used to answer libusb's descriptor reads cheaply) |
| claim | `UsbDeviceConnection.claimInterface(iface, force)` / `releaseInterface` |
| control xfer | `UsbDeviceConnection.controlTransfer(requestType, request, value, index, buffer, length, timeout)` |
| bulk/int xfer (sync) | `UsbDeviceConnection.bulkTransfer(endpoint, buffer, length, timeout)` |
| async xfer | `UsbRequest.initialize(conn, endpoint)` + `UsbRequest.queue(ByteBuffer)` + `UsbDeviceConnection.requestWait()` (the only async primitive; used for interrupt-IN and to back libusb's async transfers) |
| reset / config | `UsbDeviceConnection.controlTransfer` with the standard SET_CONFIGURATION; there is **no** public `UsbDeviceConnection.reset()` (a real limitation — see §6) |
| close | `UsbDeviceConnection.close()` |

### 2.3 Permission UX

Android gates USB at *open* time. Flow:

1. Guest shim sends `ENUMERATE`/`OPEN(vid,pid,bus,addr)`.
2. Bridge finds the `UsbDevice`; if `!usbManager.hasPermission(dev)` it calls
   `requestPermission(dev, pendingIntent)`. Android shows the system dialog
   ("Allow the app to access the USB device?"). The shim's `OPEN` blocks
   (protocol-level) until the bridge's `BroadcastReceiver` for the custom action
   (`dev.chanwoo.androlinux.USB_PERMISSION`) fires with
   `EXTRA_PERMISSION_GRANTED`.
3. On grant → `openDevice` → reply `OPEN_OK(connId, rawDescriptors)`. On deny →
   `OPEN_ERR(EACCES)`, which the shim returns to the guest app as
   `LIBUSB_ERROR_ACCESS`.

The `PendingIntent` must use `FLAG_MUTABLE` (Android 12+/API 31+ requires the
USB extra to be filled in) — note this explicitly; an `FLAG_IMMUTABLE` pending
intent silently fails the grant on recent Android. Target device is API 31+
(this phone is Android 16), so `FLAG_MUTABLE` is mandatory.

### 2.4 Hotplug (attach/detach → guest)

Register a `BroadcastReceiver` for `UsbManager.ACTION_USB_DEVICE_ATTACHED` and
`ACTION_USB_DEVICE_DETACHED` (runtime `registerReceiver`, or the manifest filter
in 2.1 for attach). On attach/detach the bridge pushes a `HOTPLUG(added|removed,
vid, pid, busAddr)` async message down every connected shim socket. The shim
turns that into a `libusb_hotplug_callback` dispatch for the guest. Apps that
poll instead of using hotplug callbacks (`libusb_get_device_list` each frame)
also work — the bridge just answers ENUMERATE with the current set.

For a `udev`/`inotify`-based guest (some apps watch `/dev/bus/usb` or run
`udevadm monitor` instead of libusb hotplug), Option B does NOT cover them
(there's no real device node to inotify). That's an accepted gap of B; an
udev-shim is future work and would more naturally pair with Option A.

---

## 3. Guest side — the libusb shim (.so)

A NEW self-contained native target — propose
`app/src/main/cpp/alr_usb/guest_shim/alr_libusb_shim.c` built into
`libusb-1.0.so.0` (4 ABIs via the existing NDK CMake, like the GLES guest shim
under `alr_gpu/guest_shim/`). It exports the public `libusb_*` symbols and:

* `libusb_init` → connect `AF_UNIX` to `$ALR_USB_SOCK`; spawn an internal reader
  thread for async/hotplug messages.
* `libusb_get_device_list` / `..._descriptor` / `..._config_descriptor` →
  ENUMERATE + parse the `rawDescriptors` blob the bridge returned (the standard
  USB descriptor layout — the shim parses it locally, no per-field round trips).
* `libusb_open` → OPEN (triggers the Android permission dialog, blocks for the
  grant), store the returned `connId` in the `libusb_device_handle`.
* `libusb_claim_interface` / `release_interface` → CLAIM / RELEASE.
* `libusb_control_transfer` → CONTROL.
* `libusb_bulk_transfer` / `interrupt_transfer` → BULK / INTERRUPT.
* `libusb_submit_transfer` / `libusb_handle_events[_timeout]` /
  `libusb_cancel_transfer` → async: queue on the bridge, completion message wakes
  the shim's event loop which fires the transfer callback. The shim owns its own
  event fd/condvar — no fake usbfs `poll` needed (the whole point of choosing B).
* `libusb_hotplug_register_callback` → record; dispatched from HOTPLUG messages.

Env exported to the guest by the loader: `ALR_USB_SOCK=<cacheDir>/alr-usb/usbd.sock`
(set alongside the existing `WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR`/`ALR_*` vars in
the guest launch env).

---

## 4. Wire protocol (shim ↔ bridge, over AF_UNIX)

Length-prefixed binary, little-endian, mirroring the GPU ring's framing
philosophy (one fixed header + payload). One request → one reply, plus
unsolicited async messages (transfer completion, hotplug) tagged by opcode.

```
struct alr_usb_hdr { u32 len; u16 op; u16 flags; u32 tag; }  // tag correlates reply
```

Opcodes (request → reply):

| op | request payload | reply payload |
|---|---|---|
| `ENUMERATE` | — | count + [vid,pid,bus,addr,class] … |
| `OPEN` | vid,pid,bus,addr | connId, rawDescriptors blob \| errno |
| `CLOSE` | connId | ok |
| `CLAIM` / `RELEASE` | connId, ifaceNum, force | ok \| errno |
| `SET_CONFIG` / `SET_ALT` | connId, value | ok \| errno |
| `CONTROL` | connId, bmRequestType,bRequest,wValue,wIndex,wLength, [data if OUT], timeout | bytesTransferred, [data if IN] \| errno |
| `XFER` | connId, epAddr, type(bulk/int), length, [data if OUT], timeout | bytesTransferred, [data if IN] \| errno |
| `SUBMIT` (async) | connId, epAddr, type, length, [data], userTag | (immediate ack; result arrives as `COMPLETE`) |
| `CANCEL` | userTag | ok |
| `CLEAR_HALT` | connId, epAddr | ok \| errno |

Async (bridge → shim, unsolicited):

| op | payload |
|---|---|
| `COMPLETE` | userTag, status, bytesTransferred, [data if IN] |
| `HOTPLUG` | added/removed, vid, pid, bus, addr |

errno values use the libusb error mapping (negative `LIBUSB_ERROR_*`), so the
shim returns them verbatim. The bridge maps Android failures: a negative
`controlTransfer`/`bulkTransfer` return → `LIBUSB_ERROR_IO`; permission deny →
`LIBUSB_ERROR_ACCESS`; detached mid-transfer → `LIBUSB_ERROR_NO_DEVICE`; timeout
→ `LIBUSB_ERROR_TIMEOUT`.

---

## 5. JNI boundary (only needed to start/stop the bridge from native, optional)

The bridge is pure Kotlin/Java (it must call `UsbManager`, which has no NDK
equivalent — **all USB host access is Java-only on Android; there is no native
libusb/`/dev/bus/usb` path**, which is itself the reason this whole bridge
exists). So the *transport* is a UNIX socket, not JNI — the guest shim connects
to a socket, exactly like the Wayland client connects to the compositor socket.

The only JNI touchpoint is lifecycle, and it can be avoided entirely: the guest
launcher already sets guest env; MainActivity just needs to (a) construct
`UsbHostBridge(cacheDir, usbManager)` and (b) put `ALR_USB_SOCK` into the guest
launch environment. No new native method is strictly required. If a native-side
"is the USB bridge up?" probe is wanted to match the report harness, add a
trivial:

```cpp
// runtime_report.cpp (owner-added, same convention as nativeWaylandCompositorStatus)
extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeUsbBridgeStatus(JNIEnv* env, jobject) {
    // just reports whether $ALR_USB_SOCK exists + is connectable; pure diagnostics
}
```

Kotlin side: `private external fun nativeUsbBridgeStatus(): String`. This is
optional diagnostics only.

---

## 6. SELinux / fd boundary — what is and isn't allowed

* **No SELinux bypass; nothing here needs one.** `UsbManager.openDevice` returns
  an fd the system already granted to our app's domain (`untrusted_app`); that fd
  is legitimately ours. In Option B we never even pass the fd to the guest — the
  guest only ever sees our UNIX socket and our protocol, and *we* do the
  `controlTransfer`/`bulkTransfer` in our own process. So the SELinux story is
  trivial: same-process Java calls on a system-granted handle.
* In Option A we would `dup` the `getFileDescriptor()` fd to the guest and let
  guest libusb `ioctl` it directly. That *can* work because the fd is in our
  process and `untrusted_app` is allowed `ioctl` on it — but the guest would issue
  raw `USBDEVFS_*` ioctls that the Android USB stack's fd may or may not honor
  identically to a real usbfs node (Android's connection fd is a usbfs fd under
  the hood, so many `USBDEVFS_*` actually *do* work directly on it — this is the
  one redeeming property of Option A and why a *hybrid* is possible, see §8). No
  SELinux policy edit either way.
* `UsbDeviceConnection.reset()` does not exist publicly; a guest
  `libusb_reset_device` maps to a best-effort SET_CONFIGURATION or returns
  `LIBUSB_ERROR_NOT_SUPPORTED`. Honest gap.
* Kernel driver detach (`USBDEVFS_DISCONNECT`): on Android the kernel usually has
  no driver bound to an OTG device for a non-system app, so detach is usually a
  no-op success. Storage/HID class devices the system itself claims may be
  unavailable (the system already owns them) — that's a platform limit, reported
  as `LIBUSB_ERROR_BUSY`/`ACCESS`, not something we can override without root.

---

## 7. Data flow (end-to-end, Option B)

```
guest app
  └─ libusb_control_transfer()                     (unmodified app code)
      └─ alr_libusb_shim.so  (LD_PRELOAD / rootfs overlay; OUR libusb-1.0.so.0)
          └─ write CONTROL frame → AF_UNIX $ALR_USB_SOCK
              └─ UsbHostBridge (Kotlin, USB I/O thread)
                  └─ UsbDeviceConnection.controlTransfer(...)
                      └─ Android USB stack → kernel usbfs → device
                  ←─ bytesTransferred (+ IN data)
              ←─ CONTROL reply frame
          ←─ return n to libusb caller
  ←─ app gets its bytes
```

Hotplug/async ride the same socket as unsolicited `HOTPLUG`/`COMPLETE` frames the
shim's reader thread dispatches into libusb callbacks.

---

## 8. Optional hybrid (future, only if a non-libusb raw app appears)

Because Android's `UsbDeviceConnection.getFileDescriptor()` is itself a usbfs fd,
a thin Option-A layer can be added later *for that fd only*: the interposer's (to
be added) `ioctl` wrapper passes `USBDEVFS_*` straight through to the dup'd
Android fd, and only `open("/dev/bus/usb/...")` is virtualized to return that fd.
That reuses the kernel's real usbfs for the hard ioctls instead of emulating
them, dodging Option A's worst cost. This is explicitly future work; not built
now.

---

## 9. Owner wiring (the precise minimal edits)

The owner is the MAIN session (MainActivity) plus, optionally, the
runtime_report.cpp owner. Minimal edits:

1. **`app/src/main/AndroidManifest.xml`** — add
   `<uses-feature android:name="android.hardware.usb.host" android:required="false" />`
   (and, optionally, the `USB_DEVICE_ATTACHED` intent-filter + `@xml/usb_device_filter`
   meta-data on `.ui.RunningSurfaceActivity` for auto-grant UX).

2. **`MainActivity.kt`** (or `RunningSurfaceActivity`, wherever the guest is
   launched) — in the same place the guest launch env is assembled (next to the
   existing `WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR`/`ALR_*` exports):
   ```kotlin
   val usbBridge = dev.chanwoo.androlinux.usb.UsbHostBridge(
       cacheDir = cacheDir,
       usbManager = getSystemService(Context.USB_SERVICE) as UsbManager,
   ).also { it.start() }                       // creates <cacheDir>/alr-usb/usbd.sock
   // add to the guest env map passed to the loader:
   guestEnv["ALR_USB_SOCK"] = usbBridge.socketPath
   ```
   `UsbHostBridge` is a NEW self-contained Kotlin file
   (`app/src/main/java/dev/chanwoo/androlinux/usb/UsbHostBridge.kt`) — the owner
   adds it; this design doc owns only itself, not that file.

3. **Guest loader env** — ensure `ALR_USB_SOCK` reaches the guest process env
   (same mechanism that already exports `WAYLAND_DISPLAY`).

4. **Build** — add `alr_usb/guest_shim/alr_libusb_shim.c` as an NDK shared-lib
   target producing `libusb-1.0.so.0` (model: the GLES guest shim CMake under
   `alr_gpu/guest_shim`), and overlay/`LD_PRELOAD` it so it shadows the rootfs
   libusb.

5. **(Optional)** `runtime_report.cpp`: add
   `Java_..._nativeUsbBridgeStatus` + Kotlin `external fun nativeUsbBridgeStatus()`
   for the diagnostic report screen (pure observability).

No edits to `alr_compositor.cpp`, the interposer, or the loader are required for
Option B. (Option A *would* require adding an `ioctl` wrapper to
`libalr_interpose.c` and a `/dev/bus/usb` open special-case — noted, not built.)

---

## 10. Honest gaps / non-goals

* Covers **libusb apps**; raw `/dev/bus/usb` and `udev`-watching apps are not
  covered by Option B (future Option-A/hybrid or udev-shim work).
* `libusb_reset_device`, kernel-driver detach for system-claimed devices, and USB
  device-mode (gadget) are not achievable without root / are platform-limited.
* Isochronous transfers: Android `UsbRequest` supports bulk/interrupt well;
  isoc support via `UsbDeviceConnection` is limited — webcams (UVC isoc) may not
  work; report `LIBUSB_ERROR_NOT_SUPPORTED` honestly rather than silently failing.
* Performance: every transfer is a socket round trip + a Java call; fine for
  HID/serial/printer/Arduino/PTP; high-throughput bulk (mass storage) will be
  slower than native usbfs but functionally correct. The async batch path keeps
  it usable.
* This document is HOST-ONLY design; nothing here has been device-verified.
```
