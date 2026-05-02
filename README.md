# OTG USB Free

> **Coming soon on Google Play Store**

Free and open-source Android app that displays the live video feed from a USB UVC camera connected to your Android device via an OTG cable.

Designed to be lightweight, ad-free, and privacy-friendly: no data collected, no tracking, no internet access required.

## Features

- Full-screen real-time preview of the connected USB UVC camera
- Settings panel with persisted preferences:
  - Resolution (auto, 320×240 → 1920×1080)
  - Frame rate (auto, 15 / 24 / 30 / 60 fps)
  - Image fit: contain / fill / stretch
  - Rotation: 0° / 90° / 180° / 270°
  - Horizontal and vertical mirror
  - Screen orientation (auto / landscape / portrait)
  - Keep screen on
  - Auto-hide UI
- JPEG snapshot saved to `Pictures/USBCameraViewer`
- Tap on the preview to show/hide the toolbar
- Adaptive launcher icon

## Related project

[Discover how to easily share video files and documents locally and remotely](https://droplnk.app) — see also [droplnk.app/s/](https://droplnk.app/s/).

## Privacy

The app does **not** collect, store or transmit any personal data.
See the full [Privacy Policy](https://adapt-to-it.github.io/otg-usb-free/privacy-policy.html).

## Build

Requires Android Studio (Giraffe+) or `./gradlew assembleDebug` with Android SDK 34. Java 17 is required by Gradle 8.7.

```bash
./gradlew assembleDebug
# APK: app/build/outputs/apk/debug/app-debug.apk
```

## Release build for Play Store

1. Generate a keystore (one time):
   ```bash
   keytool -genkeypair -v -keystore release.keystore -alias otgusbfree \
     -keyalg RSA -keysize 2048 -validity 10000
   ```
2. Create `keystore.properties` in the project root (do **not** commit it):
   ```
   storeFile=release.keystore
   storePassword=********
   keyAlias=otgusbfree
   keyPassword=********
   ```
3. Build the AAB:
   ```bash
   ./gradlew bundleRelease
   # AAB: app/build/outputs/bundle/release/app-release.aab
   ```

## Tech

- Native UVC binding: [`com.herohan:UVCAndroid`](https://github.com/jiangdongguo/AndroidUSBCamera) `1.0.9`
- `applicationId`: `it.adaptit.otgusbfree`
- `minSdk` 21, `targetSdk` 34
- Supported ABIs: armeabi-v7a, arm64-v8a, x86, x86_64
- Runtime permissions: `CAMERA` only (no audio, no storage)

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for the full text.
