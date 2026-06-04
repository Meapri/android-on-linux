# Heavy-app feasibility — what actually blocks GNOME / Qt / daemon-class Linux apps on ALR

**Status:** host-audited (no device required). The PASS/FAIL model here reproduces the
device-proven ground truth 10/10 (see "Validation"). Tooling: `tools/app_closure_audit.py`
(+ `tests/test_app_closure_audit.py`). Base: Ubuntu noble 24.04 arm64, reconstructed dpkg
admin DB via `tools/build_dpkg_db.py`.

This doc exists so we **stop over-promising GNOME**. It states precisely which heavy-app
classes are structurally reachable, which become reachable only if we add a
session-dbus / dconf shim, and which are out regardless.

---

## 1. The misconception we are correcting

The intuitive rule "if an app's dependency closure pulls `systemd` / `dbus` /
`dconf-service` / `libpam-systemd`, it will fail `dpkg --configure` on a non-root,
no-systemd device" is **WRONG**, and acting on it both (a) falsely condemns apps that
actually work and (b) gives no insight into the ones that really fail.

Proof: **galculator is device-PROVEN** to install and configure exit-0, yet its full
noble Depends closure is **157 packages and includes `systemd`, `systemd-sysv`, `dbus`,
`dbus-daemon`, `dconf-service`, `dconf-gsettings-backend`, `libdconf1`, `libpam-systemd`,
`init-system-helpers`** — pulled transitively through `libgtk-3-0t64`'s own `Depends`
(`shared-mime-info`, `adwaita-icon-theme`, …). `gpicview`, `l3afpad`, `sakura`,
`xarchiver` have the *identical* support set. So every GTK3 app drags the systemd/dbus
chain, and it is inert here: on the device those packages unpack, `ldconfig` runs, **no
daemon is started**, and `dpkg --configure` exits 0.

## 2. What actually triggers the exit-73 cascade

apt only unpacks+configures the **install DELTA** — the closure minus what the base's
reconstructed dpkg DB already marks installed (the base ships the GTK3/glib/pango/cairo
`.so` files + Adwaita icons + compiled gschemas; `tools/build_dpkg_db.py` maps those
files/SONAMEs back to ~194 "installed" packages). Within that delta, the
`dpkg --configure -a` exit-73 failure is caused by a **small set of maintainer-script
packages** whose `postinst` needs a running init / dbus session / perl / registration
service. The validated trigger set (`CASCADE_TRIGGERS`) groups into four buckets:

| bucket | trigger packages | why the postinst needs more than the base has |
|--------|------------------|-----------------------------------------------|
| **perl/dict** | `perl-base`, `perl`, `dictionaries-common`, `emacsen-common` | run perl `update-*` registration scripts; `dictionaries-common` drives `dpkg-reconfigure` for installed dict word-lists |
| **gnome-platform** | `gsettings-desktop-schemas`, `appstream`/`libappstream5`, `session-migration`, `glib-networking[-services]` | gsettings schema compile + appstream metadata pool refresh + session-migration trigger; these assume a GNOME session / D-Bus activation |
| **sandbox** | `bubblewrap`, `ghostscript` | `bwrap` setuid/namespace install assumptions; ghostscript font/ICC cache postinst |
| **daemon** | `gstreamer1.0-plugins-*`, `avahi-daemon`, `cups-daemon`, `rtkit`, `polkitd`/`policykit-1`, `accountsservice`, `packagekit`, `colord` | each ships a real daemon whose postinst registers/`systemctl enable`s a service or a D-Bus system-bus name |

**A package is LIKELY-PASS iff its install delta contains ZERO of these triggers** — i.e.
its delta is galculator-class. Otherwise HEAVY.

> Deliberately EXCLUDED from triggers (this is the crux): `systemd`, `systemd-sysv`,
> `dbus`, `dbus-daemon`, `dconf-service`, `dconf-gsettings-backend`, `libpam-systemd`,
> `init-system-helpers`. galculator ships all of them in its delta and configures exit-0,
> so by construction they cannot be the cause of any failure.

### 2a. The X11 debconf/init-script postinst trigger (xpdf-class — device-corrected)

A fifth bucket was added after a **device-proven miss**: `apt install xpdf` failed
`dpkg --configure` with **`x11-common` postinst exit 127** + **`libpaper1` postinst exit 2**,
yet the model had scored xpdf LIKELY-PASS. The maintainer-script forensics (reading the
real noble `.deb` control archives) pin the cause: `libpaper1`'s postinst's FIRST line is
`. /usr/share/debconf/confmodule` (the base ships no confmodule → sourcing it errors →
exit 2) then `db_get … ; ucf …` (no debconf frontend, no `ucf`); `x11-common`'s postinst
sources the confmodule (`db_purge`) and may call `update-rc.d`/`invoke-rc.d` (absent →
127). So the trigger set now also includes **`x11-common`, `libpaper1`** and their X11
siblings **`xfonts-utils`, `xfonts-encodings`, `xfonts-base`, `xserver-common`** (same
`update-fonts-*`/`update-rc.d`/debconf postinst class).

> This is NOT "all X11". `xcalc` (package `x11-apps`) is **not in noble main+universe** —
> it is staged via the dedicated `x11-stage.tar` overlay, never an apt-configure, so it is
> unaffected (it audits MISSING, not HEAVY). The trigger is specifically the
> debconf/init-script postinst set in the *apt-install* delta.

### 2a′. SUPERSEDED — those buckets split into NEUTRALIZED vs GENUINELY-HEAVY

The sections above are the FORENSIC HISTORY (how each trigger was discovered). The CURRENT
model re-classifies them, because the **general maintscript-shim** is now staged for EVERY
apt install (`AptInstaller.MAINTSCRIPT_SHIM_OVERLAY`, generalized from gnome-only — see
`docs/research/maintscript-shim-generalization.md`). The shim's no-op stubs (confmodule,
`policy-rc.d`, `ucf`, `update-rc.d`/`invoke-rc.d`, `deb-systemd-helper`, `dpkg-reconfigure`,
`dpkg-maintscript-helper`) drive exactly the DEBCONF / INIT-SCRIPT / CONFFILE-MAINTSCRIPT /
registration class to exit 0. So:

* **NEUTRALIZED_BY_SHIM** (no longer a trigger): the entire **X11 debconf/init bucket** above
  (`x11-common`, `libpaper1`, `xfonts-*`, `xserver-common`), the conffile-maintscript class
  (`appstream`/`libappstream*`), `session-migration`, and the gsettings/gio registration
  (`gsettings-desktop-schemas`, `glib-networking*`). An app HEAVY *only* for these is now
  LIKELY-PASS — this is the X11-image-viewer (`nsxiv`/`feh`/`qiv`/`xpdf`) + apt-Qt-GUI
  (`qpdfview`) + gnome-calculator unlock.
* **CASCADE_TRIGGERS** (GENUINELY-heavy, the shim cannot fake): the **perl/dict**,
  **sandbox** (`bubblewrap`/`ghostscript`), and **daemon** buckets. These stay HEAVY.

Device ground truth still holds: `gnome-calculator` FLIPS to reachable (its blockers were all
the neutralized class); `mousepad`/`gedit` stay HEAVY (perl/dict); `eog`/`nautilus`/`evince`
stay HEAVY (**bubblewrap** — even with their gsettings neutralized, the sandbox postinst
remains). The discriminator that keeps eog HEAVY while gnome-calculator flips is precisely
`bubblewrap` ∈ eog's delta, ∉ gnome-calculator's.

### 2b. The ALREADY-INSTALLED short-circuit (why gimp stays PASS)

Adding x11-common/libpaper1 as triggers exposed a subtlety: **gimp's *closure* drags both**
(gimp → poppler/motif → libpaper1, x11-common), yet gimp is device-PROVEN PASS. Resolution:
**gimp is base-PROVIDED** (the base rootfs ships `/usr/bin/gimp`; the reconstructed dpkg DB
marks `gimp`+`gimp-data` installed). `apt install gimp` is therefore a **no-op** — no
maintainer script in its closure ever runs, so those postinsts cannot fail. The audit now
checks this FIRST: a target already in the base installed-set is `ALREADY-INSTALLED`
(PASS-class), regardless of triggers buried in its already-satisfied closure. Of the six
proven-PASS apps only gimp is base-provided; the other five are real apt installs that
don't pull x11-common/libpaper1. This keeps the **10/10** match while the X11 triggers are
correctly active (xpdf → HEAVY, and its sibling **nsxiv** → HEAVY — both dropped from the
catalog).

## 3. Validation (model vs device ground truth)

`tools/app_closure_audit.py --live` against the real noble index + Contents, scored
against project-memory device results:

| app | device truth | model | trigger buckets |
|-----|:------------:|:-----:|-----------------|
| galculator | PASS | LIKELY-PASS | — |
| l3afpad | PASS | LIKELY-PASS | — |
| htop | PASS | LIKELY-PASS | — |
| gimp | PASS | **ALREADY-INSTALLED** | — (base-provided; apt no-op) |
| foot | PASS | LIKELY-PASS | — |
| netsurf-gtk | PASS | LIKELY-PASS | — |
| mousepad | FAIL | HEAVY | perl |
| gnome-calculator | FAIL | HEAVY | gnome |
| gedit | FAIL | HEAVY | perl, gnome |
| eog | FAIL | HEAVY | gnome, sandbox |
| **xpdf** | **FAIL** | **HEAVY** | **x11 (x11-common+libpaper1)** |
| **nsxiv** | (xpdf-sibling) | **HEAVY** | **x11 (x11-common+libpaper1+xfonts)** |

**10/10 match** on the original ground truth, AND the device-corrected xpdf now predicts
HEAVY (it previously under-counted as LIKELY-PASS). (`tests/test_app_closure_audit.py::
test_live_audit_matches_device_ground_truth` + `::test_xpdf_is_heavy_via_x11_common_and_libpaper1`,
network-gated `ALR_AUDIT_NET=1`.)

## 4. Heavy-app class breakdown — what blocks each, and the shim path

Representative apps, audited (delta size + trigger buckets):

| app | verdict | delta | buckets | what blocks it |
|-----|:-------:|----:|---------|----------------|
| gnome-calculator | HEAVY | 78 | gnome | gsettings-desktop-schemas + libappstream5 + session-migration postinsts |
| gnome-text-editor | HEAVY | 75 | perl, gnome | + dictionaries-common (perl) |
| gnome-terminal | HEAVY | 76 | gnome | gsettings/dconf registration (also: needs a real PTY host — separate track) |
| gedit | HEAVY | 108 | perl, gnome | gspell/gtksourceview dict + gnome platform |
| eog | HEAVY | 88 | gnome, sandbox | gnome platform + `bubblewrap` (thumbnailer sandbox) |
| file-roller | HEAVY | 69 | gnome | nautilus/gnome integration |
| nautilus | HEAVY | 180 | gnome, sandbox | full gnome platform + tracker/bubblewrap |
| evince | HEAVY | 117 | perl, gnome, sandbox, gstreamer | the worst stack: all four buckets |
| atril (MATE) | HEAVY | 183 | perl, gnome, sandbox, gstreamer | same as evince |
| mousepad (XFCE) | HEAVY | 66 | perl | gspell/gtksourceview pull perl + dictionaries-common |
| xfce4-terminal | HEAVY | 78 | perl | perl maintainer script (also needs PTY host) |

### Reachability classes

**(A) Structurally OUT (do not promise, even with shims):**
- Anything in the **daemon** bucket whose *function* is the daemon: `cups`/printing,
  `avahi`/mDNS, `packagekit`, `accountsservice`, `colord`, `polkit`-gated admin tools.
  These need a live init + system D-Bus + privilege model that a non-root single-process
  Android guest does not have. The postinst failure is the least of it — the app's runtime
  contract is a running system service.
- Apps that assume **systemd-as-PID1** for their own lifecycle (login managers, session
  daemons, `gnome-session`, the GNOME Shell desktop itself).

**(B) Reachable with the session-dbus / schema-compile / skip-triggers shim — NOW BUILT:**
- The **gnome-platform** bucket (gnome-calculator, gnome-text-editor, eog, file-roller)
  fails at *configure* on `gsettings-desktop-schemas` / `appstream` / `session-migration`
  postinsts and at *runtime* on uncompiled schemas + a missing session bus. The three-part
  shim that unblocks this is now implemented (host-built artifacts + launcher wiring):
  - **(i) install-configure neutralizer** — `tools/build_maintscript_shim_overlay.py` ships
    `maintscript-shim-stage.tar`: a NO-OP debconf `confmodule` (every `db_*` returns 0),
    `policy-rc.d` → 101, and `exit 0` stubs for `ucf`/`update-rc.d`/`invoke-rc.d`/
    `deb-systemd-helper`/`dpkg-reconfigure`. The forensics show the base ships NONE of these
    (only `dpkg-trigger`), so `libpaper1` (sources confmodule → exit 2) and `x11-common`
    (debconf/init → exit 127) and `session-migration` postinsts run to exit 0 with the stubs
    present. `AptInstaller` stages this + sets `DEBIAN_FRONTEND=noninteractive` ONLY for a
    gnome-platform package. *(Selftest proves the stub confmodule drives the real libpaper1
    postinst body to exit 0.)*
  - **(ii) precompiled gschemas** — `tools/build_common_data_overlay.py --schemas
    --schema-package gnome-calculator` ships `gnome-schemas-stage.tar`: a host-run
    `glib-compile-schemas` over (base schemas ∪ gsettings-desktop-schemas ∪ the app's own
    `org.gnome.calculator`), so the device gets a `gschemas.compiled` SUPERSET with no
    on-device compiler. *(Verified: the real compiled binary contains `org.gnome.calculator`
    + `org.gnome.desktop.interface` + the base `org.gtk.*`, §5-E conformant.)*
  - **(iii) runtime session bus** — `tools/build_dbus_overlay.py` ships `dbus-daemon-stage.tar`
    (`/usr/bin/dbus-daemon` + `dbus-run-session`; the base has only the libdbus client). The
    launcher's `NativeAppSession.GnomePlatformShim` WRAPS a gnome-platform launch in
    `dbus-run-session -- <app>` (private session bus, `DBUS_SESSION_BUS_ADDRESS` exported,
    torn down on exit, all in the one blocking guest call — no loader change) and points
    `GSETTINGS_SCHEMA_DIR` at the rootfs schemas. The native runtime already sets
    `GSETTINGS_BACKEND=memory`, so settings WRITES go to memory (no dconf-service needed);
    only the compiled schema DEFAULTS (ii) + the bus for `GtkApplication` registration (iii)
    are required.
  - **HOST STATUS:** all three overlays build as real §5-E-conformant artifacts and the
    audit/builders are pytest-green; `gnome-calculator` is added to `BundledCatalog`
    (`org.gnome.Calculator`). **DEVICE STATUS:** the final "installs to `installed=true` →
    launches → window renders" is the DEVICE-VERIFY gate (the device was owned by another
    agent during this work) — see the checklist. Do not over-claim a device run.
  - **What it does NOT unlock:** evince/atril/nautilus (still drag sandbox + gstreamer +
    perl on top of gnome-platform), and the perl bucket below. gnome-terminal additionally
    needs a PTY host (separate track).
- The **perl** bucket (mousepad, xfce4-terminal, gedit's gspell) needs `perl-base` present
  and the `dictionaries-common` / `emacsen-common` `dpkg-reconfigure` to complete non-
  interactively. Adding `perl-base` to the base/overlay + a `debconf` non-interactive
  frontend (`DEBIAN_FRONTEND=noninteractive`) + a stub `dictionaries-common` config could
  clear it, but perl-base is a heavy add (~40 MB) for marginal apps; lower priority than
  the dbus shim.

**(C) Already reachable today (LIKELY-PASS, no shim) — under-promised:**
The delta model shows several "heavy-looking" apps actually configure clean today:

| app | verdict | delta | note |
|-----|:-------:|----:|------|
| **geany** | LIKELY-PASS | 54 | GTK3 IDE; galculator-class delta. Real candidate. |
| **thunar** | LIKELY-PASS | 73 | XFCE file manager; 0 triggers. |
| **mate-calc** | LIKELY-PASS | 56 | MATE calculator; 0 triggers (unlike gnome-calculator!). |
| **lxterminal** | LIKELY-PASS | 55 | LXDE terminal (needs PTY host to RUN, but configures clean). |
| **transmission-gtk** | LIKELY-PASS | 71 | GTK torrent client; 0 triggers. |
| **libreoffice-calc** | LIKELY-PASS | 106 | configures clean (runtime is a separate, large question). |
| **vlc** | LIKELY-PASS | 180 | configures clean; runtime needs GL/audio (separate tracks). |

> `mate-calc` LIKELY-PASS vs `gnome-calculator` HEAVY is the headline: pick the MATE/XFCE/
> LXDE equivalent of a GNOME app and it usually configures clean, because those desktops
> don't pull `gsettings-desktop-schemas`+`appstream`+`session-migration` as hard deps.

> **Caveat — configure-clean ≠ runs.** This audit predicts `dpkg --configure` success
> only. A LIKELY-PASS verdict for vlc/libreoffice/firefox means the *install* completes;
> whether the app then *launches and renders* depends on the GL/Vulkan, audio, font, and
> exec-re-entry tracks. Also: **`firefox` on noble is a 121 KiB snap-transition stub**
> (`Depends: debconf`) — it configures clean but is a snapd shim, not a runnable browser;
> do not list it as a working app.

## 5. Recommended posture (catalog honesty)

1. **Promise (device-proven or galculator-class host-audited):** galculator, l3afpad,
   htop, gimp, foot, netsurf-gtk, gpicview, xarchiver, sakura, viewnior, xzgv,
   qalculate-gtk. (See `BundledCatalog`.) **xpdf + nsxiv were DROPPED** — both pull the
   `x11-common`+`libpaper1` debconf/init-script postinsts that device-proved FAIL (the
   corrected §2a model now flags them HEAVY).
2. **Promote to "candidate, host-audited LIKELY-PASS" (configure-clean, run-pending):**
   geany, thunar, mate-calc, transmission-gtk. Light desktops (XFCE/MATE/LXDE) over GNOME.
3. **GNOME gnome-platform apps — shim now BUILT (§4 class B):** `gnome-calculator` is in
   `BundledCatalog` (`org.gnome.Calculator`) behind the install-configure neutralizer +
   precompiled-gschemas + session-dbus shim. The *gnome-platform-only* apps
   (gnome-text-editor/eog/file-roller) reuse the same path; evince/atril/nautilus are still
   out (sandbox + gstreamer + perl on top). DEVICE-VERIFY pending (see §7).
4. **Structurally out:** printing/mDNS/polkit/accountsservice/packagekit GUIs and anything
   that wants systemd-as-PID1 or a real privilege model.

## 6. How to reproduce / extend

```
# offline logic + selftest (audit model + the three gnome-platform overlay builders)
uvx pytest tests/test_app_closure_audit.py tests/test_build_common_data_overlay.py \
    tests/test_build_dbus_overlay.py tests/test_build_maintscript_shim_overlay.py -q

# live audit of any package set against real noble (matches device ground truth + xpdf)
python -m tools.app_closure_audit --live --base app/src/main/assets/rootfs/payloads/tiny-rootfs.tar \
    --package gnome-calculator --package mate-calc --package geany --package xpdf --json

# build the three gnome-platform overlays (real §5-E artifacts → out/)
BASE=app/src/main/assets/rootfs/payloads/tiny-rootfs.tar
python -m tools.build_common_data_overlay --out out/gnome-schemas-stage.tar \
    --schemas --schema-package gnome-calculator --component main --component universe --base $BASE
python -m tools.build_maintscript_shim_overlay --out out/maintscript-shim-stage.tar --base $BASE
python -m tools.build_dbus_overlay --out out/dbus-daemon-stage.tar --base $BASE --component main
```

The audit model is one function (`tools.app_closure_audit.classify`): if the target is
base-installed → `ALREADY-INSTALLED`; else resolve the noble closure, subtract the base
installed-set (`tools.build_dpkg_db.installed_packages` over the noble `Contents` index),
and report any `CASCADE_TRIGGERS` in the remaining delta.

## 7. DEVICE-VERIFY CHECKLIST (gnome-calculator + xpdf-correction)

Run on a real device (the host work above is green; these are the device gates). Always
`am force-stop dev.chanwoo.androlinux` before a re-launch (project memory: onCreate skips
overlays otherwise).

**Setup — push the three gnome overlays (+ the standing apt/interpose overlays):**
```
adb push out/gnome-schemas-stage.tar   /data/local/tmp/gnome-schemas-stage.tar
adb push out/maintscript-shim-stage.tar /data/local/tmp/maintscript-shim-stage.tar
adb push out/dbus-daemon-stage.tar     /data/local/tmp/dbus-daemon-stage.tar
# (apt path also needs the existing fakeroot/apt-dpkg/dpkg-db/apt-mirror/interpose tars)
```

**A. gnome-calculator INSTALL → `installed=true`:**
1. Trigger the in-app install of `org.gnome.Calculator` (or the marker path
   `echo install:gnome-calculator > /data/local/tmp/.alr-aptdrain`).
2. EXPECT logcat: `aptinstall: pkg=gnome-calculator is gnome-platform — also staging
   [maintscript-shim, gnome-schemas]`, then the `dpkg -i (set)` + `dpkg --configure -a`
   lines, then **`aptinstall: installed=true (dpkg --status gnome-calculator)`** and
   `Setting up gnome-calculator`. The libpaper1/x11-common/session-migration `Setting up`
   lines must NOT abort (the stub confmodule/policy-rc.d neutralize them).
3. CONFIRM no `dpkg --configure` exit-73 in the log; `/usr/bin/gnome-calculator` present.

**B. gnome-calculator LAUNCH → window renders:**
4. Tap the new `GNOME Calculator` launcher tile (it self-reconciles via DesktopEntryScanner
   from `org.gnome.Calculator.desktop`).
5. EXPECT logcat: `[org.gnome.Calculator] gnome-shim: wrapping launch in dbus-run-session
   (session bus + GSETTINGS_SCHEMA_DIR=/usr/share/glib-2.0/schemas)` and the program line
   `/usr/bin/dbus-run-session -- /usr/bin/gnome-calculator`.
6. EXPECT: NO `Settings schema 'org.gnome.desktop.interface' is not installed` abort (the
   precompiled gschemas overlay satisfies it); the calculator window renders on the
   compositor (screencap shows the GTK4 calculator, not black).
7. HONEST fallbacks to watch: if `dbus-run-session` isn't staged the log says "launching
   without a session bus" and GTK4 falls back (warning, not crash); if the window is black
   check the GTK4/wl_shm render path (separate track), not this shim.

**C. xpdf correction (negative):**
8. xpdf is no longer a catalog tile (dropped). The audit now marks it HEAVY; confirm
   `python -m tools.app_closure_audit --live … --package xpdf` reports `HEAVY` with
   `x11-common, libpaper1` — i.e. the app is correctly ABSENT/marked, not offered as a
   broken install.
