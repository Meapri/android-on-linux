# alr — build entry points.
#
#   make test        host-side core tests (macOS or Linux, no device needed)
#                    NOTE: CI runs this leg on gcc too, and gcc warns where
#                    clang does not (-Wformat-truncation, -Wuninitialized).
#                    Reproduce that leg locally with:  make test CC=gcc-16
#   make preload     guest-side libalr_preload.so under the pinned zig
#   make alr         cross-build the alr binary with the NDK
#   make doctor      cross-build alr-doctor with the NDK
#   make check       test + the device-free preload release gates
#   make install     lay the release layout down under $(DESTDIR)$(PREFIX)
#   make release     build the release tarball (scripts/make-release.sh)
#   make device-test push + build + run the host tests ON the device
#
# Two ABI sides (docs/01-platform-facts.md §F1/§F2):
#   host  (bionic) -> NDK 29, --target=aarch64-linux-android24
#   guest (glibc)  -> zig cc, --target=aarch64-linux-gnu.2.17
# Termux's own clang is an on-device inner loop only, never a release path.

CC        ?= cc
CFLAGS    ?= -O1 -Wall -Wextra -Werror -std=c11
LDFLAGS   ?=
COMMON    := src/common
BUILD     := build

# Termux's own prefix, not /usr/local: src/cli/alr.c falls back to exactly this
# string when $PREFIX is unset and then looks for the interposer at
# $(PREFIX)/share/alr/libalr_preload.so.  Installing anywhere else means the
# guest boots with no path virtualization and looks like a corrupt rootfs.
PREFIX    ?= /data/data/com.termux/files/usr
DESTDIR   ?=

# The source list is exact, and identical to scripts/make-release.sh's.
# alr_supervisor.c belongs to this binary rather than to a separate program:
# the supervisor IS the parent process (ADR 0001), so there is nothing to link
# apart and nothing extra to install.
ALR_SRC := src/cli/alr.c src/cli/alr_resolvd.c \
           $(COMMON)/alr_exec_rule.c $(COMMON)/alr_elf.c \
           src/supervisor/alr_supervisor.c
ALR_INC := -I$(COMMON) -Isrc/supervisor

# --- pinned toolchains -----------------------------------------------------
NDK             ?= $(ANDROID_NDK_HOME)
NDK_API         := 24
NDK_TRIPLE      := aarch64-linux-android$(NDK_API)
ZIG             ?= zig
# NOTE: the guest target lives in scripts/build-preload.sh (TARGET) and is
# deliberately NOT duplicated here -- a second copy is how pins drift.
ZIG_VERSION_REQ := 0.16.0

# One definition of the bionic-side compiler for both `alr` and `alr-doctor`.
# They ship in the same tarball, and a flag that drifts between them (the page
# size below above all) is invisible until one of the two refuses to load on a
# 16K-page device.  The '*' is expanded by the shell at recipe time
# (darwin-x86_64 / linux-x86_64 / ...), which is why it is not $(wildcard).
#
# Overridable as a whole: `make alr HOST_CC=clang` is the on-device Termux
# inner loop, which needs no NDK and no --target.  Dev only, never a release.
NDK_CLANG   ?= $(NDK)/toolchains/llvm/prebuilt/*/bin/clang
HOST_CC     ?= $(NDK_CLANG) --target=$(NDK_TRIPLE)

# Byte-for-byte the flag string scripts/make-release.sh uses, and for its
# reasons -- if `make alr` and the release build disagree, the binary anyone
# debugs is not the binary anyone ships.  Two differences from $(CFLAGS) are
# deliberate:
#   -O1, not -O2   the only alr binaries ever run under the zygote seccomp
#                  filter were built at -O1 (dev-push.sh CFLAGS_DEV).
#   no -Werror     whether these sources are warning-clean under NDK headers is
#                  UNVERIFIED; warnings still print, they just are not fatal.
HOST_CFLAGS ?= -O1 -Wall -Wextra -std=c11 -D_GNU_SOURCE $(ALR_INC)

# Appends rather than replaces $(LDFLAGS): the two page-size flags are not a
# preference.  Android 15+ ships 16K pages and the loader rejects a binary
# linked for 4K, so a packager passing their own LDFLAGS must not be able to
# drop them by accident.
HOST_LDFLAGS := -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 $(LDFLAGS)

# An explicit `make alr CC=... CFLAGS=...` still wins: a packager who types it
# means it.  $(origin) rather than plain $(CC)/$(CFLAGS) because those two are
# already set in this file for `make test`, where the host-native cc and
# -Werror are the right answers and here they are not.
#
# Command line only, NOT the environment.  An exported CC is usually the CI's
# host compiler and would silently turn the cross build native; an exported
# CFLAGS would just as silently move the optimisation level away from the -O1
# every device-verified binary was built at.  Both are reachable on purpose,
# neither by accident.
ifeq "$(origin CC)" "command line"
HOST_CC := $(CC)
endif
ifeq "$(origin CFLAGS)" "command line"
HOST_CFLAGS := $(CFLAGS) -D_GNU_SOURCE $(ALR_INC)
endif

# `doctor` has always refused to run with an empty $(NDK) rather than let the
# glob expand to a bare path and report "clang: not found" three screens later.
# Skipped when HOST_CC has been pointed elsewhere -- that is the on-device loop.
ifeq ($(strip $(NDK)),)
ifneq ($(findstring $(NDK_CLANG),$(HOST_CC)),)
NDK_ERR := set NDK=/path/to/android-ndk (r29+), or build on-device with HOST_CC=clang
endif
endif

.PHONY: all test doctor alr preload check check-docs install release clean dist-clean \
        device-test check-zig

all: test

# --- host-side core tests (no device, no cross toolchain) ------------------
$(BUILD):
	@mkdir -p $(BUILD)

test: $(BUILD)
	@$(CC) $(CFLAGS) -I$(COMMON) -o $(BUILD)/test_path \
	    tests/host/test_path.c
	@$(CC) $(CFLAGS) -I$(COMMON) -o $(BUILD)/test_exec_rule \
	    tests/host/test_exec_rule.c $(COMMON)/alr_elf.c $(COMMON)/alr_exec_rule.c
	@echo "── host core tests ───────────────────────────────────────────"
	@$(BUILD)/test_path tests/cases/paths.tsv
	@$(BUILD)/test_exec_rule
	@echo "──────────────────────────────────────────────────────────────"

# --- alr -------------------------------------------------------------------
alr: $(BUILD)
ifdef NDK_ERR
	$(error $(NDK_ERR))
endif
	$(HOST_CC) $(HOST_CFLAGS) $(HOST_LDFLAGS) -o $(BUILD)/alr $(ALR_SRC)

# --- alr-doctor ------------------------------------------------------------
# Cross-built for release; see scripts/dev-bootstrap.md for the on-device path.
doctor: $(BUILD)
ifdef NDK_ERR
	$(error $(NDK_ERR))
endif
	$(HOST_CC) $(HOST_CFLAGS) $(HOST_LDFLAGS) \
	    -o $(BUILD)/alr-doctor src/cli/doctor.c

# --- guest-side .so --------------------------------------------------------
# The 2.17 floor is deliberate: it makes the linker REFUSE any accidental call
# into the stat family (GLIBC_2.33+), which the interposer must only DEFINE.
preload:
	@ZIG=$(ZIG) scripts/build-preload.sh

check-zig:
	@v=$$($(ZIG) version 2>/dev/null); \
	if [ "$$v" != "$(ZIG_VERSION_REQ)" ]; then \
	  echo "zig $(ZIG_VERSION_REQ) required (found '$$v')."; \
	  echo "Reproducibility only holds for an exact pin: a zig upgrade changes"; \
	  echo "bundled compiler-rt and LLVM codegen."; exit 1; fi

# --- device-free release gates ---------------------------------------------
# Everything a hosted runner can actually decide.  It cannot decide anything in
# tests/device/: those refuse to run unless uid>=10000 and Seccomp==2, because
# the app seccomp filter is installed by the zygote and nowhere else.
check: test
	@ZIG=$(ZIG) bash scripts/check-preload.sh
	@bash scripts/check-docs.sh
	@bash scripts/check-reasons.sh
	@bash scripts/check-invariants.sh
	@bash scripts/check-acceptance-names.sh
	@# The path-coverage gate reads the built .so's symbol table, so build it
	@# rather than assuming a previous command left one behind.  It did on the
	@# author's machine and did not in the release workflow -- check-preload.sh
	@# builds into a scratch dir on purpose, so `build/` is absent there, and
	@# `make check` failed on the release tag having passed locally all day.
	@ZIG=$(ZIG) scripts/build-preload.sh >/dev/null
	@bash scripts/check-path-coverage.sh
	@bench/regression_gate.py --self-test

# --- release layout --------------------------------------------------------
# The same bin/ and share/ that alr-<version>-aarch64.tar.gz carries at its top
# level, with the same modes, so `tar -xzf ... -C $PREFIX` and `make install`
# leave the same installed tree.  (The tarball also carries LICENSE and
# README.md at its root; those are archive documentation, not installed files.)
#
# libalr_preload.manifest.json is renamed to manifest.json on purpose:
# share/alr/ holds exactly one .so, so a name that repeats the .so's is noise,
# and `alr version` recomputes that sha256 from the installed file -- the two
# must agree or the manifest is worse than absent.
install:
	@for f in $(BUILD)/alr $(BUILD)/alr-doctor $(BUILD)/libalr_preload.so \
	          $(BUILD)/libalr_preload.manifest.json; do \
	    [ -f "$$f" ] || { echo "missing $$f -- run 'make alr doctor preload' first" >&2; \
	                      exit 1; }; \
	done
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/alr
	install -m 755 $(BUILD)/alr        $(DESTDIR)$(PREFIX)/bin/alr
	install -m 755 $(BUILD)/alr-doctor $(DESTDIR)$(PREFIX)/bin/alr-doctor
	install -m 755 $(BUILD)/libalr_preload.so \
	    $(DESTDIR)$(PREFIX)/share/alr/libalr_preload.so
	install -m 644 $(BUILD)/libalr_preload.manifest.json \
	    $(DESTDIR)$(PREFIX)/share/alr/manifest.json

# --- release tarball -------------------------------------------------------
# Version and layout live in the script, not here; ALR_VERSION in
# src/common/alr_version.h is the single source of truth for both.
release: scripts/make-release.sh
	@bash scripts/make-release.sh

# --- on-device verification of the same core tests -------------------------
# The path rule must behave identically on bionic/aarch64 and on the dev host.
device-test:
	@scripts/dev-push.sh test

clean:
	@rm -rf $(BUILD)

dist-clean: clean
	@rm -rf dist
	@rm -f alr-*-aarch64.tar.gz

# Cross-references are how this repo carries its argument -- a claim points at
# the evidence file that measured it.  A broken link reads as a citation, so it
# gets a gate rather than a reviewer's attention.
check-docs:
	@bash scripts/check-docs.sh
