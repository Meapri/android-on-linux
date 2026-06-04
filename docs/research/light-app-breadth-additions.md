# Light-app breadth additions — gpicview/xarchiver verdict, new apps, device-verify checklist

**Status:** host-side breadth work (device-verify pending — device owned by concurrent
agents at time of writing). Tooling: `tools/app_closure_audit.py`, `tools/deb_closure.py`,
`tools/build_dpkg_db.py`. Companion: `docs/research/heavy-app-feasibility.md` (the model).

## 1. gpicview / xarchiver / sakura verdict (the AUDITED entries, re-audited)

**Verdict: all three are LIKELY-PASS (galculator-class) — KEEP.** But the *reasoning in
their old catalog comments was wrong* and has been corrected.

The old comments claimed each had "no systemd/dbus/dconf-service closure". **False.** Their
full noble closures (157–159 packages) drag `systemd`, `systemd-sysv`, `dbus`,
`dbus-daemon`, `dconf-service`, `dconf-gsettings-backend`, `libpam-systemd`,
`init-system-helpers` — pulled by `libgtk-3-0t64`'s own `Depends`, exactly like
device-PROVEN galculator/l3afpad. The correct basis for the LIKELY-PASS verdict is the
**install-delta cascade-trigger test** (see heavy-app-feasibility.md §2): their install
delta (closure − base installed-set) is galculator's 52-package envelope plus only their
own leaves, with **zero exit-73 maintainer-script triggers**:

| app | closure | delta | delta = galculator-envelope + … | cascade triggers |
|-----|----:|----:|---------------------------------|------------------|
| gpicview | 157 | 53 | `gpicview` (nothing else) | none |
| xarchiver | 157 | 53 | `xarchiver` | none |
| sakura | 159 | 55 | `sakura` + `libvte-2.91-0` + `libvte-2.91-common` | none |

Because their delta introduces no `perl-base`/`gsettings-desktop-schemas`/`appstream`/
`session-migration`/`bubblewrap`/`ghostscript`/`gstreamer` trigger, `dpkg --configure`
should exit-0 exactly as galculator does. The catalog comments + descriptions in
`NativeAlrRuntime.kt` were rewritten to state this correctly.

> **sakura caveat:** `sakura.desktop` is `Terminal=true`, which `DesktopEntryScanner`
> drops (Phase-1 no-PTY policy). It installs+launches via the catalog's explicit
> appId→apt map, but does NOT self-reconcile a scanned launcher tile. (Same situation as
> the existing `htop` entry.)

## 2. New apps added to BundledCatalog (host-audited LIKELY-PASS)

All audited LIKELY-PASS against the live noble index; appId == `.desktop` basename ==
apt package == binary basename (the reconciliation contract). Real DT_NEEDED-minimal
overlay tars were built host-side and pass `overlay_guard` + `stage_tar_spec` (see §4).

| appId | toolkit | category | own size | delta | what it ships beyond base | tile auto-reconciles? |
|-------|---------|----------|------:|----:|----------------------------|:---------------------:|
| **viewnior** | GTK3 | graphics | 687 KiB | 54 | `viewnior` + `libexiv2` | YES (NoDisplay=false) |
| **xzgv** | GTK2 | graphics | 319 KiB | 16 | `xzgv` + GTK2 stack (`libgtk-x11-2.0`, `libgdk-x11-2.0`, `libexif`) | YES |
| **xpdf** | Xlib/Motif | office | 331 KiB | 18 | `xpdf` + `libpoppler`, `libXm`(Motif), `libXft`, `libpaper` | YES |
| **qalculate-gtk** | GTK3 | utility | 6.6 MiB | 62 | `qalculate-gtk` + `libqalculate` + `libicu*` + `libmpfr` | YES |
| **nsxiv** | Xlib | graphics | 186 KiB | 33 | `nsxiv` + `libImlib2`, `libXft`, `libexif`, `libX11-xcb` | **NO** (NoDisplay=true) |

Why these (and why genuinely useful, not toys):
- **viewnior** — a more capable GTK3 image viewer than gpicview (rotate/slideshow/EXIF);
  same GTK3-on-Wayland path. Clean install→tile→launch loop.
- **xzgv** — thumbnail-grid image viewer; smallest GTK delta (16). NOTE it is **GTK2**, so
  the overlay correctly carries the GTK2 runtime the base lacks (base ships GTK3 only).
- **xpdf** — proves the **X11-via-Xwayland document-viewer breadth** (a pure Xlib/Motif app,
  not GTK). Tiny (119 KiB deb). Unlike evince/atril it has NO ghostscript/gstreamer/gnome
  triggers. Requires Xwayland (rootful) up, like the proven xcalc.
- **qalculate-gtk** — a vastly more powerful calculator than galculator (units, currency,
  symbolic algebra, plotting). Unlike `gnome-calculator` (HEAVY) it has no
  appstream/gsettings/session-migration trigger → configures clean.
- **nsxiv** — the maintained `sxiv` fork; representative ultra-light Xlib image viewer.
  Included because the task named it; **documented caveat**: its `.desktop` is
  `NoDisplay=true` (registered as a MIME handler), so it installs+launches from the catalog
  but does NOT auto-surface a scanned launcher tile.

Candidates DROPPED and why:
- **feh, sxiv, qiv** — LIKELY-PASS but `.desktop` is `NoDisplay=true` (no auto-tile);
  `nsxiv` already represents the Xlib-image-viewer class, so these were redundant.
- **mupdf** — LIKELY-PASS but `NoDisplay=true` AND a 48 MB statically-linked deb (heavy for
  a "light" tier); `xpdf` is the far lighter PDF representative.
- **geany, thunar, mate-calc, transmission-gtk** — audit LIKELY-PASS (configure-clean) but
  promoted to the "candidate, run-pending" list in heavy-app-feasibility.md §4(C) rather
  than the proven tier (they need device-launch confirmation; geany/thunar are strong next
  adds).
- **gnome-calculator/gedit/eog/mousepad/evince/atril/nautilus** — HEAVY (real exit-73
  cascade); see heavy-app-feasibility.md.

## 3. DEVICE-VERIFY CHECKLIST (batch — run when the device is free)

Prereqs (per project memory): `am force-stop` the app FIRST (else onCreate overlays/probes
are skipped); set `JAVA_HOME=openjdk@17` for the build; the apt online path needs the
seeded dpkg DB overlay + the apt-mirror-over-HTTP (no-DNS) path. For the X11 apps, an
**Xwayland (rootful)** session must be up (the same path that renders xcalc).

For each app: install by command, expect a launcher tile (except where noted), launch,
expect a window. The expected install result is `dpkg configured = true` (galculator-class).

| # | app | install command (in-app or `==install:<pkg>`) | expected after install | launch | expected on screen |
|---|-----|----------------------------------------------|------------------------|--------|--------------------|
| 1 | viewnior | `==install:viewnior` | tile "Viewnior" appears (DesktopEntryScanner) | tap tile / `viewnior /sdcard/test.png` | GTK3 window, image shown, rotate/slideshow work |
| 2 | xzgv | `==install:xzgv` | tile "xzgv" appears | tap tile / `xzgv /sdcard/Pictures` | GTK2 window, thumbnail grid renders |
| 3 | xpdf | `==install:xpdf` | tile "Xpdf" appears | tap tile / `xpdf /sdcard/test.pdf` (Xwayland up) | Motif PDF window, page renders via libpoppler |
| 4 | qalculate-gtk | `==install:qalculate-gtk` | tile "Qalculate!" appears | tap tile | GTK3 calculator window, type `2 m + 30 cm` → unit result |
| 5 | nsxiv | `==install:nsxiv` | **no auto-tile** (NoDisplay) — verify install only via `dpkg-query -W nsxiv` | `nsxiv /sdcard/test.png` (Xwayland up) | Xlib image window, image shown |
| 6 | gpicview (re-verify) | `==install:gpicview` | tile "GPicView" appears | tap tile / `gpicview /sdcard/test.png` | GTK3 window, image shown |
| 7 | xarchiver (re-verify) | `==install:xarchiver` | tile "Xarchiver" appears | tap tile / `xarchiver /sdcard/test.zip` | GTK3 window, archive listing |
| 8 | sakura (re-verify) | `==install:sakura` | **no auto-tile** (Terminal=true) — verify via `dpkg-query -W sakura` | launch from catalog | VTE terminal window (needs PTY host — separate track) |

**Pass criteria per row:**
- Install: the in-app progress reaches `configuring`/`registering` and the result is
  installed=true (NOT the exit-73 `dpkg --configure -a` failure that sank mousepad).
- Tile (rows 1–4, 6–7): the appId tile appears in the launcher grid post-install
  (DesktopEntryScanner picked up the now-present `.desktop`).
- Launch: a window appears on the ALR Wayland compositor (rows 1,2,4,6,7 GTK; rows 3,5
  Xlib via Xwayland).

**If a GTK app installs but `dpkg --configure` fails:** capture which package's postinst
errored — if it is one of `gsettings-desktop-schemas`/`appstream`/`perl-base`/
`dictionaries-common`, the audit model under-counted a trigger for that leaf (file a
correction to `CASCADE_TRIGGERS`); if it is `dbus`/`systemd`/`dconf-service`, that would
contradict galculator and points at a device regression in the seeded-DB/`policy-rc.d`
path, not the app.

## 4. Built overlay artifacts (host)

Real DT_NEEDED-minimal overlay tars built from the noble mirror, all `overlay_guard` OK +
`stage_tar_spec` OK + `missing_soname=[]`, written to `out/overlays/` (build output, not
committed — they are large binaries; rebuild with the commands below):

| overlay | bytes | ships |
|---------|------:|-------|
| `xpdf-stage.tar` | 6.95 MB | xpdf + libpoppler/libXm/libXft/libpaper |
| `viewnior-stage.tar` | 3.31 MB | viewnior + libexiv2 |
| `xzgv-stage.tar` | 6.35 MB | xzgv + GTK2 stack |
| `qalculate-gtk-stage.tar` | 43.4 MB | qalculate-gtk + libqalculate + libicu + libmpfr |
| `nsxiv-stage.tar` | 1.01 MB | nsxiv + libImlib2/libXft/libexif/libX11-xcb |

These are an **offline-stage fallback / build proof**; the BundledCatalog installs these
apps via `RootfsDepKind.APT` (online), so the tars are not shipped in the APK.

```
# rebuild any overlay (needs the `zstd` CLI on PATH for noble .zst debs):
python -c "import sys; sys.path.insert(0,'.'); from tools.deb_closure import build_minimal_overlay; \
  print(build_minimal_overlay(['viewnior'], 'app/src/main/assets/rootfs/payloads/tiny-rootfs.tar', \
  'out/overlays/viewnior-stage.tar', mirror='http://ports.ubuntu.com/ubuntu-ports', suite='noble', \
  arch='arm64', components=('main','universe'))['violations'])"
```
