# UVCAndroid native bindings — non offuscare classi native
-keep class com.serenegiant.usb.** { *; }
-keep class com.serenegiant.widget.** { *; }
-keep class com.herohan.uvcapp.** { *; }
-keepclassmembers class * {
    native <methods>;
}
-dontwarn com.serenegiant.**
-dontwarn com.herohan.**
