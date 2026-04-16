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
    
    // Variables for my Hybrid mode logic
    private var batteryPercent = 100
    private var lastLatencyValue = 0.0

    // Dynamic Calibration Variables
    private enum class CalibrationState { IDLE, PROFILING_BASELINE, PROFILING_SIMD, PROFILING_GPU, READY }
    private var calibrationState = CalibrationState.IDLE
    private var profilingFrameCounter = 0
    private val FRAMES_PER_MODE = 15
    private val WARMUP_FRAMES = 5 // Skip first few frames for cache/thermal stability
    private var avgBaselineLatency = 0.0
    private var avgSimdLatency = 0.0
    private var avgGpuLatency = 0.0

    enum class ProcessingMode { BASELINE, SIMD, GPU, HYBRID }
    private var currentMode = ProcessingMode.BASELINE

    private var tealOrangeLUT: FloatArray? = null
    private var blackWhiteLUT: FloatArray? = null
    private var lutSizeteal: Int = 0
    private var lutSizebw: Int = 0
    private var currentLUT: FloatArray? = null
    private var lutSize: Int = 0
    private var targetWidth = 1280
    private var targetHeight = 720
    private lateinit var cameraExecutor: ExecutorService
    private var displaySurface: Surface? = null

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
                batteryPercent = (level * 100 / scale.toFloat()).toInt()
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        unregisterReceiver(batteryReceiver)
        cameraExecutor.shutdown()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        // Keep the screen from turning off while processing video
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        val filter = IntentFilter(Intent.ACTION_BATTERY_CHANGED)
        registerReceiver(batteryReceiver, filter)

        // Preload all available LUTs exactly once when the app opens
        val bwLutData = LutParser.parseCubeFile(this, "LUTs/BlackAndWhiteLUT.cube")
        if (bwLutData != null) {
            blackWhiteLUT = bwLutData.data
            lutSizebw = bwLutData.size
        }

        val tealOrangeLutData = LutParser.parseCubeFile(this, "LUTs/TealOrangeLUT.cube")
        if (tealOrangeLutData != null) {
            tealOrangeLUT = tealOrangeLutData.data
            lutSizeteal = tealOrangeLutData.size
        }

        // Set the default LUT
        currentLUT = null

        cameraExecutor = Executors.newSingleThreadExecutor()

        if (allPermissionsGranted()) {
            startCamera()
        } else {
            requestPermissionLauncher.launch(Manifest.permission.CAMERA)
        }

        binding.gpuTextureView.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(st: SurfaceTexture, width: Int, height: Int) {
                displaySurface = Surface(st)
            }
            override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, width: Int, height: Int) {
                displaySurface = Surface(st)
            }
            override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
                displaySurface?.release()
                displaySurface = null
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
                currentMode = ProcessingMode.BASELINE
            }
            R.id.mode_simd -> {
                currentMode = ProcessingMode.SIMD
            }
            R.id.mode_gpu -> {
                currentMode = ProcessingMode.GPU
            }
            R.id.mode_hybrid -> {
                currentMode = ProcessingMode.HYBRID
                calibrationState = CalibrationState.IDLE
            }
            R.id.res_720p -> {
                targetWidth = 1280
                targetHeight = 720
                restartCamera()
            }
            R.id.res_1080p -> {
                targetWidth = 1920
                targetHeight = 1080
                restartCamera()
            }
            R.id.teal_orange -> {
                currentLUT = tealOrangeLUT
                lutSize = lutSizeteal
            }
            R.id.black_white -> {
                currentLUT = blackWhiteLUT
                lutSize = lutSizebw
            }
            R.id.no_lut -> {
                currentLUT = null
                lutSize = 0
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
                                Size(targetWidth, targetHeight),
                                ResolutionStrategy.FALLBACK_RULE_CLOSEST_HIGHER_THEN_LOWER
                            )
                        ).build()
                ).build()
            imageAnalysis.setAnalyzer(cameraExecutor) { imageProxy ->
                val width = imageProxy.width
                val height = imageProxy.height

                // 1. Battery Temperature is now updated via BroadcastReceiver
                
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

                    // Average the latency over the frames processed in the last second
                    if (latencyFrameCount > 0) {
                        displayLatency = totalLatencyOverInterval / latencyFrameCount
                    }
                    totalLatencyOverInterval = 0.0
                    latencyFrameCount = 0

                    val cpuDiff = currentCpuTime - lastCpuTime
                    val realDiff = timeInterval * 1_000_000L
                    cpuUsage = (cpuDiff.toDouble() / realDiff.toDouble()) * 100.0

                    // Fetch Memory data
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


                // Initialize memory for the frame if it doesn't exist yet
                if (rgbaBuffer == null || rgbaBuffer!!.capacity() < width * height * 4) {
                    rgbaBuffer = ByteBuffer.allocateDirect(width * height * 4)
                }

                // Decide which mode to use if we are in Hybrid mode
                var modeToUseRightNow = currentMode
                if (currentMode == ProcessingMode.HYBRID) {
                    when (calibrationState) {
                        CalibrationState.IDLE -> {
                            calibrationState = CalibrationState.PROFILING_BASELINE
                            profilingFrameCounter = 0
                            avgBaselineLatency = 0.0
                            modeToUseRightNow = ProcessingMode.BASELINE
                        }
                        CalibrationState.PROFILING_BASELINE -> {
                            modeToUseRightNow = ProcessingMode.BASELINE
                            profilingFrameCounter++
                            // Skip the first few frames (warm-up)
                            if (profilingFrameCounter > WARMUP_FRAMES) {
                                avgBaselineLatency += lastLatencyValue
                                // If we've collected enough stable frames
                                if (profilingFrameCounter >= (WARMUP_FRAMES + FRAMES_PER_MODE)) {
                                    avgBaselineLatency /= FRAMES_PER_MODE
                                    calibrationState = CalibrationState.PROFILING_SIMD
                                    profilingFrameCounter = 0
                                    avgSimdLatency = 0.0
                                }
                            }
                        }
                        CalibrationState.PROFILING_SIMD -> {
                            modeToUseRightNow = ProcessingMode.SIMD
                            profilingFrameCounter++
                            if (profilingFrameCounter > WARMUP_FRAMES) {
                                avgSimdLatency += lastLatencyValue
                                if (profilingFrameCounter >= (WARMUP_FRAMES + FRAMES_PER_MODE)) {
                                    avgSimdLatency /= FRAMES_PER_MODE
                                    calibrationState = CalibrationState.PROFILING_GPU
                                    profilingFrameCounter = 0
                                    avgGpuLatency = 0.0
                                }
                            }
                        }
                        CalibrationState.PROFILING_GPU -> {
                            modeToUseRightNow = ProcessingMode.GPU
                            profilingFrameCounter++
                            if (profilingFrameCounter > WARMUP_FRAMES) {
                                avgGpuLatency += lastLatencyValue
                                if (profilingFrameCounter >= (WARMUP_FRAMES + FRAMES_PER_MODE)) {
                                    avgGpuLatency /= FRAMES_PER_MODE
                                    calibrationState = CalibrationState.READY
                                    profilingFrameCounter = 0
                                }
                            }
                        }
                        CalibrationState.READY -> {
                            // PERFORMANCE-FIRST HYBRID LOGIC (Restored)
                            
                            // 1. Thermal Emergency (over 40 degrees) - Use the calibrated "Safe Mode"
                            if (tempInCelsius > 40.0) {
                                modeToUseRightNow = ProcessingMode.BASELINE
                            }
                            // 2. Battery Low - Use the calibrated "Efficient Mode" (SIMD on your phone)
                            else if (batteryPercent < 20) {
                                modeToUseRightNow = ProcessingMode.SIMD
                            }
                            // 3. Performance Check - If latency > 15% slower than the GPU benchmark
                            else if (lastLatencyValue > (avgGpuLatency * 1.15)) {
                                // If GPU is slowing down (thermal throttling?), fall back to SIMD
                                modeToUseRightNow = ProcessingMode.SIMD
                            }
                            // 4. Default - Use the winner from our benchmark (GPU on your phone)
                            else {
                                modeToUseRightNow = if (avgGpuLatency < avgSimdLatency) 
                                    ProcessingMode.GPU else ProcessingMode.SIMD
                            }
                        }
                    }
                }

                // Send the frame to C++ based on the selected mode
                val latency = when (modeToUseRightNow) {
                    ProcessingMode.SIMD -> processFrameNativeSIMD(
                        imageProxy.planes[0].buffer,
                        rgbaBuffer!!,
                        width,
                        height,
                        imageProxy.planes[0].rowStride,
                        currentLUT,
                        lutSize
                    )
                    ProcessingMode.GPU -> processFrameNativeGPU(
                        imageProxy.planes[0].buffer,
                        rgbaBuffer!!,
                        width,
                        height,
                        imageProxy.planes[0].rowStride,
                        currentLUT,
                        lutSize,
                        displaySurface
                    )
                    else -> processFrameNative(
                        imageProxy.planes[0].buffer,
                        rgbaBuffer!!,
                        width,
                        height,
                        imageProxy.planes[0].rowStride,
                        currentLUT,
                        lutSize
                    )
                }
                
                // Save the latency so we can use it for the next frame in Hybrid mode
                lastLatencyValue = latency

                // Accumulate latency over the interval
                totalLatencyOverInterval += latency
                latencyFrameCount++

                rgbaBuffer!!.rewind()

                if (modeToUseRightNow != ProcessingMode.GPU) {
                    // Convert the raw returned buffer into a Bitmap so the screen can show it
                    if (reusableBitmap == null || reusableBitmap!!.width != width || reusableBitmap!!.height != height) {
                        reusableBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                    }
                    reusableBitmap!!.copyPixelsFromBuffer(rgbaBuffer!!)
                }

                runOnUiThread {
                    if (modeToUseRightNow == ProcessingMode.GPU) {
                        binding.gpuTextureView.visibility = android.view.View.VISIBLE
                        binding.processedImageView.visibility = android.view.View.GONE
                    } else {
                        binding.gpuTextureView.visibility = android.view.View.GONE
                        binding.processedImageView.visibility = android.view.View.VISIBLE

                        // Rotate the bitmap for portrait display
                        val matrix = Matrix()
                        matrix.postRotate(90f)
                        val rotatedBitmap = Bitmap.createBitmap(reusableBitmap!!, 0, 0, width, height, matrix, false)
                        binding.processedImageView.setImageBitmap(rotatedBitmap)
                    }
                    frameCount++
                    
                    // Show the mode name, and if it is Hybrid, show what it is actually doing
                    val modeDisplayName = if (currentMode == ProcessingMode.HYBRID) {
                        when (calibrationState) {
                            CalibrationState.READY -> "HYBRID (READY: ${modeToUseRightNow.name})"
                            CalibrationState.IDLE -> "HYBRID (PREPARING...)"
                            else -> "HYBRID (PROFILING ${modeToUseRightNow.name}...)"
                        }
                    } else {
                        currentMode.name
                    }
                    
                    binding.modeLabel.text = "MODE: $modeDisplayName | RES: ${targetHeight}p"
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
