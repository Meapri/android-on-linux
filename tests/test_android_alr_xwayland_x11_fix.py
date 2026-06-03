"""Source-invariant guards for the ROOTFUL Xwayland X11-window fixes (WS-4 §5 M4).

Two device gaps blocked the X11 window even though the staged Xwayland binary runs:

  (1) /tmp/.X0-lock create EPERM → the X server aborts before binding its socket
      ("X0 socket never created"). Root cause: the guest /tmp maps to <rootfs>/tmp,
      but the base ships no .X11-unix dir and no 1777 /tmp a non-root euid can use,
      and Xwayland refuses -nolock for non-root. FIX: the app pre-creates
      <rootfs>/tmp AND <rootfs>/tmp/.X11-unix at mode 1777 (sticky) before launch.

  (2) the X11 AF_UNIX socket /tmp/.X11-unix/X0 is bound/connected with a sun_path
      that is NOT path-mediated (bind/connect are not path syscalls), so it hits the
      bare-Android /tmp (absent) and ENOENT-fails. FIX: the LD_PRELOAD interposer
      adds an X11 sun_path transform (alr_x11_sock_xform) that rewrites
      /tmp/.X11-unix/X<n> → <rootfs>/tmp/.X11-unix/X<n> in BOTH bind() and connect(),
      so server and client meet on the same rootfs socket node.

These run on the host (the .so is aarch64-linux and cannot be dlopened on the build
mac), so they assert the wiring is present and correctly shaped — a structural guard
against the fix being reverted/loosened. The on-device behavior is the integration
gate (see the device-test plan in the M4 deliverable).
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
INTERPOSE = ROOT / "app/src/main/cpp/alr_interpose/libalr_interpose.c"


# --------------------------------------------------------------------------- #
# (1) app-side /tmp + /tmp/.X11-unix 1777 prep before the Xwayland launch
# --------------------------------------------------------------------------- #

def test_main_activity_preps_tmp_and_x11_unix_sticky_1777_before_launch():
    text = MAIN.read_text()
    # the marker-gated Xwayland block exists
    assert '/data/local/tmp/.alr-xwayland' in text
    # both dirs are created…
    assert 'java.io.File(rootfsDirX, "tmp")' in text
    assert '.X11-unix' in text
    # …and chmod'd to the sticky 1777 the X server expects (0x3FF == 01777).
    # Os.chmod (not File.set*) because Java cannot set the sticky bit.
    assert "android.system.Os.chmod" in text
    assert "0x3FF" in text  # 01777
    # the prep is keyed to the X server expectations, not an arbitrary mode
    assert "1777" in text


def test_main_activity_readiness_uses_exists_not_isfile_for_the_socket():
    """A bound AF_UNIX socket is a special file → File.isFile() is False for it.
    The readiness wait MUST use exists() or it would spin the full timeout even
    when the server is up."""
    text = MAIN.read_text()
    # the wait loop condition is on exists(), not isFile(), for the X0 socket
    assert "!xSock.exists()" in text
    assert "xSock.isFile" not in text  # the old, wrong probe must be gone
    # the lock file is also surfaced as an auxiliary signal
    assert 'java.io.File(rootfsDirX, "tmp/.X0-lock")' in text


# --------------------------------------------------------------------------- #
# (2) interposer X11 sun_path transform, wired into bind() AND connect()
# --------------------------------------------------------------------------- #

def test_interposer_declares_the_x11_socket_transform():
    text = INTERPOSE.read_text()
    assert "alr_x11_sock_xform" in text
    # it matches the hardcoded X socket dir prefix…
    assert '"/tmp/.X11-unix/"' in text
    # …rewrites to the rootfs prefix (reuses g_rootfs like rw())…
    assert "g_rootfs" in text
    # …and is disabled when the interposer is (rootfs unset) — fail-safe.
    assert "g_rootfs_len == 0" in text


def test_interposer_x11_transform_is_used_by_both_bind_and_connect():
    text = INTERPOSE.read_text()
    body = text
    # locate the bind() and connect() function bodies and assert each calls the xform
    bind_at = body.index("int bind(int fd, const struct sockaddr *addr, socklen_t len)")
    connect_at = body.index("int connect(int fd, const struct sockaddr *addr, socklen_t len)")
    assert bind_at < connect_at  # bind defined before connect (sanity)
    bind_body = body[bind_at:connect_at]
    connect_body = body[connect_at:connect_at + 1200]
    assert "alr_x11_sock_xform(addr, len" in bind_body, "bind() must apply the X11 xform"
    assert "alr_x11_sock_xform(addr, len" in connect_body, "connect() must apply the X11 xform"


def test_interposer_x11_transform_respects_sun_path_length_and_abstract():
    """The transform must (a) skip abstract sockets (leading NUL), and (b) fail
    safe (pass the literal through) when <rootfs> + path would overflow sun_path —
    never write past the 108-byte sun_path buffer."""
    text = INTERPOSE.read_text()
    xform_at = text.index("static int alr_x11_sock_xform(")
    end = text.index("\nint bind(", xform_at)
    body = text[xform_at:end]
    # abstract sockets (sun_path[0]==NUL) are left untouched
    assert "sun_path[0] == '\\0'" in body
    # an overflow check before copying into out->sun_path
    assert "sizeof(out->sun_path)" in body
    assert "return 0;" in body  # the passthrough/fail-safe exits


# --------------------------------------------------------------------------- #
# (cross-check) the launch still pins DISPLAY=:0 and the rootful argv
# --------------------------------------------------------------------------- #

def test_xwayland_launch_argv_and_display_unchanged():
    text = MAIN.read_text()
    # the rootful argv the run path execs (sized to the device panel)
    assert "/usr/bin/Xwayland\\n:0\\n-shm\\n-geometry\\n${outW}x${outH}" in text


# --------------------------------------------------------------------------- #
# (3) X0-lock hard-link fallback — Android /data forbids hard links
# --------------------------------------------------------------------------- #
# DEVICE-PROVEN root cause: the ALR rootfs lives on Android /data, whose filesystem
# REFUSES hard links — link()/linkat() return EPERM even for the app's own uid in its
# own dir (`ln a b` -> "Permission denied"; rename()/symlink() work). The X server's
# LockServer() create()s /tmp/.Xnn-tmp and link()s it onto /tmp/.X0-lock to atomically
# claim the display; the link EPERMs -> "Fatal server error: Linking lock file
# (/tmp/.X0-lock) in place failed: Permission denied" -> Xwayland aborts before binding
# the X socket. FIX: the interposer's link/linkat wrappers fall back to an atomic
# rename() of the same (rewritten) paths on EPERM/EACCES.

def test_interposer_link_falls_back_to_rename_on_eperm():
    text = INTERPOSE.read_text()
    # a shared link emit helper that retries via rename on the hard-link-forbidden errno
    assert "alr_link_emit" in text
    helper_at = text.index("static int alr_link_emit(")
    helper = text[helper_at: text.index("\nint link(", helper_at)]
    # it tries the real link, then on EPERM/EACCES retries the real rename
    assert 'ALR_REAL(real_link' in helper
    assert "errno == EPERM || errno == EACCES" in helper
    assert 'ALR_REAL(real_rename' in helper
    # link() routes through the helper (not a bare real() call anymore)
    link_at = text.index("int link(const char *oldp, const char *newp)")
    link_body = text[link_at: text.index("int linkat(", link_at)]
    assert "alr_link_emit(rw(oldp" in link_body


def test_interposer_linkat_falls_back_to_renameat_on_eperm():
    text = INTERPOSE.read_text()
    linkat_at = text.index("int linkat(int oldfd, const char *oldp, int newfd, const char *newp, int flags)")
    body = text[linkat_at: linkat_at + 1400]
    # same EPERM/EACCES fallback, but via renameat and ONLY for the AT_FDCWD lock pattern
    assert "errno == EPERM || errno == EACCES" in body
    assert "oldfd == AT_FDCWD && newfd == AT_FDCWD" in body
    assert 'ALR_REAL(real_renameat' in body


def test_interposer_link_fallback_preserves_genuine_errors():
    """The fallback must ONLY trigger on the hard-link-forbidden errnos so a real
    link error (EEXIST = lock already held, ENOENT = missing source) is still
    reported truthfully and rename is not wrongly attempted."""
    text = INTERPOSE.read_text()
    helper_at = text.index("static int alr_link_emit(")
    helper = text[helper_at: text.index("\nint link(", helper_at)]
    # the rename retry is GUARDED by the errno check (not unconditional)
    assert helper.index("EPERM") < helper.index("real_rename")
    # if rename also fails, the ORIGINAL link errno is restored (saved/restored)
    assert "int saved = errno;" in helper
    assert "errno = saved;" in helper


# --------------------------------------------------------------------------- #
# (4) xkbcomp ships with Xwayland — the next blocker after the lock fix
# --------------------------------------------------------------------------- #
# With the X0-lock fixed, a ROOTFUL Xwayland next exec()s `xkbcomp` to compile its
# virtual-core keyboard keymap; the base lacks it -> "XKB: Failed to compile keymap" /
# "Failed to activate virtual core keyboard: 2". xkbcomp is exec'd (NOT a DT_NEEDED of
# Xwayland), so the DT_NEEDED-minimal flattener won't pull it — it must be a named leaf
# package (x11-xkb-utils) in the SAME xwayland overlay.

def test_xwayland_overlay_bundles_xkbcomp_for_the_keymap_compile():
    from tools.build_xwayland_overlay import X_OVERLAYS
    xw = X_OVERLAYS["xwayland"]
    assert "x11-xkb-utils" in xw.leaf_packages
    assert "/usr/bin/xkbcomp" in xw.expect_files
