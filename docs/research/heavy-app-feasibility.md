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

## 3. Validation (model vs device ground truth)

`tools/app_closure_audit.py --live` against the real noble index + Contents, scored
against project-memory device results:

| app | device truth | model | trigger buckets |
|-----|:------------:|:-----:|-----------------|
| galculator | PASS | LIKELY-PASS | — |
| l3afpad | PASS | LIKELY-PASS | — |
| htop | PASS | LIKELY-PASS | — |
| gimp | PASS | LIKELY-PASS | — |
| foot | PASS | LIKELY-PASS | — |
| netsurf-gtk | PASS | LIKELY-PASS | — |
| mousepad | FAIL | HEAVY | perl |
| gnome-calculator | FAIL | HEAVY | gnome |
| gedit | FAIL | HEAVY | perl, gnome |
| eog | FAIL | HEAVY | gnome, sandbox |

**10/10 match.** (`tests/test_app_closure_audit.py::test_live_audit_matches_device_ground_truth`,
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

**(B) Reachable ONLY with a session-dbus / dconf shim (candidate future work):**
- The **gnome-platform** bucket (gnome-calculator, gnome-text-editor, eog, file-roller,
  gnome-terminal) fails at *configure* on `gsettings-desktop-schemas` / `appstream` /
  `session-migration` / `dconf-service` postinsts. These do NOT need a real init — they
  need (i) the gsettings schemas compiled into `glib-2.0/schemas/gschemas.compiled`
  (the base already does this for its own set; the shim must re-run `glib-compile-schemas`
  after the overlay lands), (ii) a **dbus session bus** for dconf to talk to at *runtime*
  (a `dbus-launch`/`dbus-daemon --session` started by the launcher before the guest, with
  `DBUS_SESSION_BUS_ADDRESS` exported), and (iii) suppressing or no-op-ing the appstream
  pool refresh + session-migration triggers (e.g. `policy-rc.d` returning 101, or pre-
  seeding the dpkg DB so those postinsts are skipped).
  - **What the shim would unlock:** the gnome-platform-only apps (no perl, no sandbox, no
    gstreamer) — i.e. **gnome-calculator, gnome-text-editor, eog, file-roller,
    gnome-terminal** (the last still needs the PTY host). These are the realistic GNOME
    wins IF a session-bus + schema-compile + skip-triggers shim is built.
  - **What it does NOT unlock:** evince/atril/nautilus (still drag sandbox + gstreamer +
    perl on top of gnome-platform), and the perl bucket below.
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
   htop, gimp, foot, netsurf-gtk, gpicview, xarchiver, sakura, viewnior, xzgv, xpdf,
   qalculate-gtk, nsxiv. (See `BundledCatalog`.)
2. **Promote to "candidate, host-audited LIKELY-PASS" (configure-clean, run-pending):**
   geany, thunar, mate-calc, transmission-gtk. Light desktops (XFCE/MATE/LXDE) over GNOME.
3. **Do NOT promise GNOME apps** (gnome-calculator/gedit/eog/file-roller/nautilus) until a
   session-dbus + schema-compile + skip-triggers shim exists. When scoping that shim, the
   *gnome-platform-only* apps are the reachable target; evince/atril/nautilus are not
   (sandbox + gstreamer + perl on top).
4. **Structurally out:** printing/mDNS/polkit/accountsservice/packagekit GUIs and anything
   that wants systemd-as-PID1 or a real privilege model.

## 6. How to reproduce / extend

```
# offline logic + selftest
uvx pytest tests/test_app_closure_audit.py -q

# live audit of any package set against real noble (matches device ground truth)
python -m tools.app_closure_audit --live --base app/src/main/assets/rootfs/payloads/tiny-rootfs.tar \
    --package gnome-calculator --package mate-calc --package geany --json
```

The model is one function (`tools.app_closure_audit.classify`): resolve the noble
closure, subtract the base installed-set (`tools.build_dpkg_db.installed_packages` over the
noble `Contents` index), and report any `CASCADE_TRIGGERS` in the remaining delta.
