import java.util.Properties
import java.io.FileInputStream

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.example.otgcam"
    compileSdk = 34

    defaultConfig {
        applicationId = "it.adaptit.otgusbfree"
        minSdk = 26
        targetSdk = 34
        versionCode = 2
        versionName = "1.1"
        ndk {
            abiFilters += listOf("armeabi-v7a", "arm64-v8a", "x86", "x86_64")
        }
        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++17", "-Wall", "-Wextra")
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        // Abilita il consumo dei moduli prefab del NDK (shaderc, ecc.).
        prefab = true
    }

    sourceSets {
        // Le validation layers di Vulkan vengono scaricate dal task
        // downloadValidationLayers + extractValidationLayers e iniettate
        // come jniLibs solo nella variant debug.
        getByName("debug") {
            jniLibs.srcDir(layout.buildDirectory.dir("validation-layers/jniLibs"))
        }
    }

    signingConfigs {
        create("release") {
            val propsFile = rootProject.file("keystore.properties")
            if (propsFile.exists()) {
                val props = Properties()
                FileInputStream(propsFile).use { props.load(it) }
                storeFile = rootProject.file(props.getProperty("storeFile"))
                storePassword = props.getProperty("storePassword")
                keyAlias = props.getProperty("keyAlias")
                keyPassword = props.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            val rel = signingConfigs.getByName("release")
            if (rel.storeFile?.exists() == true) {
                signingConfig = rel
            }
        }
        debug {
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    packaging {
        jniLibs.useLegacyPackaging = true
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.herohan:UVCAndroid:1.0.9")
}

// ---------- Vulkan validation layers ----------
//
// Le validation layers ufficiali sono distribuite come zip pre-compilato
// da KhronosGroup. Vengono scaricate solo se manca lo zip in cache,
// estratte in build/validation-layers/jniLibs/<abi>/ e iniettate come
// sorgente jniLibs della variant debug. In release non vengono incluse.
//
// Se il download fallisce (rete assente, firewall) la build prosegue
// senza layers: il codice nativo gestisce la loro assenza a runtime.

val validationLayersVersion = "1.3.290.0"

val downloadValidationLayers by tasks.registering {
    val outDir = layout.buildDirectory.dir("validation-layers").get().asFile
    val zipFile = File(outDir, "android-binaries-$validationLayersVersion.zip")
    val markerFile = File(outDir, "downloaded-$validationLayersVersion.marker")
    outputs.file(markerFile)

    doLast {
        outDir.mkdirs()
        if (!zipFile.exists()) {
            val url = "https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/" +
                "download/vulkan-sdk-$validationLayersVersion/" +
                "android-binaries-$validationLayersVersion.zip"
            logger.lifecycle("Downloading Vulkan validation layers: $url")
            try {
                uri(url).toURL().openStream().use { input ->
                    zipFile.outputStream().use { output -> input.copyTo(output) }
                }
            } catch (t: Throwable) {
                logger.warn(
                    "Vulkan validation layers download failed (${t.message}). " +
                        "Debug builds will run without validation layers."
                )
            }
        }
        markerFile.writeText(if (zipFile.exists()) zipFile.absolutePath else "missing")
    }
}

val extractValidationLayers by tasks.registering(Copy::class) {
    dependsOn(downloadValidationLayers)
    val zipFile = layout.buildDirectory
        .file("validation-layers/android-binaries-$validationLayersVersion.zip").get().asFile
    onlyIf { zipFile.exists() }
    from({ zipTree(zipFile) })
    into(layout.buildDirectory.dir("validation-layers/jniLibs"))
}

// Ancora extractValidationLayers ai task di merge jniLibs della variant debug.
// `tasks.matching` è una live collection: l'hook si applica anche a task creati
// successivamente da AGP, quindi non serve afterEvaluate.
tasks.matching { task ->
    task.name == "mergeDebugJniLibFolders" || task.name == "mergeDebugNativeLibs"
}.configureEach {
    dependsOn(extractValidationLayers)
}
