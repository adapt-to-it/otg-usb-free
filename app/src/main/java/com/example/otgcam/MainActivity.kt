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
import android.view.SurfaceView
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
import com.serenegiant.usb.IFrameCallback
import com.serenegiant.usb.Size
import com.serenegiant.usb.UVCCamera
import com.serenegiant.usb.UVCParam
import java.nio.ByteBuffer
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

    private lateinit var cameraView: SurfaceView
    private lateinit var aspectController: AspectController
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
    private var exposureDef = 0

    // Compute enhancement (Vulkan compute shader).
    private lateinit var btnEnh: ImageButton
    private lateinit var enhPanel: View
    private lateinit var enhSeekDefog: SeekBar
    private lateinit var enhSeekSharp: SeekBar
    private lateinit var enhSeekDenoise: SeekBar
    private lateinit var enhValueDefog: TextView
    private lateinit var enhValueSharp: TextView
    private lateinit var enhValueDenoise: TextView
    private lateinit var cbDefogAuto: CheckBox

    private var camera: ICameraHelper? = null
    private var selectedDevice: UsbDevice? = null
    private var surfaceReady = false
    private var previewAttached = false
    private var permissionRequested = false

    private val mainHandler = Handler(Looper.getMainLooper())
    private val hideUiRunnable = Runnable { hideOverlayUi() }

    // Riceve i frame UVC quando il path Vulkan e' attivo. Il callback gira
    // sul thread interno di UVCAndroid; il native e' protetto dal mutex globale.
    @Volatile private var lastFrameW = 0
    @Volatile private var lastFrameV = 0
    private val vulkanFrameCallback = object : IFrameCallback {
        override fun onFrame(buffer: ByteBuffer) {
            val sz = lastPreviewSize ?: return
            if (sz.width <= 0 || sz.height <= 0) return
            // ByteBuffer fornito da UVCAndroid: deve essere direct.
            if (!buffer.isDirect) return
            val ok = VulkanRenderer.uploadFrameYUYV(buffer, sz.width, sz.height)
            if (ok) {
                VulkanRenderer.renderFrame()
            }
        }
    }
    @Volatile private var lastPreviewSize: Size? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = Settings(this)
        aspectController = AspectController(settings)
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
        findViewById<ImageButton>(R.id.btnEvClose).setOnClickListener {
            evPanel.visibility = View.GONE
            scheduleHideUi()
        }

        // Enhancement panel (compute Vulkan).
        btnEnh = findViewById(R.id.btnEnh)
        enhPanel = findViewById(R.id.enhPanel)
        enhSeekDefog = findViewById(R.id.enhSeekDefog)
        enhSeekSharp = findViewById(R.id.enhSeekSharp)
        enhSeekDenoise = findViewById(R.id.enhSeekDenoise)
        enhValueDefog = findViewById(R.id.enhValueDefog)
        enhValueSharp = findViewById(R.id.enhValueSharp)
        enhValueDenoise = findViewById(R.id.enhValueDenoise)
        cbDefogAuto = findViewById(R.id.cbDefogAuto)
        btnEnh.setOnClickListener { toggleEnhPanel() }
        findViewById<Button>(R.id.btnEnhReset).setOnClickListener { resetEnhancement() }
        findViewById<ImageButton>(R.id.btnEnhClose).setOnClickListener {
            enhPanel.visibility = View.GONE
            scheduleHideUi()
        }
        bindEnhSeek(enhSeekDefog, enhValueDefog, settings.defogPercent) { v ->
            settings.defogPercent = v; applyEnhancement()
        }
        bindEnhSeek(enhSeekSharp, enhValueSharp, settings.sharpenPercent) { v ->
            settings.sharpenPercent = v; applyEnhancement()
        }
        bindEnhSeek(enhSeekDenoise, enhValueDenoise, settings.denoisePercent) { v ->
            settings.denoisePercent = v; applyEnhancement()
        }
        cbDefogAuto.isChecked = settings.defogEnabled
        cbDefogAuto.setOnCheckedChangeListener { _, v ->
            settings.defogEnabled = v; applyEnhancement()
        }
        // Visibile solo se Vulkan compute path attivo.
        if (VulkanRenderer.isReady) {
            btnEnh.visibility = View.VISIBLE
        }

        bindEvSeek(evSeekBrightness, evValueBrightness, settings.brightnessPercent) { v ->
            settings.brightnessPercent = v; applyBrightness()
        }
        bindEvSeek(evSeekContrast, evValueContrast, settings.contrastPercent) { v ->
            settings.contrastPercent = v; applyContrast()
        }
        // Esposizione: seekbar discreta in mezzi-stop EV (-6..+6, passo 0.5).
        // SeekBar nativo lavora con 0..max, quindi mappiamo:
        //   ui_pos = ev_half + 12   (0..24)
        bindEvSeekExposure()

        bindControls()
        populateSettings()

        cameraView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                surfaceReady = true
                if (VulkanRenderer.isReady) {
                    if (VulkanRenderer.attachSurface(holder.surface)) {
                        // Step 3: render di un clear-color rosso per verificare che
                        // la swapchain Vulkan e' viva. Step 4 sostituira' questa
                        // chiamata con un render driven dai frame UVC.
                        VulkanRenderer.renderClear(1f, 0f, 0f, 1f)
                    }
                }
            }
            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, ht: Int) {
                // Solo qui la Surface ha dimensioni valide e può ricevere frame.
                surfaceReady = true
                if (VulkanRenderer.isReady) {
                    // Resize / rotazione del compositor: ricreiamo la swapchain.
                    if (VulkanRenderer.attachSurface(h.surface)) {
                        VulkanRenderer.renderClear(1f, 0f, 0f, 1f)
                    }
                    applyAspect()
                } else {
                    rebindSurface()
                }
            }
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                surfaceReady = false
                previewAttached = false
                if (VulkanRenderer.isReady) {
                    VulkanRenderer.detachSurface()
                } else {
                    try { camera?.removeSurface(holder.surface) } catch (_: Exception) {}
                }
            }
        })

        cameraView.setOnClickListener {
            // Clic esterno: se un pannello e' aperto lo chiude (priorita' su
            // toggle overlay). Settings ha priorita' perche' e' modale.
            if (settingsPanel.visibility == View.VISIBLE) {
                hideSettingsPanel()
            } else if (evPanel.visibility == View.VISIBLE) {
                evPanel.visibility = View.GONE
                scheduleHideUi()
            } else if (enhPanel.visibility == View.VISIBLE) {
                enhPanel.visibility = View.GONE
                scheduleHideUi()
            } else {
                toggleOverlayUi()
            }
        }
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
            setOnCheckedChangeListener { _, v ->
                settings.mirrorH = v
                applyPreviewConfig()
                applyAspect()  // path Vulkan: mirror passa via setTransform
            }
        }
        findViewById<CheckBox>(R.id.cbMirrorV).apply {
            isChecked = settings.mirrorV
            setOnCheckedChangeListener { _, v ->
                settings.mirrorV = v
                applyPreviewConfig()
                applyAspect()
            }
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

    // Ripopola gli spinner Risoluzione/FPS con i valori effettivamente
    // supportati dalla cam (cam.supportedSizeList). Va invocato dopo
    // onCameraOpen e ad ogni cambio di risoluzione (per aggiornare gli FPS).
    private fun populateDynamicCameraOptions() {
        val cam = camera ?: return
        val sizes: List<Size> = try { cam.supportedSizeList ?: emptyList() }
                                catch (_: Exception) { emptyList() }
        if (sizes.isEmpty()) return

        // Risoluzioni uniche, ordinate per pixel count decrescente.
        val resPairs = sizes.map { it.width to it.height }
            .distinct()
            .sortedByDescending { it.first.toLong() * it.second }
        val resValues = listOf("auto") + resPairs.map { "${it.first}x${it.second}" }
        val resEntries = listOf("Auto") + resPairs.map { "${it.first}×${it.second}" }

        rebindSpinner(R.id.spResolution, resEntries, resValues, settings.resolution) {
            settings.resolution = it
            populateDynamicCameraOptions()  // ricalcola gli FPS per la nuova res
            restartPreview()
        }

        // FPS supportati per la risoluzione attualmente selezionata.
        val curW = settings.resolutionWidth()
        val curH = settings.resolutionHeight()
        val match = sizes.firstOrNull { it.width == curW && it.height == curH }
        val fpsList: List<Int> = match?.fpsList?.distinct()?.sorted()
            ?: sizes.flatMap { it.fpsList ?: emptyList() }.distinct().sorted()
        val fpsValues = listOf("0") + fpsList.map { it.toString() }
        val fpsEntries = listOf("Auto") + fpsList.map { "$it fps" }
        rebindSpinner(R.id.spFps, fpsEntries, fpsValues, settings.fps.toString()) {
            settings.fps = it.toIntOrNull() ?: 0
            restartPreview()
        }
    }

    private fun rebindSpinner(
        id: Int, entries: List<String>, values: List<String>,
        currentValue: String, onPick: (String) -> Unit
    ) {
        val sp = findViewById<Spinner>(id) ?: return
        sp.onItemSelectedListener = null
        sp.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, entries)
        val idx = values.indexOf(currentValue).coerceAtLeast(0)
        sp.setSelection(idx, false)
        sp.tag = values[idx]
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
    }

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
        val srcW = size.width
        val srcH = size.height
        if (srcW <= 0 || srcH <= 0) return
        lastPreviewSize = size

        if (VulkanRenderer.isReady) {
            // Path Vulkan: la view sta sempre match_parent e la composizione
            // (fit/fill/stretch + rotation + mirror) avviene nel blit GPU.
            VulkanRenderer.setAspect(when (settings.aspect) { "fill" -> 1; "stretch" -> 2; else -> 0 })
            val lp = cameraView.layoutParams as? android.widget.FrameLayout.LayoutParams
            if (lp != null) {
                val needs = lp.width != android.view.ViewGroup.LayoutParams.MATCH_PARENT ||
                    lp.height != android.view.ViewGroup.LayoutParams.MATCH_PARENT ||
                    lp.gravity != android.view.Gravity.CENTER
                if (needs) {
                    lp.gravity = android.view.Gravity.CENTER
                    lp.width = android.view.ViewGroup.LayoutParams.MATCH_PARENT
                    lp.height = android.view.ViewGroup.LayoutParams.MATCH_PARENT
                    cameraView.layoutParams = lp
                    cameraView.requestLayout()
                }
            }
            aspectController.update(cameraView.width, cameraView.height, srcW, srcH)
            return
        }

        // Fallback (Vulkan non disponibile): replico la vecchia logica
        // basata su layoutParams del SurfaceView, senza setAspectRatio
        // (era specifica della libreria UVCAndroid).
        var fW = srcW
        var fH = srcH
        if (settings.rotation == 90 || settings.rotation == 270) {
            val tmp = fW; fW = fH; fH = tmp
        }
        val parent = cameraView.parent as? android.view.View ?: return
        val parentW = parent.width
        val parentH = parent.height
        if (parentW <= 0 || parentH <= 0) return
        val lp = cameraView.layoutParams as? android.widget.FrameLayout.LayoutParams ?: return
        lp.gravity = android.view.Gravity.CENTER
        when (settings.aspect) {
            "stretch" -> {
                lp.width = android.view.ViewGroup.LayoutParams.MATCH_PARENT
                lp.height = android.view.ViewGroup.LayoutParams.MATCH_PARENT
            }
            "fill" -> {
                val parentAspect = parentW.toDouble() / parentH
                val srcAspect = fW.toDouble() / fH
                if (srcAspect > parentAspect) {
                    lp.height = parentH
                    lp.width = (parentH * srcAspect).toInt()
                } else {
                    lp.width = parentW
                    lp.height = (parentW / srcAspect).toInt()
                }
            }
            else -> {
                // fit: ridimensiona la view al massimo rettangolo iso-aspect contenuto
                val parentAspect = parentW.toDouble() / parentH
                val srcAspect = fW.toDouble() / fH
                if (srcAspect > parentAspect) {
                    lp.width = parentW
                    lp.height = (parentW / srcAspect).toInt()
                } else {
                    lp.height = parentH
                    lp.width = (parentH * srcAspect).toInt()
                }
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
            // Catturiamo il valore di default (auto-exposure spento per leggerlo,
            // poi riacceso). Serve come base per la scala EV.
            exposureDef = try {
                val cur = ctrl!!.exposureTimeAbsolute
                if (cur in exposureMin..exposureMax) cur else (exposureMin + exposureMax) / 2
            } catch (_: Exception) { (exposureMin + exposureMax) / 2 }
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
            val ctrl = camera?.uvcControl ?: return
            val evHalf = settings.exposureEvHalf.coerceIn(-12, 12)
            if (evHalf == 0) {
                // EV=0 -> default cam, riabilita auto-exposure.
                try { ctrl.setExposureTimeAuto(true) } catch (_: Exception) {}
                return
            }
            // EV != 0 -> manual, scala 2^(ev/2) rispetto al default.
            try { ctrl.setExposureTimeAuto(false) } catch (_: Exception) {}
            val base = if (exposureDef in exposureMin..exposureMax) exposureDef
                       else (exposureMin + exposureMax) / 2
            val factor = Math.pow(2.0, evHalf / 2.0)
            val raw = (base * factor).toInt().coerceIn(exposureMin, exposureMax)
            ctrl.setExposureTimeAbsolute(raw)
        } catch (_: Exception) {}
    }

    private fun bindEvSeekExposure() {
        evSeekExposure.max = 24  // 0..24 -> ev_half -12..+12
        val initialPos = (settings.exposureEvHalf + 12).coerceIn(0, 24)
        evSeekExposure.progress = initialPos
        evValueExposure.text = formatEv(initialPos - 12)
        evSeekExposure.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar?, progress: Int, fromUser: Boolean) {
                val evHalf = progress - 12
                evValueExposure.text = formatEv(evHalf)
                if (fromUser) {
                    settings.exposureEvHalf = evHalf
                    applyExposure()
                    scheduleHideUi()
                }
            }
            override fun onStartTrackingTouch(sb: SeekBar?) { mainHandler.removeCallbacks(hideUiRunnable) }
            override fun onStopTrackingTouch(sb: SeekBar?) { scheduleHideUi() }
        })
    }

    private fun formatEv(evHalf: Int): String {
        if (evHalf == 0) return "0"
        val sign = if (evHalf > 0) "+" else "-"
        val abs = Math.abs(evHalf)
        val whole = abs / 2
        val half = abs % 2
        return if (half == 0) "$sign$whole" else "$sign$whole.5"
    }

    private fun applyAllImageControls() {
        applyBrightness()
        applyContrast()
        applyExposure()
        // Riabilita esplicitamente l'autofocus: alcune cam UVC lasciano AF
        // disabilitato quando il preview e' guidato da Surface esterna.
        try { camera?.uvcControl?.setFocusAuto(true) } catch (_: Exception) {}
    }

    private fun applyEnhancement() {
        if (!VulkanRenderer.isReady) return
        VulkanRenderer.setEnhancement(
            settings.sharpenPercent,
            settings.denoisePercent,
            settings.defogPercent,
            settings.defogEnabled
        )
    }

    private fun bindEnhSeek(seek: SeekBar, valueText: TextView, initial: Int, onChange: (Int) -> Unit) {
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
            override fun onStartTrackingTouch(sb: SeekBar?) { mainHandler.removeCallbacks(hideUiRunnable) }
            override fun onStopTrackingTouch(sb: SeekBar?) { scheduleHideUi() }
        })
    }

    private fun toggleEnhPanel() {
        if (enhPanel.visibility == View.VISIBLE) {
            enhPanel.visibility = View.GONE
            scheduleHideUi()
        } else {
            // Chiudi l'altro pannello se aperto.
            evPanel.visibility = View.GONE
            enhSeekDefog.progress = settings.defogPercent
            enhValueDefog.text = settings.defogPercent.toString()
            enhSeekSharp.progress = settings.sharpenPercent
            enhValueSharp.text = settings.sharpenPercent.toString()
            enhSeekDenoise.progress = settings.denoisePercent
            enhValueDenoise.text = settings.denoisePercent.toString()
            cbDefogAuto.isChecked = settings.defogEnabled
            enhPanel.visibility = View.VISIBLE
            mainHandler.removeCallbacks(hideUiRunnable)
        }
    }

    private fun resetEnhancement() {
        settings.sharpenPercent = 0
        settings.denoisePercent = 0
        settings.defogPercent = 0
        settings.defogEnabled = false
        runOnUiThread {
            enhSeekDefog.progress = 0; enhValueDefog.text = "0"
            enhSeekSharp.progress = 0; enhValueSharp.text = "0"
            enhSeekDenoise.progress = 0; enhValueDenoise.text = "0"
            cbDefogAuto.isChecked = false
        }
        applyEnhancement()
    }

    private fun toggleEvPanel() {
        if (evPanel.visibility == View.VISIBLE) {
            evPanel.visibility = View.GONE
            scheduleHideUi()
        } else {
            // Tab-mode: chiudi enhPanel se aperto.
            enhPanel.visibility = View.GONE
            evSeekBrightness.progress = settings.brightnessPercent
            evValueBrightness.text = settings.brightnessPercent.toString()
            evSeekContrast.progress = settings.contrastPercent
            evValueContrast.text = settings.contrastPercent.toString()
            val ePos = (settings.exposureEvHalf + 12).coerceIn(0, 24)
            evSeekExposure.progress = ePos
            evValueExposure.text = formatEv(ePos - 12)
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
        // Esposizione: reset = EV 0 (auto-exposure / default cam).
        // Aggiorniamo anche exposureDef leggendo il valore "auto" corrente.
        if (exposureSupported) {
            exposureDef = try {
                val cur = ctrl?.exposureTimeAbsolute ?: exposureDef
                if (cur in exposureMin..exposureMax) cur else exposureDef
            } catch (_: Exception) { exposureDef }
        }
        settings.brightnessPercent = newBrightness
        settings.contrastPercent = newContrast
        settings.exposureEvHalf = 0
        runOnUiThread {
            evSeekBrightness.progress = newBrightness
            evValueBrightness.text = newBrightness.toString()
            evSeekContrast.progress = newContrast
            evValueContrast.text = newContrast.toString()
            evSeekExposure.progress = 12  // 0 EV
            evValueExposure.text = formatEv(0)
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
                    populateDynamicCameraOptions()
                    applyPreviewConfig()
                    rebindSurface()
                    applyAspect()
                    detectImageControlsSupport()
                    // Ad ogni open della cam ripristiniamo i valori di default
                    // hardware (brightness/contrast/exposure) e li scriviamo
                    // in Settings, ignorando la cache della sessione precedente.
                    // Includiamo anche un setFocusAuto(true) per riattivare AF.
                    resetImageControls()
                    // Reset enhancement (compute) ai default off ad ogni open.
                    if (VulkanRenderer.isReady) {
                        resetEnhancement()
                    }
                    try { camera?.uvcControl?.setFocusAuto(true) } catch (_: Exception) {}
                    hideHint()
                    showCamBadge(deviceDisplayName(device ?: selectedDevice))
                    scheduleHideUi()
                }
            }
            override fun onCameraClose(device: UsbDevice?) {
                try { camera?.setFrameCallback(null, 0) } catch (_: Exception) {}
                lastPreviewSize = null
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
        if (!surfaceReady) return
        // Path Vulkan: registriamo un IFrameCallback che riceve i frame YUYV
        // e li passa al native (uploadFrameYUYV + renderFrame). cam.addSurface
        // NON viene chiamato perche' la SurfaceView e' di proprieta' di Vulkan.
        if (VulkanRenderer.isReady) {
            // setFrameCallback richiede che la UVCCamera interna esista, quindi
            // va chiamato SOLO dopo isCameraOpened. Se la cam non e' aperta
            // ancora, segniamo previewAttached=false e rebindSurface verra'
            // richiamato da onCameraOpen.
            if (!cam.isCameraOpened) {
                previewAttached = false
                return
            }
            try {
                try { cam.stopPreview() } catch (_: Exception) {}
                cam.setFrameCallback(vulkanFrameCallback, UVCCamera.PIXEL_FORMAT_RAW)
                cam.startPreview()
                previewAttached = true
            } catch (t: Throwable) {
                android.util.Log.e("MainActivity", "Vulkan frame callback wiring failed", t)
                previewAttached = false
            }
            return
        }
        // Fallback legacy: il preview UVC va direttamente sul SurfaceView.
        val surface = cameraView.holder.surface ?: return
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
        if (VulkanRenderer.isReady) {
            // Vedi nota in rebindSurface(): il path Vulkan non passa il
            // Surface alla camera UVC.
            return
        }
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
