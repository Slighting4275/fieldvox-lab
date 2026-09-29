plugins {
    id("com.android.application")
}

android {
    namespace = "org.fieldvox.lab"
    compileSdk = 37
    buildToolsVersion = "37.0.0"

    defaultConfig {
        applicationId = "org.fieldvox.lab"
        minSdk = 30
        targetSdk = 37
        versionCode = 1
        versionName = "0.1.0-stage1u-n-audio-dwell"

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20", "-O3", "-Wall", "-Wextra")
                cFlags += listOf("-std=c11", "-O3", "-Wall", "-Wextra")
            }
        }
    }

    ndkVersion = "30.0.16248370"

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
}
