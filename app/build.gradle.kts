plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "dev.chanwoo.androlinux"
    compileSdk = 35

    // The bundled rootfs tar is large (200+ MB once it carries a full GTK3/GIMP
    // closure). Store it uncompressed in the APK: it is mostly already-compressed
    // ELF/data, and APK-time compression of it OOMs Gradle's asset task.
    androidResources {
        noCompress += "tar"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        // Kotlin 2.0: the Compose compiler ships with Kotlin and is enabled by the
        // org.jetbrains.kotlin.plugin.compose plugin (no composeOptions block).
        compose = true
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
            // Termux prebuilts carry Android/Bionic GNU version metadata.
            // The Android Gradle strip task can corrupt/remove the loadable dynstr data,
            // making the device linker read garbage NEEDED names such as "h_file"/"nk".
            keepDebugSymbols += listOf(
                "**/libalr_proot.so",
                "**/libtalloc.so",
                "**/libproot-loader.so",
            )
        }
    }

    defaultConfig {
        applicationId = "dev.chanwoo.androlinux"
        minSdk = 26
        // 28, not 35, and this is the single most consequential line in the project.
        //
        // The SELinux domain an app runs in is chosen by targetSdk, and only the
        // legacy domains may execute a file in app-private storage.  From the AOSP
        // policy source (system/sepolicy/private/untrusted_app_27.te):
        //     allow untrusted_app_27 app_data_file:file execute_no_trans;
        // No such rule exists for untrusted_app_29/30/32 or untrusted_app.
        //
        // Running a stock Ubuntu glibc rootfs means execve()ing a downloaded
        // ld.so.  At targetSdk >= 29 that is denied and the only ways left are a
        // userspace ELF loader (bionic/glibc TLS coexistence -- a research
        // project) or PRoot's ptrace emulation (measured here: cannot even
        // `dpkg -i`).  At 28 the kernel just runs it.
        //
        // The cost is Google Play, which requires a recent targetSdk.  That was
        // already given up deliberately; distribution is sideload/F-Droid, the
        // same trade Termux makes.  ALR DIRECT APP-DATA EXECVE in the device
        // report is what proves this line is doing its job.
        targetSdk = 28
        versionCode = 163
        versionName = "0.4.163-sd-v163"
        ndkVersion = "27.2.12479018"

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20", "-Wall", "-Wextra", "-Werror")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    sourceSets.getByName("main") {
        jniLibs.srcDir(layout.buildDirectory.dir("generated/native-test-command/jniLibs"))
        // alr's CLI (libalr.so) and its guest interposer (an asset, because it
        // is a glibc object bionic must never try to load). See buildAlrRuntime.
        jniLibs.srcDir(layout.buildDirectory.dir("generated/alr/jniLibs"))
        assets.srcDir(layout.buildDirectory.dir("generated/alr/assets"))
    }
}

tasks.register("packageNativeTestCommand") {
    val generatedDir = layout.buildDirectory.dir("generated/native-test-command/jniLibs")
    outputs.dir(generatedDir)
    outputs.upToDateWhen { false }
    doLast {
        val abis = listOf("arm64-v8a", "armeabi-v7a", "x86", "x86_64")
        abis.forEach { abi ->
            val builtTestCommand = fileTree(layout.buildDirectory.dir("intermediates/cxx/Debug")) {
                include("**/obj/$abi/alr-test-command")
            }.files.singleOrNull()
                ?: throw GradleException("missing alr-test-command for $abi; run buildCMakeDebug[$abi] first")
            val builtTrampoline = fileTree(layout.buildDirectory.dir("intermediates/cxx/Debug")) {
                include("**/obj/$abi/alr-runtime-trampoline")
            }.files.singleOrNull()
                ?: throw GradleException("missing alr-runtime-trampoline for $abi; run buildCMakeDebug[$abi] first")
            val destDir = generatedDir.get().dir(abi).asFile
            destDir.mkdirs()
            builtTestCommand.copyTo(destDir.resolve("libalr_test_command.so"), overwrite = true)
            builtTrampoline.copyTo(destDir.resolve("libalr_runtime_trampoline.so"), overwrite = true)
        }
    }
}

// Build the alr runtime from runtime/alr and package it.
//
// Two artifacts, two toolchains, and they are NOT interchangeable:
//
//   libalr.so   the CLI + ptrace supervisor. An ANDROID/bionic aarch64
//               executable, built with the NDK. It ships in jniLibs, which is
//               how an APK ships an executable -- files named lib*.so there are
//               extracted to nativeLibraryDir with the execute bit.
//
//   libalr_preload.so
//               the guest interposer. A GLIBC object that is LD_PRELOADed into
//               Ubuntu binaries inside the rootfs. The NDK cannot build it --
//               it must link against glibc 2.17, which is what runtime/alr's
//               zig build does. It ships as an ASSET and is copied into the
//               rootfs at provisioning time; putting it in jniLibs would ask
//               bionic's linker to load a glibc object, which fails.
//
// Both fail loudly if their toolchain is missing. A build that silently ships
// without the runtime produces an app that looks fine and cannot execute
// anything, which is the most expensive failure this project has.
tasks.register("buildAlrRuntime") {
    val alrDir = layout.projectDirectory.dir("../runtime/alr")
    val jniOut = layout.buildDirectory.dir("generated/alr/jniLibs")
    val assetOut = layout.buildDirectory.dir("generated/alr/assets/alr")
    inputs.dir(alrDir.dir("src"))
    inputs.file(alrDir.file("Makefile"))
    outputs.dir(jniOut)
    outputs.dir(assetOut)
    doLast {
        val ndk = android.ndkDirectory
        require(ndk.isDirectory) { "NDK not found at $ndk; set ndkVersion or ANDROID_NDK_HOME" }

        // 1. the CLI, via the NDK
        val alrBuild = alrDir.dir("build").asFile
        alrBuild.mkdirs()
        providers.exec {
            workingDir = alrDir.asFile
            commandLine("make", "alr", "NDK=${ndk.absolutePath}")
        }.result.get().assertNormalExitValue()
        val alrBin = alrBuild.resolve("alr")
        require(alrBin.isFile) { "runtime/alr build produced no build/alr" }
        val abiDir = jniOut.get().dir("arm64-v8a").asFile
        abiDir.mkdirs()
        alrBin.copyTo(abiDir.resolve("libalr.so"), overwrite = true)

        // 2. the guest interposer, via zig (pinned 0.16.0 by runtime/alr)
        providers.exec {
            workingDir = alrDir.asFile
            commandLine("bash", "scripts/build-preload.sh")
        }.result.get().assertNormalExitValue()
        val preload = alrBuild.resolve("libalr_preload.so")
        require(preload.isFile) { "runtime/alr build produced no build/libalr_preload.so" }
        val assets = assetOut.get().asFile
        assets.mkdirs()
        preload.copyTo(assets.resolve("libalr_preload.so"), overwrite = true)
        val manifest = alrBuild.resolve("libalr_preload.manifest.json")
        if (manifest.isFile) manifest.copyTo(assets.resolve("manifest.json"), overwrite = true)

        // 3. the libdl.so.2 stub our own DT_NEEDED needs, for rootfs images
        // that were trimmed without it. alr installs it only when the guest
        // has none of its own.
        val libdl = alrBuild.resolve("libdl.so.2")
        require(libdl.isFile) { "runtime/alr build produced no build/libdl.so.2" }
        libdl.copyTo(assets.resolve("libdl.so.2"), overwrite = true)

        logger.lifecycle("alr runtime packaged: libalr.so + libalr_preload.so")
    }
}

// The asset/jniLib merge tasks consume buildAlrRuntime's output directories, so
// Gradle needs the edge declared or it refuses to order them (and would
// otherwise be free to package an empty or stale runtime).
tasks.matching { it.name.startsWith("merge") && (it.name.endsWith("Assets") || it.name.endsWith("JniLibFolders") || it.name.endsWith("NativeLibs")) }
    .configureEach { dependsOn("buildAlrRuntime") }

// packageProotCandidate removed: nothing bundles PRoot any more.


tasks.matching { it.name == "mergeDebugJniLibFolders" }.configureEach {
    dependsOn("packageNativeTestCommand", "buildAlrRuntime")
}

tasks.matching { it.name.startsWith("buildCMakeDebug") }.configureEach {
    finalizedBy("packageNativeTestCommand", "buildAlrRuntime")
}

dependencies {
    implementation("org.apache.commons:commons-compress:1.26.2")

    // Compose BOM pins every individual compose artifact version below.
    val composeBom = platform("androidx.compose:compose-bom:2024.09.00")
    implementation(composeBom)
    androidTestImplementation(composeBom)

    // Compose core / UI (versions pinned by the BOM — coordinates only).
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")              // ColorPainter/Painter (AppDetailScreen)
    implementation("androidx.compose.ui:ui-tooling-preview")       // @Preview (all screens)
    implementation("androidx.compose.foundation:foundation")       // LazyVerticalGrid/LazyRow/combinedClickable
    implementation("androidx.compose.material3:material3")          // Material3 (Scaffold/Card/Chip/…)
    implementation("androidx.compose.material:material-icons-core") // Icons.Filled.{Add,Delete,Info,Search,Settings}

    // Activity / Lifecycle / Navigation / ViewModel (Compose).
    implementation("androidx.activity:activity-compose:1.9.2")             // setContent / ComponentActivity (LauncherActivity)
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.6")       // lifecycleScope/repeatOnLifecycle (RunningSurfaceActivity)
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.6") // viewModel(factory=…) (AlrApp routes)
    implementation("androidx.navigation:navigation-compose:2.8.1")         // NavHost/composable/rememberNavController (AlrApp)

    // Coroutines (StateFlow/Flow — the whole runtime/ layer depends on it).
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")

    // Icon bitmap loader (optional — screens fall back to a category badge without it).
    implementation("io.coil-kt:coil-compose:2.7.0")

    // Compose debug tooling (Preview render / layout inspector).
    debugImplementation("androidx.compose.ui:ui-tooling")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}
