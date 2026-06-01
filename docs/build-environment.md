# Build Environment Notes

Current VPS result: Android debug APK builds successfully on this aarch64 Oracle VPS using qemu-user-static for x86_64 Android SDK tools.

## Verified installed stack

- Host architecture: `aarch64`
- Java/JDK: OpenJDK 17
- Gradle: project Gradle wrapper 8.10.2
- Android SDK: `~/Android/Sdk`
- Android platform: 35
- Android build-tools: 35.0.0 plus AGP-installed 34.0.0 as needed
- Android NDK: 27.2.12479018
- CMake: system `/usr/bin/cmake` on aarch64 host
- Ninja: system `/usr/bin/ninja`
- qemu-user-static/binfmt: used to run x86_64 SDK tools such as AAPT2 and NDK clang
- x86_64 runtime shims: `libc6-amd64-cross`, `libstdc++6-amd64-cross`, and copied amd64 `libz.so.1`

## Bootstrap command

```bash
./scripts/bootstrap-android-build-host.sh
```

On aarch64/arm64, the script installs qemu/binfmt and x86_64 runtime shims, writes:

```properties
sdk.dir=/home/ubuntu/Android/Sdk
cmake.dir=/usr
```

to `local.properties`, then runs:

```bash
./gradlew --no-daemon :app:assembleDebug
```

## Verification gates

APK build success is only valid when this command succeeds:

```bash
./gradlew --no-daemon :app:assembleDebug
```

Current verified APK:

```text
app/build/outputs/apk/debug/app-debug.apk
```

Do not claim device runtime success until the debug APK is installed and `MainActivity` displays the native runtime report on an Android device or emulator.
