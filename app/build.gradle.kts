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
        targetSdk = 35
        versionCode = 137
        versionName = "0.4.137-cp6-mr2-v137"
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

tasks.register("packageProotCandidate") {
    val generatedDir = layout.buildDirectory.dir("generated/native-test-command/jniLibs")
    val prebuiltNativeDir = layout.projectDirectory.dir("src/main/prebuiltNative")
    outputs.dir(generatedDir)
    inputs.dir(prebuiltNativeDir).optional()
    outputs.upToDateWhen { false }
    doLast {
        val abis = listOf("arm64-v8a", "armeabi-v7a", "x86", "x86_64")
        abis.forEach { abi ->
            val destDir = generatedDir.get().dir(abi).asFile
            destDir.mkdirs()
            val prebuiltProot = prebuiltNativeDir.dir(abi).file("libalr_proot.so").asFile
            if (prebuiltProot.isFile) {
                prebuiltProot.copyTo(destDir.resolve("libalr_proot.so"), overwrite = true)
                val prebuiltTalloc = prebuiltNativeDir.dir(abi).file("libtalloc.so").asFile
                if (prebuiltTalloc.isFile) {
                    prebuiltTalloc.copyTo(destDir.resolve("libtalloc.so"), overwrite = true)
                }
                val prebuiltProotLoader = prebuiltNativeDir.dir(abi).file("libproot-loader.so").asFile
                if (prebuiltProotLoader.isFile) {
                    prebuiltProotLoader.copyTo(destDir.resolve("libproot-loader.so"), overwrite = true)
                }
            } else {
                val built = fileTree(layout.buildDirectory.dir("intermediates/cxx/Debug")) {
                    include("**/obj/$abi/alr-proot-candidate")
                }.files.singleOrNull()
                    ?: throw GradleException("missing alr-proot-candidate for $abi; run buildCMakeDebug[$abi] first")
                built.copyTo(destDir.resolve("libalr_proot.so"), overwrite = true)
            }
        }
    }
}

tasks.matching { it.name == "mergeDebugJniLibFolders" }.configureEach {
    dependsOn("packageNativeTestCommand", "packageProotCandidate")
}

tasks.matching { it.name.startsWith("buildCMakeDebug") }.configureEach {
    finalizedBy("packageNativeTestCommand", "packageProotCandidate")
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
