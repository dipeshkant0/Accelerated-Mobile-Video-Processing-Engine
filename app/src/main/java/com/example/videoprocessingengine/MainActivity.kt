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
            val temp = intent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) ?: 0
            tempInCelsius = temp / 10.0
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        unregisterReceiver(batteryReceiver)
        cameraExecutor.shutdown()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
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

                // Send the frame to C++ based on the selected mode
                val latency = if (currentMode == ProcessingMode.SIMD) {
                    processFrameNativeSIMD(
                        imageProxy.planes[0].buffer,
                        rgbaBuffer!!,
                        width,
                        height,
                        imageProxy.planes[0].rowStride,
                        currentLUT,
                        lutSize
                    )
                } else if (currentMode == ProcessingMode.GPU) {
                    processFrameNativeGPU(
                        imageProxy.planes[0].buffer,
                        rgbaBuffer!!,
                        width,
                        height,
                        imageProxy.planes[0].rowStride,
                        currentLUT,
                        lutSize,
                        displaySurface
                    )
                } else {
                    processFrameNative(
                        imageProxy.planes[0].buffer,
                        rgbaBuffer!!,
                        width,
                        height,
                        imageProxy.planes[0].rowStride,
                        currentLUT,
                        lutSize
                    )
                }

                // Accumulate latency over the interval
                totalLatencyOverInterval += latency
                latencyFrameCount++

                rgbaBuffer!!.rewind()

                if (currentMode != ProcessingMode.GPU) {
                    // Convert the raw returned buffer into a Bitmap so the screen can show it
                    if (reusableBitmap == null || reusableBitmap!!.width != width || reusableBitmap!!.height != height) {
                        reusableBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                    }
                    reusableBitmap!!.copyPixelsFromBuffer(rgbaBuffer!!)
                }

                runOnUiThread {
                    if (currentMode == ProcessingMode.GPU) {
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
                    //update data
                    binding.modeLabel.text = "MODE: ${currentMode.name} | RES: ${targetHeight}p"
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
