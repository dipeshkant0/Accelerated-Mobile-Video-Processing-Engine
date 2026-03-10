package com.example.videoprocessingengine

import android.Manifest
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Matrix
import android.os.BatteryManager
import androidx.appcompat.app.AppCompatActivity
import android.os.Bundle
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
    private var lastFrameTimestamp = 0L
    private var frameCount = 0
    private var fps = 0.0
    private var cpuUsage = 0.0
    private var memoryUsage = 0
    private var lastCpuTime = 0L
    private var lastRealTime = 0L
    private var tempInCelsius = 0.0

    enum class ProcessingMode { BASELINE, SIMD, GPU, HYBRID }
    private var currentMode = ProcessingMode.BASELINE
    private var targetWidth = 1280
    private var targetHeight = 720
    private lateinit var cameraExecutor: ExecutorService
    
    // frame processing function in c++
    external fun processFrameNative(inRgba: ByteBuffer, outRgba: ByteBuffer, width: Int, height: Int, rowStride: Int): Double
    external fun processFrameNativeSIMD(inRgba: ByteBuffer, outRgba: ByteBuffer, width: Int, height: Int, rowStride: Int): Double

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        cameraExecutor = Executors.newSingleThreadExecutor()

        if (allPermissionsGranted()) {
            startCamera()
        } else {
            requestPermissionLauncher.launch(Manifest.permission.CAMERA)
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
                        )
                        .build()
                )

                .build()

            imageAnalysis.setAnalyzer(cameraExecutor) { imageProxy ->
                val width = imageProxy.width
                val height = imageProxy.height

                //Fetch Temperature
                val intentFilter = IntentFilter(Intent.ACTION_BATTERY_CHANGED)
                val batteryStatus = registerReceiver(null, intentFilter)
                val temp = batteryStatus?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) ?: 0
                tempInCelsius = temp / 10.0

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


                if (rgbaBuffer == null || rgbaBuffer!!.capacity() < width * height * 4) {
                    rgbaBuffer = ByteBuffer.allocateDirect(width * height * 4)
                }

                val latency = if(currentMode == ProcessingMode.SIMD)
                {
                    processFrameNativeSIMD(
                        imageProxy.planes[0].buffer,
                        rgbaBuffer!!,
                        width,
                        height,
                        imageProxy.planes[0].rowStride
                    )
                }
                else if (currentMode == ProcessingMode.BASELINE)
                {
                    processFrameNative(
                        imageProxy.planes[0].buffer,
                        rgbaBuffer!!,
                        width,
                        height,
                        imageProxy.planes[0].rowStride
                    )
                }
                else if(currentMode == ProcessingMode.GPU)
                {
                    processFrameNative(
                        imageProxy.planes[0].buffer,
                        rgbaBuffer!!,
                        width,
                        height,
                        imageProxy.planes[0].rowStride
                    )
                }
                else{
                    processFrameNative(
                        imageProxy.planes[0].buffer,
                        rgbaBuffer!!,
                        width,
                        height,
                        imageProxy.planes[0].rowStride
                    )
                }

                rgbaBuffer!!.rewind()

                val bitmap = createBitmap(width, height)
                bitmap.copyPixelsFromBuffer(rgbaBuffer!!)

                val matrix = Matrix().apply { postRotate(90f) }
                val rotatedBitmap = Bitmap.createBitmap(bitmap, 0, 0, width, height, matrix, false)


                runOnUiThread {
                    // render Frame
                    binding.processedImageView.setImageBitmap(rotatedBitmap)
                    frameCount++
                    //update data
                    binding.modeLabel.text = "MODE: ${currentMode.name} | RES: ${targetHeight}p"
                    binding.latencyVal.text = String.format("LAT: %.1f ms", latency)
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
        init { System.loadLibrary("videoprocessingengine") }
    }
}
