package com.example.videoprocessingengine

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Bitmap
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
    private lateinit var cameraExecutor: ExecutorService

    // This is your bridge to the C++ frame processor
    // 1. Update the declaration to match your 10-argument call
    external fun processFrameNative(
        y: ByteBuffer, u: ByteBuffer, v: ByteBuffer,
        rgba: ByteBuffer, width: Int, height: Int,
        yStride: Int, uvRowStride: Int, uvPixelStride: Int
    ): Double

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

    private fun startCamera() {
        val cameraProviderFuture = ProcessCameraProvider.getInstance(this)
        cameraProviderFuture.addListener(
            {
            val cameraProvider = cameraProviderFuture.get()

            // 1. Define Image Analysis (We skip 'Preview' entirely)
            val imageAnalysis = ImageAnalysis.Builder()
                .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                // Force 720p or 1080p for a sharper look
                .setTargetResolution(android.util.Size(1280, 720))
                .build()

                imageAnalysis.setAnalyzer(cameraExecutor) { imageProxy ->
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


                if (rgbaBuffer == null) {
                    rgbaBuffer = ByteBuffer.allocateDirect(width * height * 4)
                }
                val latency = processFrameNative(
                    imageProxy.planes[0].buffer,
                    imageProxy.planes[1].buffer,
                    imageProxy.planes[2].buffer,
                    rgbaBuffer!!,
                    width,
                    height,
                    imageProxy.planes[0].rowStride,
                    imageProxy.planes[1].rowStride, // Both U and V use this
                    imageProxy.planes[1].pixelStride
                )

                rgbaBuffer!!.rewind()
                val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                bitmap.copyPixelsFromBuffer(rgbaBuffer!!)

                runOnUiThread {
                    binding.processedImageView.setImageBitmap(bitmap)
                    frameCount++ // Increment after successful display
                    binding.latencyText.text = String.format("---- Baseline ----\nLatency: %.2f ms\nFPS: %.2f\nCPU: %.1f%%\nMemory Usage: %d MB", latency, fps, cpuUsage, memoryUsage)
                }
                imageProxy.close()
            }

            try {
                cameraProvider.unbindAll()
                // 5. Bind ONLY imageAnalysis
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