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
        versionCode = 8
        versionName = "0.5.1-native"

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                cFlags += listOf("-std=c23", "-O3", "-Wall", "-Wextra")
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
