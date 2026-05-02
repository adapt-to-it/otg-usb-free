package com.example.otgcam

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.ContentValues
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.MediaStore
import android.view.SurfaceHolder
import android.view.View
import android.view.WindowManager
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.ImageButton
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import com.herohan.uvcapp.CameraHelper
import com.herohan.uvcapp.IImageCapture
import com.herohan.uvcapp.ICameraHelper
import com.serenegiant.usb.Size
import com.serenegiant.usb.UVCParam
import com.serenegiant.widget.AspectRatioSurfaceView
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class MainActivity : Activity() {

    private companion object {
        const val REQ_PERM = 1
        const val UI_HIDE_DELAY_MS = 4000L
        const val MIRROR_NONE = 0
        const val MIRROR_HORIZONTAL = 1
        const val MIRROR_VERTICAL = 2
        const val MIRROR_BOTH = 3
    }

    private lateinit var cameraView: AspectRatioSurfaceView
    private lateinit var hint: TextView
    private lateinit var topBar: View
    private lateinit var topGradient: View
    private lateinit var settingsPanel: ScrollView
    private lateinit var settings: Settings

    private var camera: ICameraHelper? = null
    private var selectedDevice: UsbDevice? = null
    private var surfaceReady = false
    private var previewAttached = false
    private var permissionRequested = false

    private val mainHandler = Handler(Looper.getMainLooper())
    private val hideUiRunnable = Runnable { hideOverlayUi() }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = Settings(this)
        applyOrientationFromSettings()
        applyKeepScreenOnFromSettings()

        setContentView(R.layout.activity_main)
        cameraView = findViewById(R.id.cameraView)
        hint = findViewById(R.id.hint)
        topBar = findViewById(R.id.topBar)
        topGradient = findViewById(R.id.topGradient)
        settingsPanel = findViewById(R.id.settingsPanel)

        bindControls()
        populateSettings()

        cameraView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                surfaceReady = true
                attachSurfaceIfReady()
            }
            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, ht: Int) {}
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                surfaceReady = false
                previewAttached = false
                try { camera?.removeSurface(holder.surface) } catch (_: Exception) {}
            }
        })

        cameraView.setOnClickListener { toggleOverlayUi() }
        scheduleHideUi()
        hideSystemUi()
    }

    override fun onStart() {
        super.onStart()
        ensurePermissionThenInit()
    }

    override fun onStop() {
        releaseCamera()
        super.onStop()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemUi()
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQ_PERM) return
        val cameraOk = checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        if (cameraOk) initCamera() else setHint(getString(R.string.hint_permission_denied))
    }

    // ---------- UI ----------

    private fun bindControls() {
        findViewById<ImageButton>(R.id.btnSettings).setOnClickListener { showSettingsPanel() }
        findViewById<ImageButton>(R.id.btnCloseSettings).setOnClickListener { hideSettingsPanel() }
        findViewById<ImageButton>(R.id.btnSnapshot).setOnClickListener { takeSnapshot() }
        findViewById<ImageButton>(R.id.btnInfo).setOnClickListener { showInfo() }
        findViewById<Button>(R.id.btnReset).setOnClickListener {
            settings.reset()
            populateSettings()
            applyAllSettings()
            Toast.makeText(this, R.string.reset_defaults, Toast.LENGTH_SHORT).show()
        }
    }

    private fun populateSettings() {
        bindSpinner(R.id.spResolution, R.array.resolution_entries, R.array.resolution_values, settings.resolution) {
            settings.resolution = it; restartPreview()
        }
        bindSpinner(R.id.spFps, R.array.fps_entries, R.array.fps_values, settings.fps.toString()) {
            settings.fps = it.toIntOrNull() ?: 0; restartPreview()
        }
        bindSpinner(R.id.spAspect, R.array.aspect_entries, R.array.aspect_values, settings.aspect) {
            settings.aspect = it; applyAspect()
        }
        bindSpinner(R.id.spRotation, R.array.rotation_entries, R.array.rotation_values, settings.rotation.toString()) {
            settings.rotation = it.toIntOrNull() ?: 0; applyPreviewConfig()
        }
        bindSpinner(R.id.spOrientation, R.array.orientation_entries, R.array.orientation_values, settings.orientation) {
            settings.orientation = it; applyOrientationFromSettings()
        }
        findViewById<CheckBox>(R.id.cbMirrorH).apply {
            isChecked = settings.mirrorH
            setOnCheckedChangeListener { _, v -> settings.mirrorH = v; applyPreviewConfig() }
        }
        findViewById<CheckBox>(R.id.cbMirrorV).apply {
            isChecked = settings.mirrorV
            setOnCheckedChangeListener { _, v -> settings.mirrorV = v; applyPreviewConfig() }
        }
        findViewById<CheckBox>(R.id.cbKeepScreen).apply {
            isChecked = settings.keepScreenOn
            setOnCheckedChangeListener { _, v -> settings.keepScreenOn = v; applyKeepScreenOnFromSettings() }
        }
        findViewById<CheckBox>(R.id.cbHideUiAuto).apply {
            isChecked = settings.hideUiAuto
            setOnCheckedChangeListener { _, v -> settings.hideUiAuto = v; if (v) scheduleHideUi() else mainHandler.removeCallbacks(hideUiRunnable) }
        }
    }

    private fun bindSpinner(
        id: Int, entriesRes: Int, valuesRes: Int,
        currentValue: String, onPick: (String) -> Unit
    ) {
        val sp = findViewById<Spinner>(id)
        val entries = resources.getStringArray(entriesRes)
        val values = resources.getStringArray(valuesRes)
        sp.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, entries)
        val idx = values.indexOf(currentValue).coerceAtLeast(0)
        sp.setSelection(idx, false)
        sp.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: AdapterView<*>?, v: View?, pos: Int, id: Long) {
                val newVal = values[pos]
                if (newVal != currentValueOf(sp.tag)) {
                    sp.tag = newVal
                    onPick(newVal)
                }
            }
            override fun onNothingSelected(p: AdapterView<*>?) {}
        }
        sp.tag = currentValue
    }

    private fun currentValueOf(tag: Any?): String = (tag as? String) ?: ""

    private fun showSettingsPanel() {
        mainHandler.removeCallbacks(hideUiRunnable)
        settingsPanel.visibility = View.VISIBLE
        showOverlayUi()
    }

    private fun hideSettingsPanel() {
        settingsPanel.visibility = View.GONE
        scheduleHideUi()
    }

    private fun toggleOverlayUi() {
        if (settingsPanel.visibility == View.VISIBLE) {
            hideSettingsPanel(); return
        }
        if (topBar.visibility == View.VISIBLE) hideOverlayUi() else showOverlayUi()
    }

    private fun showOverlayUi() {
        topBar.visibility = View.VISIBLE
        topGradient.visibility = View.VISIBLE
        scheduleHideUi()
    }

    private fun hideOverlayUi() {
        if (settingsPanel.visibility == View.VISIBLE) return
        topBar.visibility = View.GONE
        topGradient.visibility = View.GONE
    }

    private fun scheduleHideUi() {
        mainHandler.removeCallbacks(hideUiRunnable)
        if (settings.hideUiAuto) {
            mainHandler.postDelayed(hideUiRunnable, UI_HIDE_DELAY_MS)
        }
    }

    private fun showInfo() {
        val version = try { packageManager.getPackageInfo(packageName, 0).versionName } catch (_: Exception) { "1.0" }
        AlertDialog.Builder(this)
            .setTitle(R.string.info_title)
            .setMessage(getString(R.string.info_body, version))
            .setPositiveButton(R.string.info_close, null)
            .show()
    }

    // ---------- Settings application ----------

    private fun applyOrientationFromSettings() {
        requestedOrientation = when (settings.orientation) {
            "landscape" -> ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
            "portrait" -> ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
            else -> ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        }
    }

    private fun applyKeepScreenOnFromSettings() {
        if (settings.keepScreenOn) {
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
    }

    private fun applyAllSettings() {
        applyOrientationFromSettings()
        applyKeepScreenOnFromSettings()
        applyPreviewConfig()
        applyAspect()
    }

    private fun applyPreviewConfig() {
        val cam = camera ?: return
        try {
            val cfg = cam.previewConfig ?: return
            cfg.setRotation(settings.rotation)
            var mirror = MIRROR_NONE
            if (settings.mirrorH) mirror = mirror or MIRROR_HORIZONTAL
            if (settings.mirrorV) mirror = mirror or MIRROR_VERTICAL
            cfg.setMirror(mirror)
            cam.previewConfig = cfg
        } catch (_: Exception) {}
    }

    private fun applyAspect() {
        // AspectRatioSurfaceView gestisce solo "fit"; per fill/stretch
        // useremo l'aspect ratio per dare la proporzione preview.
        val cam = camera ?: return
        val size = try { cam.previewSize } catch (_: Exception) { null } ?: return
        val w = size.width
        val h = size.height
        if (w <= 0 || h <= 0) return
        when (settings.aspect) {
            "stretch" -> cameraView.setAspectRatio(cameraView.width, cameraView.height.coerceAtLeast(1))
            "fill" -> {
                val viewAspect = cameraView.width.toDouble() / cameraView.height.coerceAtLeast(1)
                val frameAspect = w.toDouble() / h
                if (frameAspect > viewAspect) {
                    cameraView.setAspectRatio(cameraView.width, cameraView.height.coerceAtLeast(1))
                } else {
                    cameraView.setAspectRatio(w, h)
                }
            }
            else -> cameraView.setAspectRatio(w, h)
        }
    }

    private fun restartPreview() {
        val cam = camera ?: return
        try {
            cam.stopPreview()
            cam.closeCamera()
            val dev = selectedDevice ?: return
            cam.selectDevice(dev)
        } catch (_: Exception) {}
    }

    // ---------- Snapshot ----------

    private fun takeSnapshot() {
        val cam = camera ?: run {
            Toast.makeText(this, R.string.snapshot_failed, Toast.LENGTH_SHORT).show()
            return
        }
        try {
            val ts = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
            val name = "USBCam_$ts.jpg"
            val cv = ContentValues().apply {
                put(MediaStore.Images.Media.DISPLAY_NAME, name)
                put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    put(MediaStore.Images.Media.RELATIVE_PATH, "Pictures/USBCameraViewer")
                }
            }
            val opts = IImageCapture.OutputFileOptions.Builder(
                contentResolver,
                MediaStore.Images.Media.EXTERNAL_CONTENT_URI,
                cv
            ).build()
            cam.takePicture(opts, object : IImageCapture.OnImageCaptureCallback {
                override fun onImageSaved(r: IImageCapture.OutputFileResults) {
                    runOnUiThread { Toast.makeText(this@MainActivity, R.string.snapshot_saved, Toast.LENGTH_SHORT).show() }
                }
                override fun onError(code: Int, msg: String, t: Throwable?) {
                    runOnUiThread { Toast.makeText(this@MainActivity, R.string.snapshot_failed, Toast.LENGTH_SHORT).show() }
                }
            })
        } catch (_: Exception) {
            Toast.makeText(this, R.string.snapshot_failed, Toast.LENGTH_SHORT).show()
        }
    }

    // ---------- Permissions ----------

    private fun ensurePermissionThenInit() {
        val missing = missingPermissions()
        when {
            missing.isEmpty() -> initCamera()
            permissionRequested -> setHint(getString(R.string.hint_permission))
            else -> {
                permissionRequested = true
                setHint(getString(R.string.hint_permission))
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                    requestPermissions(missing, REQ_PERM)
                }
            }
        }
    }

    private fun missingPermissions(): Array<String> {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return emptyArray()
        return listOf(Manifest.permission.CAMERA)
            .filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
            .toTypedArray()
    }

    // ---------- Camera ----------

    private fun initCamera() {
        if (camera != null) {
            triggerInitialAttachIfPresent()
            return
        }
        setHint(getString(R.string.hint_connect))
        val cam = CameraHelper()
        cam.setStateCallback(object : ICameraHelper.StateCallback {
            override fun onAttach(device: UsbDevice?) {
                if (device == null) return
                if (selectedDevice?.deviceId == device.deviceId) return
                selectedDevice = device
                runOnUiThread { setHint(getString(R.string.hint_connecting)) }
                try { camera?.selectDevice(device) } catch (_: Exception) {}
            }
            override fun onDeviceOpen(device: UsbDevice?, isFirstOpen: Boolean) {
                try {
                    val param = buildUvcParam()
                    camera?.openCamera(param)
                } catch (_: Exception) {
                    try { camera?.openCamera(UVCParam()) } catch (_: Exception) {}
                }
            }
            override fun onCameraOpen(device: UsbDevice?) {
                runOnUiThread {
                    applyPreviewConfig()
                    attachSurfaceIfReady()
                    try { camera?.startPreview() } catch (_: Exception) {}
                    applyAspect()
                    hint.visibility = View.GONE
                    scheduleHideUi()
                }
            }
            override fun onCameraClose(device: UsbDevice?) {
                runOnUiThread {
                    previewAttached = false
                    try { camera?.removeSurface(cameraView.holder.surface) } catch (_: Exception) {}
                    setHint(getString(R.string.hint_connect))
                }
            }
            override fun onDeviceClose(device: UsbDevice?) {}
            override fun onDetach(device: UsbDevice?) {
                if (device != null && selectedDevice?.deviceId == device.deviceId) selectedDevice = null
                runOnUiThread { setHint(getString(R.string.hint_connect)) }
            }
            override fun onCancel(device: UsbDevice?) {
                if (device != null && selectedDevice?.deviceId == device.deviceId) selectedDevice = null
                runOnUiThread { setHint(getString(R.string.hint_connect)) }
            }
        })
        camera = cam
        triggerInitialAttachIfPresent()
    }

    private fun buildUvcParam(): UVCParam {
        val w = settings.resolutionWidth()
        val h = settings.resolutionHeight()
        val fps = settings.fps
        if (w <= 0 || h <= 0) return UVCParam()
        val size = Size(0, w, h, if (fps > 0) fps else 0, null)
        return UVCParam(size, 0)
    }

    private fun triggerInitialAttachIfPresent() {
        val cam = camera ?: return
        try {
            val devices = cam.deviceList ?: return
            if (devices.isEmpty()) return
            val first = devices[0]
            if (selectedDevice?.deviceId == first.deviceId) return
            selectedDevice = first
            setHint(getString(R.string.hint_connecting))
            cam.selectDevice(first)
        } catch (_: Exception) {
            val um = getSystemService(USB_SERVICE) as? UsbManager ?: return
            val first = um.deviceList?.values?.firstOrNull() ?: return
            if (selectedDevice?.deviceId == first.deviceId) return
            selectedDevice = first
            setHint(getString(R.string.hint_connecting))
            try { cam.selectDevice(first) } catch (_: Exception) {}
        }
    }

    private fun attachSurfaceIfReady() {
        val cam = camera ?: return
        if (!surfaceReady || previewAttached) return
        val surface = cameraView.holder.surface ?: return
        try {
            cam.addSurface(surface, false)
            previewAttached = true
        } catch (_: Exception) {}
    }

    private fun releaseCamera() {
        mainHandler.removeCallbacks(hideUiRunnable)
        camera?.let { c ->
            try { c.stopPreview() } catch (_: Exception) {}
            try { c.closeCamera() } catch (_: Exception) {}
            try { c.release() } catch (_: Exception) {}
        }
        camera = null
        selectedDevice = null
        previewAttached = false
    }

    private fun setHint(text: String) {
        runOnUiThread {
            hint.text = text
            hint.visibility = View.VISIBLE
        }
    }

    private fun hideSystemUi() {
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = (
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_FULLSCREEN
        )
    }
}
