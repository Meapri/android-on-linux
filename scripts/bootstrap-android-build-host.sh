#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_DIR="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
CMDLINE_TOOLS_ZIP_URL="https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip"
GRADLE_VERSION="8.10.2"
NDK_VERSION="27.2.12479018"
CMAKE_VERSION="3.22.1"

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "missing required command: $1" >&2
    exit 127
  }
}

setup_aarch64_x86_64_tool_emulation() {
  echo "Configuring aarch64 host to run x86_64 Android SDK tools through qemu-user-static..." >&2
  sudo apt-get update
  sudo apt-get install -y \
    qemu-user-static \
    binfmt-support \
    libc6-amd64-cross \
    libstdc++6-amd64-cross

  sudo mkdir -p /lib64 /lib
  if [[ ! -e /lib64/ld-linux-x86-64.so.2 ]]; then
    sudo ln -s /usr/x86_64-linux-gnu/lib64/ld-linux-x86-64.so.2 /lib64/ld-linux-x86-64.so.2
  fi
  if [[ ! -e /lib/x86_64-linux-gnu ]]; then
    sudo ln -s /usr/x86_64-linux-gnu/lib /lib/x86_64-linux-gnu
  fi

  if [[ ! -e /usr/x86_64-linux-gnu/lib/libz.so.1 ]]; then
    tmp_zlib="$(mktemp -d -t zlib-amd64.XXXXXX)"
    curl -L "https://archive.ubuntu.com/ubuntu/pool/main/z/zlib/zlib1g_1.3.dfsg-3.1ubuntu2.1_amd64.deb" -o "$tmp_zlib/zlib.deb"
    dpkg-deb -x "$tmp_zlib/zlib.deb" "$tmp_zlib/root"
    sudo cp -a "$(find "$tmp_zlib/root" -name 'libz.so.1' -print -quit)" /usr/x86_64-linux-gnu/lib/
    sudo cp -a "$(find "$tmp_zlib/root" -name 'libz.so.1.*' -print -quit)" /usr/x86_64-linux-gnu/lib/
    rm -rf "$tmp_zlib"
  fi
}

arch="$(uname -m)"
case "$arch" in
  x86_64)
    ;;
  aarch64|arm64)
    setup_aarch64_x86_64_tool_emulation
    ;;
  *)
    echo "Unsupported build-host architecture: $arch" >&2
    exit 64
    ;;
esac

need_cmd curl
need_cmd unzip
need_cmd java
need_cmd sudo

mkdir -p "$SDK_DIR/cmdline-tools"
if [[ ! -x "$SDK_DIR/cmdline-tools/latest/bin/sdkmanager" ]]; then
  tmp_zip="$(mktemp -t android-commandlinetools.XXXXXX.zip)"
  tmp_dir="$(mktemp -d -t android-commandlinetools.XXXXXX)"
  curl -L "$CMDLINE_TOOLS_ZIP_URL" -o "$tmp_zip"
  unzip -q "$tmp_zip" -d "$tmp_dir"
  rm -rf "$SDK_DIR/cmdline-tools/latest"
  mkdir -p "$SDK_DIR/cmdline-tools/latest"
  mv "$tmp_dir/cmdline-tools"/* "$SDK_DIR/cmdline-tools/latest/"
  rm -rf "$tmp_zip" "$tmp_dir"
fi

export ANDROID_SDK_ROOT="$SDK_DIR"
export ANDROID_HOME="$SDK_DIR"
export PATH="$SDK_DIR/cmdline-tools/latest/bin:$SDK_DIR/platform-tools:$PATH"

yes | sdkmanager --licenses >/dev/null || true
sdkmanager \
  "platform-tools" \
  "platforms;android-35" \
  "build-tools;35.0.0" \
  "ndk;$NDK_VERSION" \
  "cmake;$CMAKE_VERSION"

{
  echo "sdk.dir=$SDK_DIR"
  if [[ "$arch" == "aarch64" || "$arch" == "arm64" ]]; then
    echo "cmake.dir=/usr"
  fi
} > "$ROOT/local.properties"

if [[ ! -x "$ROOT/gradlew" ]]; then
  gradle_tmp="$(mktemp -d -t gradle-bootstrap.XXXXXX)"
  curl -L "https://services.gradle.org/distributions/gradle-${GRADLE_VERSION}-bin.zip" -o "$gradle_tmp/gradle.zip"
  unzip -q "$gradle_tmp/gradle.zip" -d "$gradle_tmp"
  "$gradle_tmp/gradle-${GRADLE_VERSION}/bin/gradle" --no-daemon -p "$ROOT" wrapper --gradle-version "$GRADLE_VERSION"
  rm -rf "$gradle_tmp"
fi

"$ROOT/gradlew" --no-daemon -p "$ROOT" :app:assembleDebug
