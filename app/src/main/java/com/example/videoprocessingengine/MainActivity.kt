package com.example.videoprocessingengine

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Matrix
import android.graphics.SurfaceTexture
import android.os.BatteryManager
import androidx.appcompat.app.AppCompatActivity
import android.os.Bundle
import android.view.Surface
import android.view.TextureView
import android.view.WindowManager
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.core.content.ContextCompat
import com.example.videoprocessingengine.databinding.ActivityMainBinding
import java.nio.ByteBuffer
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import android.util.Log
import android.util.Size
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.core.graphics.createBitmap


class MainActivity : AppCompatActivity() {
    private lateinit var binding: ActivityMainBinding
    private var rgbaBuffer: ByteBuffer? = null

    private var reusableBitmap: Bitmap? = null
    private var lastFrameTimestamp = 0L
    private var frameCount = 0
    private var fps = 0.0
    private var cpuUsage = 0.0
    private var memoryUsage = 0
    private var lastCpuTime = 0L
    private var lastRealTime = 0L
    private var tempInCelsius = 0.0
    private var totalLatencyOverInterval = 0.0
    private var latencyFrameCount = 0
    private var displayLatency = 0.0
    
    private var batPct = 100
    private var lastLat = 0.0

    enum class ProcessingMode { BASELINE, SIMD, GPU, HYBRID }
    private var curMode = ProcessingMode.BASELINE

    private var lutTeal: FloatArray? = null
    private var lutBW: FloatArray? = null
    private var lutNight: FloatArray? = null
    private var lutThermal: FloatArray? = null
    private var sTeal: Int = 0
    private var sBW: Int = 0
    private var sNight: Int = 0
    private var sThermal: Int = 0
    private var lutArr: FloatArray? = null
    private var lSize: Int = 0
    private var tW = 1280
    private var tH = 720
    private lateinit var camExec: ExecutorService
    private var surf: Surface? = null

    // frame processing function in c++
    external fun processFrameNative(inRgba: ByteBuffer, outRgbaBuf: ByteBuffer, width: Int, height: Int, rowStride: Int, currentLUT: FloatArray?, lutSize: Int): Double
    external fun processFrameNativeSIMD(inRgba: ByteBuffer, outRgbaBuf: ByteBuffer, width: Int, height: Int, rowStride: Int, currentLUT: FloatArray?, lutSize: Int): Double
    external fun processFrameNativeGPU(inRgba: ByteBuffer, outRgbaBuf: ByteBuffer, width: Int, height: Int, rowStride: Int, currentLUT: FloatArray?, lutSize: Int, surface: Surface?): Double
    
    private val batteryReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            // Get the temperature
            val temp = intent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) ?: 0
            tempInCelsius = temp / 10.0
            
            // Get the battery percentage
            val level = intent?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
            val scale = intent?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1
            if (level != -1 && scale != -1) {
                batPct = (level * 100 / scale.toFloat()).toInt()
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        unregisterReceiver(batteryReceiver)
        camExec.shutdown()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        // Keep the screen from turning off while processing video
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        val filter = IntentFilter(Intent.ACTION_BATTERY_CHANGED)
        val batteryStatus = registerReceiver(batteryReceiver, filter)
        
        // Initialize values from the current battery status
        batteryStatus?.let { intent ->
            val temp = intent.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0)
            tempInCelsius = temp / 10.0
            val level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
            val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
            if (level != -1 && scale != -1) {
                batPct = (level * 100 / scale.toFloat()).toInt()
            }
        }

        // Preload all available LUTs exactly once when the app opens
        val bwLutData = LutParser.parseCubeFile(this, "LUTs/BlackAndWhiteLUT.cube")
        if (bwLutData != null) {
            lutBW = bwLutData.data
            sBW = bwLutData.size
        }

        val tealOrangeLutData = LutParser.parseCubeFile(this, "LUTs/TealOrangeLUT.cube")
        if (tealOrangeLutData != null) {
            lutTeal = tealOrangeLutData.data
            sTeal = tealOrangeLutData.size
        }

        val nightVisionLutData = LutParser.parseCubeFile(this, "LUTs/Night Vision.cube")
        if (nightVisionLutData != null) {
            lutNight = nightVisionLutData.data
            sNight = nightVisionLutData.size
        }

        val thermalLutData = LutParser.parseCubeFile(this, "LUTs/Thermal.cube")
        if (thermalLutData != null) {
            lutThermal = thermalLutData.data
            sThermal = thermalLutData.size
        }

        // Set the default LUT
        lutArr = null

        camExec = Executors.newSingleThreadExecutor()

        if (allPermissionsGranted()) {
            startCamera()
        } else {
            requestPermissionLauncher.launch(Manifest.permission.CAMERA)
        }

        binding.gpuTextureView.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(st: SurfaceTexture, width: Int, height: Int) {
                surf = Surface(st)
            }
            override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, width: Int, height: Int) {
                surf = Surface(st)
            }
            override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
                surf?.release()
                surf = null
                return true
            }
            override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
        }
    }
    override fun onCreateOptionsMenu(menu: android.view.Menu): Boolean {
        menuInflater.inflate(R.menu.main_menu, menu)
        return true
    }

    // Handle menu clicks
    override fun onOptionsItemSelected(item: android.view.MenuItem): Boolean {
          item.isChecked = true

        when (item.itemId) {
            R.id.mode_baseline -> {
                curMode = ProcessingMode.BASELINE
            }
            R.id.mode_simd -> {
                curMode = ProcessingMode.SIMD
            }
            R.id.mode_gpu -> {
                curMode = ProcessingMode.GPU
            }
            R.id.mode_hybrid -> {
                curMode = ProcessingMode.HYBRID
            }
            R.id.res_720p -> {
                tW = 1280
                tH = 720
                restartCamera()
            }
            R.id.res_1080p -> {
                tW = 1920
                tH = 1080
                restartCamera()
            }
            R.id.teal_orange -> {
                lutArr = lutTeal
                lSize = sTeal
            }
            R.id.black_white -> {
                lutArr = lutBW
                lSize = sBW
            }
            R.id.night_vision -> {
                lutArr = lutNight
                lSize = sNight
            }
            R.id.thermal -> {
                lutArr = lutThermal
                lSize = sThermal
            }
            R.id.no_lut -> {
                lutArr = null
                lSize = 0
            }
        }
        return true
    }

    private fun restartCamera() {
        val cameraProviderFuture = ProcessCameraProvider.getInstance(this)
        cameraProviderFuture.get().unbindAll()
        startCamera()
    }
    private fun startCamera() {
        val cameraProviderFuture = ProcessCameraProvider.getInstance(this)
        cameraProviderFuture.addListener({
            val cameraProvider = cameraProviderFuture.get()
                
            val imageAnalysis = ImageAnalysis.Builder()
                .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                .setOutputImageFormat(ImageAnalysis.OUTPUT_IMAGE_FORMAT_RGBA_8888)
                .setResolutionSelector(
                    ResolutionSelector.Builder()
                        .setResolutionStrategy(
                            ResolutionStrategy(
                                Size(tW, tH),
                                ResolutionStrategy.FALLBACK_RULE_CLOSEST_HIGHER_THEN_LOWER
                            )
                        ).build()
                ).build()
            imageAnalysis.setAnalyzer(camExec) { imageProxy ->
                val width = imageProxy.width
                val height = imageProxy.height

                val currentTimestamp = System.currentTimeMillis()
                val currentCpuTime = android.os.Debug.threadCpuTimeNanos()

                if (lastRealTime == 0L) {
                    lastRealTime = currentTimestamp
                    lastCpuTime = currentCpuTime
                    lastFrameTimestamp = currentTimestamp
                }

                if (currentTimestamp - lastFrameTimestamp >= 1000) {
                    val timeInterval = currentTimestamp - lastFrameTimestamp
                    fps = (frameCount * 1000.0) / timeInterval

                    if (latencyFrameCount > 0) {
                        displayLatency = totalLatencyOverInterval / latencyFrameCount
                    }
                    totalLatencyOverInterval = 0.0
                    latencyFrameCount = 0

                    val cpuDiff = currentCpuTime - lastCpuTime
                    val realDiff = timeInterval * 1_000_000L
                    cpuUsage = (cpuDiff.toDouble() / realDiff.toDouble()) * 100.0

                    val memInfo = android.app.ActivityManager.MemoryInfo()
                    val actManager = getSystemService(ACTIVITY_SERVICE) as android.app.ActivityManager
                    actManager.getMemoryInfo(memInfo)
                    val pid = android.os.Process.myPid()
                    val processMemoryInfo = actManager.getProcessMemoryInfo(intArrayOf(pid))[0]
                    memoryUsage = processMemoryInfo.totalPss / 1024

                    frameCount = 0
                    lastFrameTimestamp = currentTimestamp
                    lastCpuTime = currentCpuTime
                    lastRealTime = currentTimestamp
                }

                if (rgbaBuffer == null || rgbaBuffer!!.capacity() < width * height * 4) {
                    rgbaBuffer = ByteBuffer.allocateDirect(width * height * 4)
                }

                var mode = curMode
                if (curMode == ProcessingMode.HYBRID) {
                    // Switch to SIMD if temperature > 40C or battery < 15% to save power and reduce heat
                    mode = if (tempInCelsius < 40.0 && batPct > 15) ProcessingMode.GPU else ProcessingMode.SIMD
                }

                val lat = when (mode) {
                    ProcessingMode.SIMD -> processFrameNativeSIMD(imageProxy.planes[0].buffer, rgbaBuffer!!, width, height, imageProxy.planes[0].rowStride, lutArr, lSize)
                    ProcessingMode.GPU -> processFrameNativeGPU(imageProxy.planes[0].buffer, rgbaBuffer!!, width, height, imageProxy.planes[0].rowStride, lutArr, lSize, surf)
                    else -> processFrameNative(imageProxy.planes[0].buffer, rgbaBuffer!!, width, height, imageProxy.planes[0].rowStride, lutArr, lSize)
                }
                
                lastLat = lat
                totalLatencyOverInterval += lat
                latencyFrameCount++

                rgbaBuffer!!.rewind()

                if (mode != ProcessingMode.GPU) {
                    if (reusableBitmap == null || reusableBitmap!!.width != width || reusableBitmap!!.height != height) {
                        reusableBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                    }
                    reusableBitmap!!.copyPixelsFromBuffer(rgbaBuffer!!)
                }

                runOnUiThread {
                    if (mode == ProcessingMode.GPU) {
                        binding.gpuTextureView.visibility = android.view.View.VISIBLE
                        binding.processedImageView.visibility = android.view.View.GONE
                    } else {
                        binding.gpuTextureView.visibility = android.view.View.GONE
                        binding.processedImageView.visibility = android.view.View.VISIBLE
                        val matrix = Matrix()
                        matrix.postRotate(90f)
                        val rotatedBitmap = Bitmap.createBitmap(reusableBitmap!!, 0, 0, width, height, matrix, false)
                        binding.processedImageView.setImageBitmap(rotatedBitmap)
                    }
                    frameCount++
                    
                    val name = if (curMode == ProcessingMode.HYBRID) "HYBRID (${mode.name})" else curMode.name
                    binding.modeLabel.text = "MODE: $name | RES: ${tH}p"
                    binding.latencyVal.text = String.format("LAT: %.1f ms", displayLatency)
                    binding.fpsVal.text = String.format("FPS: %.1f", fps)
                    binding.cpuVal.text = String.format("CPU: %.0f%%", cpuUsage)
                    binding.memVal.text = "MEM: ${memoryUsage}MB"
                    binding.tempVal.text = String.format("THERMAL: %.1f°C", tempInCelsius)
                }
                imageProxy.close()
            }

            try {
                cameraProvider.unbindAll()
                cameraProvider.bindToLifecycle(this, CameraSelector.DEFAULT_BACK_CAMERA, imageAnalysis)
            } catch (exc: Exception) {
                Log.e("CameraApp", "Binding failed", exc)
            }
        }, ContextCompat.getMainExecutor(this))
    }

    private val requestPermissionLauncher = registerForActivityResult(ActivityResultContracts.RequestPermission()) { if (it) startCamera() }
    private fun allPermissionsGranted() = ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

    companion object {
        init {
            System.loadLibrary("videoprocessingengine")
        }
    }
}
