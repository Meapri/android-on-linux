# termux-packages recipe for `alr`.
#
# Copy this directory to termux-packages/packages/alr/ to build it there; it is
# kept in this repo so the recipe and the sources it compiles move together.
#
# ─────────────────────────────────────────────────────────────────────────────
# THE ONE THING TO UNDERSTAND BEFORE EDITING THIS FILE
#
# `alr` is two binaries and one shared library across TWO DIFFERENT libcs:
#
#   bin/alr, bin/alr-doctor      HOST  side, bionic. Built here, by the termux
#                                NDK toolchain, like any other termux package.
#   share/alr/libalr_preload.so  GUEST side, glibc. LD_PRELOADed into stock
#                                Ubuntu arm64 binaries running under alr.
#
# libalr_preload.so MUST NOT BE BUILT BY THIS RECIPE.  It is not a termux
# artifact at all -- it is a glibc 2.17 ELF whose whole job is to be loaded by
# the guest's /lib/ld-linux-aarch64.so.1 next to the guest's libc.so.6.  Built
# against bionic it would not merely be wrong, it would not load: the symbols it
# interposes (__xstat family, the __*_chk fortify wrappers, the versioned glibc
# symbols) do not exist in bionic, and the guest loader would reject it.
#
# It is produced by scripts/build-preload.sh with a pinned zig 0.16.0 targeting
# aarch64-linux-gnu.2.17.  That 2.17 floor is load-bearing, not cosmetic: it
# makes the linker REFUSE any accidental *call* into the stat family, which the
# interposer is only allowed to DEFINE (docs/04-preload-spec.md §1, R2).  There
# is no way to reproduce that constraint with the NDK toolchain.
#
# So this recipe DOWNLOADS the prebuilt .so and its manifest from the release
# tarball and compiles only the bionic side.  See termux_step_post_get_source()
# and termux_step_post_massage() below -- the latter exists because termux's own
# post-processing (strip, termux-elf-cleaner) would happily rewrite a foreign
# glibc ELF and produce something that fails to load only on the user's phone.
# ─────────────────────────────────────────────────────────────────────────────

TERMUX_PKG_HOMEPAGE=https://github.com/Meapri/alr-termux
TERMUX_PKG_DESCRIPTION="Android 16 only. Run stock Ubuntu 24.04 arm64 glibc programs in Termux without root"
TERMUX_PKG_LICENSE="MIT"
TERMUX_PKG_LICENSE_FILE="LICENSE"
TERMUX_PKG_MAINTAINER="@Meapri"

# Keep in lockstep with src/common/alr_version.h -- that header is the single
# source of truth and scripts/make-release.sh REFUSES to cut a release when this
# line disagrees with it.
TERMUX_PKG_VERSION="0.5.0"

TERMUX_PKG_SRCURL=https://github.com/Meapri/alr-termux/archive/refs/tags/v${TERMUX_PKG_VERSION}.tar.gz
# NOTE these two hashes are the only values in the release path with no
# automated cross-check: their URLs interpolate TERMUX_PKG_VERSION, so a
# version bump silently repoints them while the old hashes stay.  Bump them in
# the same commit, or set them back to PLACEHOLDER so the guard below fires.
TERMUX_PKG_SHA256=8dd67599bad98c60a8156d583fb162214b4eb8aa0fcfc871a3bbea0db947e623

# Runtime dependencies, derived from what src/cli/alr.c actually execs or pipes
# through popen() ON THE HOST.  These are not guesses:
#
#   curl        rootfs + node + codex downloads, and the SHA256SUMS fetch
#   tar         rootfs extraction (-xzf), node (-xJf), codex (-xzf)
#   dash        provides $PREFIX/bin/sh; every popen()/`sh -c` here goes to it
#   coreutils   mkdir, chmod, sha256sum, install, head, tr, cut, rm, mv
#   findutils   find -perm /6000  (masking setuid bits after extraction)
#   grep, sed   the nodejs.org / GitHub release-tag parsing pipelines
#   xz-utils    tar -xJf on the node tarball; GNU tar shells out to xz for .xz
#
# NOT dependencies: apt-get, dpkg-divert, dpkg. Those run INSIDE the guest
# rootfs, against the guest's own copies -- they are Ubuntu packages, not Termux
# ones, and adding them here would install a second unrelated apt on the host.
TERMUX_PKG_DEPENDS="coreutils, curl, dash, findutils, grep, sed, tar, xz-utils"

# There is exactly one prebuilt guest .so and it is aarch64 glibc.  On any other
# arch this package would install a host binary with nothing to preload into.
TERMUX_PKG_BLACKLISTED_ARCHES="arm, i686, x86_64"

TERMUX_PKG_BUILD_IN_SRC=true

# The version bump is not mechanical: a new release also needs a rebuilt guest
# .so and a new _PRELOAD_TARBALL_SHA256 below, and nothing in an auto-updater
# can tell whether those were regenerated.
TERMUX_PKG_AUTO_UPDATE=false

# Keep termux's post-processing off the guest .so.  These two names are
# UNVERIFIED against the current termux-packages tree -- if either is ignored,
# termux_step_post_massage() below still catches the damage, which is why the
# hash check there is the real guarantee and this is only the cheap first line.
TERMUX_PKG_NO_STRIP=true
TERMUX_PKG_NO_ELF_CLEANER=true

# Release tarball built by scripts/make-release.sh; layout is
#   bin/alr  bin/alr-doctor  share/alr/libalr_preload.so  share/alr/manifest.json
# We take only share/alr/ from it.  bin/ in that tarball is deliberately IGNORED:
# those were linked by the release NDK, and a termux package must build its own
# host binaries so the API level and hardening flags match the rest of the
# repository rather than whatever the release machine happened to have.
_PRELOAD_TARBALL_URL="https://github.com/Meapri/alr-termux/releases/download/v${TERMUX_PKG_VERSION}/alr-${TERMUX_PKG_VERSION}-aarch64.tar.gz"
# From the SHA256SUMS the release job publishes, verified by downloading the
# tarball and re-hashing it rather than trusting the paste.
#
# Both hashes are PLACEHOLDER between the version bump and the release, because
# neither can exist before the release job runs -- one is the hash of what that
# job builds, the other the hash of the source tarball GitHub generates for the
# tag.  _alr_refuse_placeholders() above covers that window: it fails loudly at
# the top of a termux build rather than 200 lines in with a confusing 404.
# Carrying the previous release's hashes over instead points both URLs at the
# new version while the hashes still describe the old one, which fails as a
# checksum mismatch nobody can interpret.
_PRELOAD_TARBALL_SHA256="056754e882365e613be24a777bf41c62fea9f4b79cf21eb6503d6933a9669804"

# Fail at the top rather than 200 lines into a build with a confusing 404.
_alr_refuse_placeholders() {
	local v
	for v in "$TERMUX_PKG_HOMEPAGE" "$TERMUX_PKG_MAINTAINER" \
		 "$TERMUX_PKG_SRCURL" "$TERMUX_PKG_SHA256" \
		 "$_PRELOAD_TARBALL_URL" "$_PRELOAD_TARBALL_SHA256"; do
		case "$v" in
		*PLACEHOLDER*)
			termux_error_exit \
"alr recipe still has PLACEHOLDER values.  Before this package can build,
someone with release authority must fill in, in packaging/termux/alr/build.sh:
  TERMUX_PKG_HOMEPAGE          the canonical repository URL
  TERMUX_PKG_MAINTAINER        the @handle answering bug reports
  TERMUX_PKG_SRCURL / _SHA256  the tagged source tarball and its hash
  _PRELOAD_TARBALL_URL/_SHA256 the release tarball from scripts/make-release.sh
                               (its hash is in the SHA256SUMS that script emits)"
			;;
		esac
	done
}

# Run at recipe top level, not inside a step: TERMUX_PKG_SRCURL is itself a
# placeholder, so termux's own get_source would already have died on a DNS
# failure or 404 before any step-scoped guard could explain why.
_alr_refuse_placeholders

termux_step_post_get_source() {

	local tb="$TERMUX_PKG_CACHEDIR/alr-${TERMUX_PKG_VERSION}-aarch64.tar.gz"
	termux_download "$_PRELOAD_TARBALL_URL" "$tb" "$_PRELOAD_TARBALL_SHA256"

	# Extract into a side directory, never over the source tree: the release
	# tarball also carries bin/alr, and a prebuilt host binary sitting in the
	# build dir is exactly the thing that gets installed by accident later.
	rm -rf "$TERMUX_PKG_SRCDIR/guest-prebuilt"
	mkdir -p "$TERMUX_PKG_SRCDIR/guest-prebuilt"
	tar -xzf "$tb" -C "$TERMUX_PKG_SRCDIR/guest-prebuilt" \
		share/alr/libalr_preload.so share/alr/manifest.json

	local so="$TERMUX_PKG_SRCDIR/guest-prebuilt/share/alr/libalr_preload.so"
	local mf="$TERMUX_PKG_SRCDIR/guest-prebuilt/share/alr/manifest.json"
	[ -f "$so" ] || termux_error_exit "release tarball has no share/alr/libalr_preload.so"
	[ -f "$mf" ] || termux_error_exit "release tarball has no share/alr/manifest.json"

	# The manifest records the hash of the .so it was built beside.  Check it
	# here so a mismatched pair is caught at unpack time and not by a user
	# reading `alr version` on a phone and wondering which build they have.
	local want got
	want=$(sed -n 's/.*"output_sha256"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$mf")
	got=$(sha256sum "$so" | cut -d' ' -f1)
	if [ "$want" != "$got" ]; then
		termux_error_exit \
"libalr_preload.so does not match its manifest.json
  manifest output_sha256: ${want:-<absent>}
  actual:                 $got
The release tarball is internally inconsistent; do not package it."
	fi
}

termux_step_make() {
	# docs/09 AUTHORITATIVE FACTS: `alr` is exactly these five translation
	# units.  Do not switch this to a glob -- src/preload/ and src/fakeroot/
	# are guest-side and must never be pulled into a bionic link.
	#
	# _FORTIFY_SOURCE, stack protector and the rest of termux's default
	# hardening are all WELCOME here.  The prohibition on fortify in this
	# project applies only to libalr_preload.so, which DEFINES the __*_chk
	# symbols and would recurse into itself; nothing on this side does.
	# -O1 is pinned AFTER $CFLAGS so it wins over termux's default (-Oz/-O2).
	# Every other build path in this repo pins -O1 (Makefile, dev-push.sh,
	# make-release.sh, ci.yml) because it is the only optimisation level ever
	# exercised under the zygote seccomp filter.  The termux package is the
	# main user-facing channel; shipping the one untested level from here
	# would be the least-observed divergence in the project.
	$CC $CPPFLAGS $CFLAGS $LDFLAGS -O1 \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra \
		-Isrc/common -Isrc/supervisor \
		-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 \
		-o alr \
		src/cli/alr.c \
		src/cli/alr_resolvd.c \
		src/common/alr_exec_rule.c \
		src/common/alr_elf.c \
		src/supervisor/alr_supervisor.c

	$CC $CPPFLAGS $CFLAGS $LDFLAGS \
		-std=c11 -D_GNU_SOURCE -Wall -Wextra \
		-Isrc/common \
		-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 \
		-o alr-doctor \
		src/cli/doctor.c
}

termux_step_make_install() {
	install -Dm755 alr        "$TERMUX_PREFIX/bin/alr"
	install -Dm755 alr-doctor "$TERMUX_PREFIX/bin/alr-doctor"

	# src/cli/alr.c::install_preload() probes, in order: next to the binary,
	# then $PREFIX/share/alr/libalr_preload.so.  This is that second path, and
	# it is the one that must work for a packaged install.
	install -Dm755 guest-prebuilt/share/alr/libalr_preload.so \
		"$TERMUX_PREFIX/share/alr/libalr_preload.so"
	install -Dm644 guest-prebuilt/share/alr/manifest.json \
		"$TERMUX_PREFIX/share/alr/manifest.json"
}

termux_step_post_massage() {
	# termux_step_massage strips binaries and runs termux-elf-cleaner over
	# every ELF in the package.  Both are correct for bionic binaries and
	# WRONG for the guest .so: elf-cleaner rewrites dynamic-section entries
	# that bionic's loader does not understand, but the guest .so is loaded by
	# GLIBC's loader, which does understand them and needs them intact.  A
	# corrupted .so does not fail here -- it fails as "no path virtualization"
	# on someone's phone, which reads like a broken rootfs.
	#
	# Restore the pristine copy and prove it byte-for-byte.  This is the real
	# guard; TERMUX_PKG_NO_STRIP / TERMUX_PKG_NO_ELF_CLEANER above are only a
	# hint that may or may not be honoured by the version of termux-packages
	# this is built under.
	local pristine="$TERMUX_PKG_SRCDIR/guest-prebuilt/share/alr/libalr_preload.so"

	# By this step the payload has been moved out of $TERMUX_PREFIX into the
	# massage directory, so $TERMUX_PREFIX/... would point at the BUILDER's
	# live prefix -- writing there would silently do nothing useful.  The exact
	# variable name and the cwd of this step are UNVERIFIED against the current
	# termux-packages tree, so probe both spellings and refuse if neither hits
	# rather than "succeeding" against a path that was never packaged.
	local c shipped=""
	for c in "${TERMUX_PKG_MASSAGEDIR:-}${TERMUX_PREFIX}/share/alr/libalr_preload.so" \
		 "./${TERMUX_PREFIX#/}/share/alr/libalr_preload.so"; do
		if [ -f "$c" ]; then shipped="$c"; break; fi
	done
	[ -n "$shipped" ] || termux_error_exit \
		"post_massage: cannot locate the packaged libalr_preload.so; the
guest .so may have been stripped or elf-cleaned and this guard did not run."

	# Hash BEFORE restoring, or the comparison is tautological: installing the
	# pristine copy first makes the two sides equal by construction and the
	# guard can never fire.  Report what termux's strip/elf-cleaner did, then
	# put the guest artifact back.
	local a b
	a=$(sha256sum "$pristine" | cut -d' ' -f1)
	b=$(sha256sum "$shipped"  | cut -d' ' -f1)
	if [ "$a" != "$b" ]; then
		echo "alr: packaging altered the guest .so ($b), restoring $a" >&2
		install -Dm755 "$pristine" "$shipped"
	fi
	b=$(sha256sum "$shipped" | cut -d' ' -f1)
	[ "$a" = "$b" ] || termux_error_exit \
		"guest libalr_preload.so could not be restored ($a != $b)"
}
