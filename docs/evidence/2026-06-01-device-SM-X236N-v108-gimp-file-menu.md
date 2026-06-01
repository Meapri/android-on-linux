# Device Evidence — SM-X236N, v108: GIMP's File menu opens and renders (popup compositing + touch)

Touching GIMP's "File" menu on the Android SurfaceView opens GIMP's File dropdown, fully rendered as a composited xdg_popup over the main window. This proves the multi-surface compositor draws popups (menus) AND that touch reaches GIMP and drives real UI. Public Android APIs only, non-root.

Device SM-X236N, APK `0.4.108-android-gimp-multisurface-v108`.

## Verified

A tap at the screen's top-left (GIMP's "File" menu position on the main window) produced:
```
xdg_surface.get_popup id=27 278x454@0,0 parent_key=11      (File menu popup, child of main window key=11)
surface committed shm: 290x466 ... key=24 role=popup       (the menu's pixels)
surface committed shm: 1920x1199 ... key=11 role=toplevel  (main window underneath)
drain_input n=4 focus=... pointers=1 touches=1 keyboards=1 (input live)
```
Screenshot `v108-gimp-file-menu-open.png`: GIMP's File dropdown is drawn over the main editing window — every item visible and legible: New… (Ctrl+N), Create, Open… (Ctrl+O), Open as Layers… (Ctrl+Alt+O), Open Location…, Open Recent, Save… (Ctrl+S), Save As… (Shift+Ctrl+S), Save a Copy…, Revert, Export… (Ctrl+E), Export As… (Shift+Ctrl+E), Create Template…, Copy Image Location, Show in File Manager (Ctrl+Alt+F), Close View (Ctrl+W), Close All (Shift+Ctrl+W), Quit (Ctrl+Q) — with the toolbox/tool-options on the left and the brushes/layers dock on the right still composited.

## What this confirms
- **Popup (menu) compositing works**: the popup committed at 290×466 with `parent_key=11` is placed over its parent toplevel and drawn by the multi-surface present loop (popups are appended just above their parent in `present_composited`).
- **Touch opens real GIMP UI**: the tap was delivered (wl_pointer/wl_touch on the focused surface), GIMP's GTK opened the menu, and the compositor rendered it — an end-to-end interactive round trip.

## Honest gap (being fixed)
Tapping a menu ITEM (e.g. "New…") currently just dismisses the menu without activating it: injected input maps into the FOCUSED toplevel's coordinate space, not the popup's, so the tap lands at the wrong local coordinate inside GIMP. The fix (in progress) routes injected pointer/touch to the top-most mapped popup and maps the coordinate into the popup's composited rect, so menu items become clickable — then File→New (and drawing on the canvas) work end to end. Also: opening the menu let Android's system bars reappear momentarily (onWindowFocusChanged re-applies immersive).

No host regression: 236 host tests pass.
