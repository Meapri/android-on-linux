# Generalizing the maintscript-shim → the X11-viewer + apt-Qt-GUI + gnome-calc unlock

**Status:** host-side, build-green (`compileDebugKotlin` + full host pytest 2047 passed).
Device-verify pending (device owned by a concurrent agent). Code:
`AptInstaller.MAINTSCRIPT_SHIM_OVERLAY` (the gating split), `tools/app_closure_audit.py`
(the re-classified model), `BundledCatalog` (the re-added apps + the `needsXwayland` SSOT),
the UI launch-request builders (protocol=X11 wiring). The stub overlay itself is unchanged
(`tools/build_maintscript_shim_overlay.py`).

## 1. The gap

The general maintainer-script neutralizers — a no-op debconf `confmodule`, `policy-rc.d`=101,
`ucf`/`update-rc.d`/`invoke-rc.d`/`deb-systemd-helper`/`dpkg-reconfigure` stubs, and a
`dpkg-maintscript-helper` whose `supports`→0 and `rm_conffile`/`mv_conffile`→0 — were built
to fix the gnome-calculator `appstream` PREINST exit-73, but were **gated to gnome-platform
packages only** (`AptInstaller.GNOME_CONFIGURE_OVERLAYS`, applied when `isGnomePlatform`).

Consequence (device-proven): every NON-gnome app whose closure pulls `x11-common`/`libpaper1`
still failed `dpkg --configure` — `apt install xpdf` → `x11-common` postinst **exit 127**
(sources the confmodule + calls `update-rc.d`/`invoke-rc.d`, none in the base) + `libpaper1`
postinst **exit 2** (`. confmodule; db_get; ucf` under `set -e`, no frontend, no `ucf`). The
same blocker sank the whole X11-image-viewer class (`nsxiv`/`feh`/`qiv`) and every apt Qt-GUI
app (`libqt6gui6t64` → `libsm6` → `x11-common`).

## 2. The fix: split the overlay set by WHEN it applies

`AptInstaller` (file:line — `MAINTSCRIPT_SHIM_OVERLAY` / `GNOME_CONFIGURE_OVERLAYS`):

| overlay group | members | staged when |
|---------------|---------|-------------|
| **`MAINTSCRIPT_SHIM_OVERLAY`** (general) | `maintscript-shim` | **EVERY** install (`install`, `installStaged`, `remove`) |
| `GNOME_CONFIGURE_OVERLAYS` (gnome-specific) | `gnome-schemas` (precompiled `gschemas.compiled`) | only `isGnomePlatformPkg(pkg)` |

So a plain X11/Qt app now gets the maintscript neutralizer (installs cleanly) **without** the
gnome dbus/schema baggage; the gnome path is unchanged (it gets BOTH the general shim and the
gnome-specific gschemas + the runtime `dbus-run-session` shim from `NativeAppSession`). The
order is preserved so the shim's `dpkg-maintscript-helper` stub overwrites the real one staged
by `apt-dpkg` (it is staged AFTER the apt overlays). `DEBIAN_FRONTEND=noninteractive` +
`DEBCONF_NONINTERACTIVE_SEEN=true` are now set for every install (the stub confmodule is always
present), not just gnome.

**Why it is safe to apply to ALL installs:** every member is a successful no-op for state that
is meaningless in a non-root, no-systemd, single-process guest — no init to register into, no
debconf db to seed, no systemd unit to enable, and the conffile churn `dpkg-maintscript-helper`
performs is an upgrade-time cleanup (cosmetic for a from-scratch install). It changes no syscall
behaviour and weakens no sandbox. A galculator-class app (whose delta has none of the neutralized
triggers) never sources these stubs, so its install is byte-identical.

## 3. The re-classified audit model (`tools/app_closure_audit.py`)

The maintainer-script triggers split into two sets; only the second makes an app HEAVY now.

### NEUTRALIZED_BY_SHIM — the always-applied shim drives these to exit 0
| class | packages | the stub that fixes it |
|-------|----------|------------------------|
| X11 debconf/init-script | `x11-common` (was exit 127), `libpaper1` (was exit 2), `xfonts-utils`, `xfonts-encodings`, `xfonts-base`, `xserver-common` | confmodule + `update-rc.d`/`invoke-rc.d`/`ucf` |
| conffile-maintscript | `appstream`, `libappstream5`, `libappstream4` | `dpkg-maintscript-helper` (`rm_conffile`/`mv_conffile`→0) |
| systemd/session registration | `session-migration` | `deb-systemd-helper` (already `|| true`-guarded) |
| gsettings/gio registration | `gsettings-desktop-schemas`, `glib-networking[-services][-common]` | dpkg-trigger no-op (no `libglib2.0-bin` interest holder); gnome apps also get the precompiled gschemas overlay |

### CASCADE_TRIGGERS — GENUINELY-unsatisfiable, the shim cannot fake
| class | packages | why the shim cannot fix it |
|-------|----------|----------------------------|
| perl/dict | `perl`, `perl-base`, `dictionaries-common`, `emacsen-common` | real perl `update-*` registration + `dpkg-reconfigure` over installed word-lists (not a debconf-read no-op) |
| sandbox | `bubblewrap`, `ghostscript` | setuid/namespace install / font+ICC cache rebuild |
| daemon | `gstreamer1.0-plugins-*`, `avahi-daemon`, `cups-daemon`, `rtkit`, `policykit-1`, `polkitd`, `accountsservice`, `packagekit`, `colord` | each `systemctl enable`s a service / claims a bus name — no init, no system bus |

`is_cascade_trigger(pkg)` returns False for any `NEUTRALIZED_BY_SHIM` member; an app is
**LIKELY-PASS** iff its install delta has zero `CASCADE_TRIGGERS` and zero unsatisfied deps.
(`systemd`/`dbus`/`dconf-service`/`libpam-systemd` remain in neither set — galculator proves
them inert.)

## 4. Re-validated against the device-proven ground truth (`--live` noble index)

The re-classified model still reproduces the device PASS/FAIL truth, and now FLIPS the
shim-unlocked classes:

| package | old verdict | NEW verdict | discriminating triggers (delta) |
|---------|-------------|-------------|---------------------------------|
| galculator, l3afpad, htop, foot, netsurf-gtk | PASS | **LIKELY-PASS** | (none) |
| gimp | PASS | **ALREADY-INSTALLED** | base-provided (apt no-op) |
| **gnome-calculator** | HEAVY | **LIKELY-PASS** (flips) | appstream + session-migration + gsettings + glib-networking* — ALL neutralized |
| **xpdf** | HEAVY | **LIKELY-PASS** (flips) | x11-common, libpaper1 — neutralized |
| **nsxiv** / **feh** / **qiv** | HEAVY | **LIKELY-PASS** (flips) | x11-common, libpaper1, xfonts-* — neutralized |
| **qpdfview** (Qt) | HEAVY | **LIKELY-PASS** (flips) | x11-common (via libsm6) — neutralized |
| mousepad, gedit | HEAVY | **HEAVY** (stays) | perl-base, dictionaries-common, emacsen-common |
| eog, nautilus | HEAVY | **HEAVY** (stays) | **bubblewrap** (+ neutralized gsettings — but bwrap remains) |
| evince | HEAVY | **HEAVY** (stays) | bubblewrap, perl, gstreamer1.0-plugins-base |

The crux of the still-HEAVY rows: `eog`/`nautilus` keep `bubblewrap` (a genuine
setuid/namespace blocker) even though their `gsettings-desktop-schemas`/`session-migration` are
now neutralized — that is WHY they stay HEAVY while `gnome-calculator` (no bubblewrap) flips.

## 5. Re-added catalog apps (`BundledCatalog`)

All routed through the ROOTFUL Xwayland (`needsXwayland = true`) because their EXEC binary links
`libX11`/Motif/Qt-xcb but NOT `libwayland-client` (host: `tools/elf_needed` on each `.deb`):

| appId | apt pkg | toolkit | DT_NEEDED (relevant) | delta | tile? |
|-------|---------|---------|----------------------|------:|-------|
| nsxiv | nsxiv | X11 (suckless) | libX11 | 33 | no (.desktop NoDisplay/MimeType) |
| feh   | feh   | X11 (Imlib2) | libX11 | 37 | no (.desktop-less) |
| qiv   | qiv   | GTK2-x11 | libgdk-x11, libX11 | 40 | no (.desktop-less) |
| xpdf  | xpdf  | Motif/Xt | libXm, libX11, libXt (closure) | 18 | no (.desktop-less) |
| qpdfview | qpdfview | **Qt6** | (xcb plugin at runtime; no qt6-wayland in closure) | 77 | yes (qpdfview.desktop) |

`qpdfview` is the apt-Qt-GUI representative: its only blocker was `x11-common`; it ships no
`qt6-wayland` platform plugin in its closure, so Qt's xcb plugin connects to the rootful
Xwayland (DISPLAY=:0). This is distinct from the OVERLAY-delivered qmleasing Qt-on-Wayland demo
(`tools/build_toolkit_overlays.py qt6-gui`), which needs no apt.

### Routing wiring (no NativeAppSession edit)

`XwaylandLaunch.needsX11` already fires on `protocol == SurfaceProtocol.X11`. Rather than touch
the chromium-owned `NativeAppSession.X11_ONLY_APP_IDS` set, the catalog is the SSOT:
`BundledCatalog.needsXwayland(appId)` is DERIVED from the entries' `needsXwayland` flag, and the
UI launch-request builders (`AlrApp.toLaunchRequest` for CatalogApp + InstalledApp,
`LauncherViewModel.toLauncherRequest`, `AppDetailViewModel.open`) set
`protocol = SurfaceProtocol.X11` for those apps. So adding an X11 entry to the catalog is the
ONLY edit needed to route it.

## 6. Device-verify checklist

Prereq: device free; overlays pushed: `maintscript-shim-stage.tar` (now staged for EVERY
install), `xwayland-stage.tar` (`/usr/bin/Xwayland`), plus the per-app online apt closure.

### A. X11 image viewer — nsxiv (the headline unlock)
```
# install (online apt; now LIKELY-PASS — maintscript-shim auto-applied):
echo install:nsxiv > /data/local/tmp/.alr-aptdrain   # or in-app catalog → "nsxiv"
```
**Expected (PASS):**
- `logcat alr_loader`: `aptinstall: staging general maintscript-shim (always) for pkg=nsxiv`.
- install reaches `installed=true` (NOT the exit-73 `dpkg --configure -a` cascade) — the
  x11-common/libpaper1/xfonts-* postinsts exit 0 via the stub confmodule/update-rc.d/ucf.
- launch (`nsxiv test.png` via the explicit appId map): `logcat alr_runtime`
  `[nsxiv] X11 routing: Xwayland :0 ready=true (DISPLAY=:0)` + the image window renders on the
  SurfaceView via Xwayland → wl_shm (the xcalc-proven path). No "cannot open display".

### B. apt Qt-GUI — qpdfview (the Qt unlock)
```
echo install:qpdfview > /data/local/tmp/.alr-aptdrain   # or in-app catalog → "qpdfview"
```
**Expected (PASS):**
- install `installed=true` (x11-common via libsm6 neutralized; delta 77 configures clean).
- qpdfview.desktop tile auto-reconciles; launch routes through Xwayland (Qt xcb plugin →
  DISPLAY=:0) → the Qt PDF window renders on the SurfaceView.

### C. gnome-calculator (flips reachable)
```
echo install:gnome-calculator > /data/local/tmp/.alr-aptdrain
```
**Expected (PASS):**
- `logcat`: `aptinstall: pkg=gnome-calculator is gnome-platform — ALSO staging [gnome-schemas]`.
- install `installed=true` (appstream PREINST + session-migration + gsettings postinsts exit 0
  via the now-always shim; gschemas resolved by the gnome-schemas overlay).

### D. Regression guards
- `galculator`/`gpicview`/GIMP launch byte-identically (Wayland-direct; no X11 routing log) —
  the `needsXwayland` default-false path is unchanged, and the always-staged shim is a no-op for
  their delta.
- A still-HEAVY app (`mousepad` or `eog`) install still reports the honest partial/failed
  closure (perl/bubblewrap postinst) — the audit correctly leaves them HEAVY.

## 7. Honest limits
- All device items are PENDING (device owned by a concurrent agent). The change is
  host-analyzable: the gating split is source-asserted; the install verdicts are from the live
  noble index re-validated against the device-proven PASS/FAIL ground truth; the DT_NEEDED
  X11/Qt-xcb classification is from the real `.deb` binaries.
- `gsettings-desktop-schemas`/`glib-networking*` are placed in NEUTRALIZED_BY_SHIM on the basis
  that the cascade they were blamed for is the appstream/session-migration class the shim fixes,
  and their own registration is a dpkg-trigger no-op (no `libglib2.0-bin` interest holder) — a
  cosmetic gap, NOT a hard failure. This specific axis is a device-re-verify item; the still-HEAVY
  set's verdicts (driven by bubblewrap/perl/daemons) do not depend on it.
