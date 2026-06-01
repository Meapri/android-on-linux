# Device Evidence — SM-X236N, v95: in-process image decode works; GIMP 3.0 is provisioned and loading

Two results on the path to GIMP: **gdk-pixbuf decodes images (PNG/JPEG/BMP/GIF) in-process** (the prerequisite I feared was a deep loader bug — it was a missing MIME database), and **GIMP 3.0.2 is fully provisioned** (112-lib closure) and its binary loads in-process to a single, precise library-version mismatch (now being fixed).

Device SM-X236N (Android 16 / API 36, untrusted_app). APK `0.4.95-android-gimp-stderr-v95`, SHA-256 `9beaed9058a6f3c1af5ca52919f2aa54c2bee20a1e2d6b2b58cde0cdf9c46c1c`. pytest 235 passed, native-core PASS, validate-host PASS.

## Result

```
ALR IN-PROCESS IMAGE DECODE (gdk-pixbuf PNG, GIMP prerequisite):  PASS
ALR GIMP 3.0 LOADS (gimp-console-3.0 --version, 112-lib closure):  FAIL (precise: a GLib symbol)

alr-png (in-process gdk-pixbuf):
  mime.cache present=YES
  g_content_type_guess(PNG bytes) = image/png  uncertain=0
  png decode OK 2x2 / jpeg decode OK / bmp decode OK / gif decode OK
  formats=png,jpeg,ani,bmp,gif,…,svg

gimp-console-3.0 --version (stderr now captured):
  /usr/bin/gimp-console-3.0: symbol lookup error:
  undefined symbol: g_variant_builder_init_static       child exit=127
```

(GTK3 window, image decode, first-window, etc. still PASS — no regression from swapping in the 231 MB GIMP rootfs.)

## 1. In-process image decode — solved

The earlier "Unrecognized image file format" was **not** a broken builtin loader or in-process relocation issue. The rootfs `libgdk_pixbuf` is built with `GDK_PIXBUF_USE_GIO_MIME`, so it picks a decoder by **MIME type** via `g_content_type_guess()` → **xdgmime** → the **shared-mime-info database** at `$XDG_DATA_DIRS/mime`. That database was **absent** from the rootfs (the `update-mime-database` postinst trigger never ran), so every image sniffed as `application/octet-stream` → no decoder. Shipping a compiled `/usr/share/mime/mime.cache` fixes **all** formats at once. PNG/JPEG/BMP/GIF now decode in-process — GIMP's image-load prerequisite is met. (Also fixed: the loader now captures the guest's **stderr**, and the Gradle build stores the 200 MB+ rootfs tar uncompressed via `noCompress` to avoid an asset-compression OOM.)

## 2. GIMP 3.0 provisioned; loads to a version mismatch

GIMP **3.0.2** (GTK3, from Ubuntu plucky — noble only has the GTK2-based 2.10) is staged with its full transitive ELF closure: **112 libraries added, 0 missing**, worst overlay symver GLIBC_2.38 (≤ the rootfs's 2.39), plus GIMP's data, 124 plug-ins, 67 GEGL/babl ops. `gimp-console-3.0` (ET_DYN PIE) maps and jumps to the interpreter in-process; `ld.so` resolves the closure and then hits **one precise symbol mismatch**: `g_variant_builder_init_static` (added in **GLib 2.84**, which plucky's GIMP needs) is absent from the rootfs's **noble GLib (~2.80)**. So the base GLib must be upgraded to plucky to match plucky GIMP — a version reconciliation, **not** a fundamental blocker. Fix in progress (overlay plucky GLib + iterate any further symbol gaps).

## Position / next

- **Image decode: done** (GIMP can load images/icons).
- **GIMP loading: one version-reconciliation away** (GLib noble→plucky). After that, `gimp-console-3.0 --version` should print the version + exit 0 (the 112-lib closure runs in-process).
- Then the known GIMP blockers remain: **exec-re-entry** for the 124 plug-in child processes (File I/O), **multi-surface compositing** for GIMP's docks/menus/dialogs, and startup heaviness. GIMP is explicitly non-fatal on plug-in failures and reaches a paint canvas, so a GUI is plausible before those are complete.
