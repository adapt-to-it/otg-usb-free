package com.example.otgcam

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.ContentValues
import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.net.Uri
import android.provider.Settings as AndroidSettings
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
import android.widget.SeekBar
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
    private lateinit var hintBox: View
    private lateinit var hintActions: View
    private lateinit var btnRetry: Button
    private lateinit var btnGrant: Button
    private lateinit var btnAppSettings: Button
    private lateinit var camBadge: View
    private lateinit var camBadgeText: TextView
    private lateinit var topBar: View
    private lateinit var topGradient: View
    private lateinit var settingsPanel: ScrollView
    private lateinit var settings: Settings
    private lateinit var btnEv: ImageButton
    private lateinit var evPanel: View
    private lateinit var evSeekBrightness: SeekBar
    private lateinit var evSeekContrast: SeekBar
    private lateinit var evSeekExposure: SeekBar
    private lateinit var evValueBrightness: TextView
    private lateinit var evValueContrast: TextView
    private lateinit var evValueExposure: TextView
    private lateinit var colBrightness: View
    private lateinit var colContrast: View
    private lateinit var colExposure: View
    private var brightnessSupported = false
    private var contrastSupported = false
    private var exposureSupported = false
    private var exposureMin = 0
    private var exposureMax = 0

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
        // Inizializza il context Vulkan il prima possibile cosi' eventuali
        // errori vengono loggati subito; isReady=false attiva il fallback
        // Surface piu' avanti nella pipeline UVC.
        VulkanRenderer.init(this)

        setContentView(R.layout.activity_main)
        cameraView = findViewById(R.id.cameraView)
        hint = findViewById(R.id.hint)
        hintBox = findViewById(R.id.hintBox)
        hintActions = findViewById(R.id.hintActions)
        btnRetry = findViewById(R.id.btnRetry)
        btnGrant = findViewById(R.id.btnGrant)
        btnAppSettings = findViewById(R.id.btnAppSettings)
        camBadge = findViewById(R.id.camBadge)
        camBadgeText = findViewById(R.id.camBadgeText)
        topBar = findViewById(R.id.topBar)
        topGradient = findViewById(R.id.topGradient)
        settingsPanel = findViewById(R.id.settingsPanel)

        btnRetry.setOnClickListener { onRetryClicked() }
        btnGrant.setOnClickListener { requestCameraPermission() }
        btnAppSettings.setOnClickListener { openAppSettings() }

        btnEv = findViewById(R.id.btnEv)
        evPanel = findViewById(R.id.evPanel)
        evSeekBrightness = findViewById(R.id.evSeekBrightness)
        evSeekContrast = findViewById(R.id.evSeekContrast)
        evSeekExposure = findViewById(R.id.evSeekExposure)
        evValueBrightness = findViewById(R.id.evValueBrightness)
        evValueContrast = findViewById(R.id.evValueContrast)
        evValueExposure = findViewById(R.id.evValueExposure)
        colBrightness = findViewById(R.id.colBrightness)
        colContrast = findViewById(R.id.colContrast)
        colExposure = findViewById(R.id.colExposure)
        btnEv.setOnClickListener { toggleEvPanel() }
        findViewById<Button>(R.id.btnEvReset).setOnClickListener { resetImageControls() }

        bindEvSeek(evSeekBrightness, evValueBrightness, settings.brightnessPercent) { v ->
            settings.brightnessPercent = v; applyBrightness()
        }
        bindEvSeek(evSeekContrast, evValueContrast, settings.contrastPercent) { v ->
            settings.contrastPercent = v; applyContrast()
        }
        bindEvSeek(evSeekExposure, evValueExposure, settings.exposurePercent) { v ->
            settings.exposurePercent = v; applyExposure()
        }

        bindControls()
        populateSettings()

        cameraView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                surfaceReady = true
            }
            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, ht: Int) {
                // Solo qui la Surface ha dimensioni valide e può ricevere frame.
                surfaceReady = true
                rebindSurface()
            }
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

    override fun onNewIntent(intent: android.content.Intent?) {
        super.onNewIntent(intent)
        // L'activity è già viva e arriva un USB_DEVICE_ATTACHED:
        // forziamo un nuovo scan dei device collegati.
        try {
            previewAttached = false
            camera?.closeCamera()
        } catch (_: Exception) {}
        selectedDevice = null
        triggerInitialAttachIfPresent()
    }

    override fun onStop() {
        releaseCamera()
        super.onStop()
    }

    override fun onDestroy() {
        VulkanRenderer.shutdown()
        super.onDestroy()
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
        if (cameraOk) {
            initCamera()
        } else {
            // Se l'utente ha selezionato "Non chiedere più" non possiamo mostrare di nuovo
            // il dialog di sistema; offriamo apertura impostazioni dell'app.
            val canAskAgain = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
                shouldShowRequestPermissionRationale(Manifest.permission.CAMERA) else true
            setHint(
                getString(R.string.hint_permission_denied_full),
                showGrant = canAskAgain,
                showAppSettings = !canAskAgain
            )
        }
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
            settings.rotation = it.toIntOrNull() ?: 0; applyPreviewConfig(); applyAspect()
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
        val cam = camera ?: return
        val size = try { cam.previewSize } catch (_: Exception) { null } ?: return
        var srcW = size.width
        var srcH = size.height
        if (srcW <= 0 || srcH <= 0) return
        // Se la rotazione è 90/270 lo stream viene ruotato: invertiamo l'aspect.
        if (settings.rotation == 90 || settings.rotation == 270) {
            val tmp = srcW; srcW = srcH; srcH = tmp
        }
        val parent = cameraView.parent as? android.view.View ?: return
        val parentW = parent.width
        val parentH = parent.height
        if (parentW <= 0 || parentH <= 0) {
            cameraView.setAspectRatio(srcW, srcH)
            return
        }
        val lp = cameraView.layoutParams as? android.widget.FrameLayout.LayoutParams ?: return
        lp.gravity = android.view.Gravity.CENTER
        when (settings.aspect) {
            "stretch" -> {
                // Riempie completamente, distorsione consentita.
                cameraView.setAspectRatio(parentW, parentH)
                lp.width = android.view.ViewGroup.LayoutParams.MATCH_PARENT
                lp.height = android.view.ViewGroup.LayoutParams.MATCH_PARENT
            }
            "fill" -> {
                // Riempie il parent ritagliando i bordi (no bande nere).
                val parentAspect = parentW.toDouble() / parentH
                val srcAspect = srcW.toDouble() / srcH
                if (srcAspect > parentAspect) {
                    // sorgente più larga: riempi in altezza, allarga in larghezza
                    lp.height = parentH
                    lp.width = (parentH * srcAspect).toInt()
                } else {
                    lp.width = parentW
                    lp.height = (parentW / srcAspect).toInt()
                }
                cameraView.setAspectRatio(srcW, srcH)
            }
            else -> {
                // fit (contain): tutto visibile, eventuali bande nere.
                lp.width = android.view.ViewGroup.LayoutParams.MATCH_PARENT
                lp.height = android.view.ViewGroup.LayoutParams.MATCH_PARENT
                cameraView.setAspectRatio(srcW, srcH)
            }
        }
        cameraView.layoutParams = lp
        cameraView.requestLayout()
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

    // ---------- EV / Image controls ----------

    private fun bindEvSeek(seek: SeekBar, valueText: TextView, initial: Int, onChange: (Int) -> Unit) {
        seek.progress = initial
        valueText.text = initial.toString()
        seek.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar?, progress: Int, fromUser: Boolean) {
                valueText.text = progress.toString()
                if (fromUser) {
                    onChange(progress)
                    scheduleHideUi()
                }
            }
            override fun onStartTrackingTouch(sb: SeekBar?) {
                mainHandler.removeCallbacks(hideUiRunnable)
            }
            override fun onStopTrackingTouch(sb: SeekBar?) { scheduleHideUi() }
        })
    }

    private fun detectImageControlsSupport() {
        val ctrl = try { camera?.uvcControl } catch (_: Exception) { null }
        brightnessSupported = try { ctrl != null && ctrl.isBrightnessEnable } catch (_: Exception) { false }
        contrastSupported = try { ctrl != null && ctrl.isContrastEnable } catch (_: Exception) { false }
        exposureSupported = try { ctrl != null && ctrl.isExposureTimeAbsoluteEnable } catch (_: Exception) { false }
        if (exposureSupported) {
            try {
                val limit = ctrl!!.updateExposureTimeAbsoluteLimit()
                if (limit != null && limit.size >= 2) {
                    exposureMin = limit[0]
                    exposureMax = limit[1]
                    if (exposureMax <= exposureMin) exposureSupported = false
                } else exposureSupported = false
            } catch (_: Exception) { exposureSupported = false }
        }
        val anySupported = brightnessSupported || contrastSupported || exposureSupported
        runOnUiThread {
            btnEv.visibility = if (anySupported) View.VISIBLE else View.GONE
            if (!anySupported) evPanel.visibility = View.GONE
            colBrightness.visibility = if (brightnessSupported) View.VISIBLE else View.GONE
            colContrast.visibility = if (contrastSupported) View.VISIBLE else View.GONE
            colExposure.visibility = if (exposureSupported) View.VISIBLE else View.GONE
        }
    }

    private fun applyBrightness() {
        if (!brightnessSupported) return
        try { camera?.uvcControl?.setBrightnessPercent(settings.brightnessPercent) } catch (_: Exception) {}
    }

    private fun applyContrast() {
        if (!contrastSupported) return
        try { camera?.uvcControl?.setContrastPercent(settings.contrastPercent) } catch (_: Exception) {}
    }

    private fun applyExposure() {
        if (!exposureSupported || exposureMax <= exposureMin) return
        try {
            // Disabilita auto-exposure se la cam la supporta, altrimenti il valore manuale viene ignorato.
            val ctrl = camera?.uvcControl ?: return
            try { ctrl.setExposureTimeAuto(false) } catch (_: Exception) {}
            val pct = settings.exposurePercent.coerceIn(0, 100)
            val raw = exposureMin + ((exposureMax - exposureMin) * pct / 100)
            ctrl.setExposureTimeAbsolute(raw)
        } catch (_: Exception) {}
    }

    private fun applyAllImageControls() {
        applyBrightness()
        applyContrast()
        applyExposure()
    }

    private fun toggleEvPanel() {
        if (evPanel.visibility == View.VISIBLE) {
            evPanel.visibility = View.GONE
            scheduleHideUi()
        } else {
            evSeekBrightness.progress = settings.brightnessPercent
            evValueBrightness.text = settings.brightnessPercent.toString()
            evSeekContrast.progress = settings.contrastPercent
            evValueContrast.text = settings.contrastPercent.toString()
            evSeekExposure.progress = settings.exposurePercent
            evValueExposure.text = settings.exposurePercent.toString()
            evPanel.visibility = View.VISIBLE
            mainHandler.removeCallbacks(hideUiRunnable)
        }
    }

    private fun resetImageControls() {
        val ctrl = try { camera?.uvcControl } catch (_: Exception) { null }
        if (ctrl != null) {
            try { if (brightnessSupported) ctrl.resetBrightness() } catch (_: Exception) {}
            try { if (contrastSupported) ctrl.resetContrast() } catch (_: Exception) {}
            try { if (exposureSupported) {
                ctrl.resetExposureTimeAbsolute()
                ctrl.setExposureTimeAuto(true)
            } } catch (_: Exception) {}
        }
        // Rileggi i valori dopo il reset hardware e aggiorna UI + persistenza.
        val newBrightness = try { ctrl?.brightnessPercent ?: 50 } catch (_: Exception) { 50 }
        val newContrast = try { ctrl?.contrastPercent ?: 50 } catch (_: Exception) { 50 }
        // L'esposizione assoluta non ha API "percent", deduciamo dal valore corrente.
        val newExposure = try {
            if (exposureSupported && exposureMax > exposureMin) {
                val raw = ctrl?.exposureTimeAbsolute ?: exposureMin
                ((raw - exposureMin).toLong() * 100 / (exposureMax - exposureMin)).toInt().coerceIn(0, 100)
            } else 50
        } catch (_: Exception) { 50 }
        settings.brightnessPercent = newBrightness
        settings.contrastPercent = newContrast
        settings.exposurePercent = newExposure
        runOnUiThread {
            evSeekBrightness.progress = newBrightness
            evValueBrightness.text = newBrightness.toString()
            evSeekContrast.progress = newContrast
            evValueContrast.text = newContrast.toString()
            evSeekExposure.progress = newExposure
            evValueExposure.text = newExposure.toString()
            scheduleHideUi()
        }
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
            permissionRequested -> {
                val canAskAgain = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
                    shouldShowRequestPermissionRationale(Manifest.permission.CAMERA) else true
                setHint(
                    getString(R.string.hint_permission_denied_full),
                    showGrant = canAskAgain,
                    showAppSettings = !canAskAgain
                )
            }
            else -> {
                permissionRequested = true
                setHint(getString(R.string.hint_permission), showGrant = true)
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
        setHint(getString(R.string.hint_connect), showRetry = true)
        val cam = CameraHelper()
        cam.setStateCallback(object : ICameraHelper.StateCallback {
            override fun onAttach(device: UsbDevice?) {
                if (device == null) return
                if (selectedDevice?.deviceId == device.deviceId && previewAttached) return
                selectedDevice = device
                runOnUiThread {
                    setHint(getString(R.string.hint_connecting))
                    showCamBadge(deviceDisplayName(device), streaming = false)
                }
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
                    rebindSurface()
                    applyAspect()
                    detectImageControlsSupport()
                    applyAllImageControls()
                    hideHint()
                    showCamBadge(deviceDisplayName(device ?: selectedDevice))
                    scheduleHideUi()
                }
            }
            override fun onCameraClose(device: UsbDevice?) {
                runOnUiThread {
                    previewAttached = false
                    brightnessSupported = false
                    contrastSupported = false
                    exposureSupported = false
                    btnEv.visibility = View.GONE
                    evPanel.visibility = View.GONE
                    try { camera?.removeSurface(cameraView.holder.surface) } catch (_: Exception) {}
                    hideCamBadge()
                    setHint(getString(R.string.hint_connect), showRetry = true)
                }
            }
            override fun onDeviceClose(device: UsbDevice?) {}
            override fun onDetach(device: UsbDevice?) {
                if (device != null && selectedDevice?.deviceId == device.deviceId) selectedDevice = null
                previewAttached = false
                try { camera?.closeCamera() } catch (_: Exception) {}
                runOnUiThread {
                    hideCamBadge()
                    setHint(getString(R.string.hint_connect), showRetry = true)
                }
            }
            override fun onCancel(device: UsbDevice?) {
                if (device != null && selectedDevice?.deviceId == device.deviceId) selectedDevice = null
                runOnUiThread {
                    hideCamBadge()
                    setHint(getString(R.string.hint_connect), showRetry = true)
                }
            }
        })
        camera = cam
        try {
            val vcfg = cam.videoCaptureConfig
            if (vcfg != null) {
                vcfg.setAudioCaptureEnable(false)
                cam.videoCaptureConfig = vcfg
            }
        } catch (_: Exception) {}
        // Diamo alla libreria un istante per propagare lo stato del callback
        // prima di chiedere la lista dei device già collegati.
        mainHandler.postDelayed({ triggerInitialAttachIfPresent() }, 200)
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

    private fun rebindSurface() {
        val cam = camera ?: return
        val surface = cameraView.holder.surface ?: return
        if (!surfaceReady) return
        try {
            if (previewAttached) {
                try { cam.stopPreview() } catch (_: Exception) {}
                try { cam.removeSurface(surface) } catch (_: Exception) {}
                previewAttached = false
            }
            if (cam.isCameraOpened) {
                cam.addSurface(surface, false)
                previewAttached = true
                cam.startPreview()
            } else {
                cam.addSurface(surface, false)
                previewAttached = true
            }
        } catch (_: Exception) {}
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

    private fun setHint(text: String, showRetry: Boolean = false, showGrant: Boolean = false, showAppSettings: Boolean = false) {
        runOnUiThread {
            hint.text = text
            hintBox.visibility = View.VISIBLE
            btnRetry.visibility = if (showRetry) View.VISIBLE else View.GONE
            btnGrant.visibility = if (showGrant) View.VISIBLE else View.GONE
            btnAppSettings.visibility = if (showAppSettings) View.VISIBLE else View.GONE
            hintActions.visibility = if (showRetry || showGrant || showAppSettings) View.VISIBLE else View.GONE
        }
    }

    private fun hideHint() {
        runOnUiThread { hintBox.visibility = View.GONE }
    }

    private fun showCamBadge(name: String, streaming: Boolean = true) {
        runOnUiThread {
            camBadgeText.text = name
            camBadge.visibility = View.VISIBLE
            val dot = findViewById<View>(R.id.camBadgeDot)
            dot.setBackgroundResource(
                if (streaming) R.drawable.dot_streaming else R.drawable.dot_connecting
            )
        }
    }

    private fun hideCamBadge() {
        runOnUiThread { camBadge.visibility = View.GONE }
    }

    private fun deviceDisplayName(device: UsbDevice?): String {
        if (device == null) return getString(R.string.cam_unknown)
        val product = device.productName?.takeIf { it.isNotBlank() }
        if (product != null) return product
        val mfg = device.manufacturerName?.takeIf { it.isNotBlank() }
        val vidPid = String.format("USB %04X:%04X", device.vendorId, device.productId)
        return if (mfg != null) "$mfg ($vidPid)" else vidPid
    }

    private fun onRetryClicked() {
        if (!hasCameraPermission()) {
            ensurePermissionThenInit()
            return
        }
        // forza un re-scan dei device collegati
        try {
            previewAttached = false
            camera?.closeCamera()
        } catch (_: Exception) {}
        selectedDevice = null
        if (camera == null) initCamera() else triggerInitialAttachIfPresent()
    }

    private fun hasCameraPermission(): Boolean = missingPermissions().isEmpty()

    private fun requestCameraPermission() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return
        permissionRequested = true
        requestPermissions(arrayOf(Manifest.permission.CAMERA), REQ_PERM)
    }

    private fun openAppSettings() {
        val intent = Intent(AndroidSettings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
            data = Uri.fromParts("package", packageName, null)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        try { startActivity(intent) } catch (_: Exception) {}
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
