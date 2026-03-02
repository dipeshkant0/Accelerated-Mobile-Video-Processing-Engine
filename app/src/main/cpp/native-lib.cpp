#include <jni.h>
#include <chrono>
#include <algorithm>

// Faster integer-based clamp
inline uint8_t clamp_int(int val) {
    return (uint8_t)((val > 255) ? 255 : (val < 0 ? 0 : val));
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_example_videoprocessingengine_MainActivity_processFrameNative(
        JNIEnv* env, jobject, jobject yBuf, jobject uBuf, jobject vBuf,
        jobject rgbaBuf, jint width, jint height,
        jint yStride, jint uvRowStride, jint uvPixelStride) {

    auto start = std::chrono::high_resolution_clock::now();

    auto* yData = static_cast<uint8_t*>(env->GetDirectBufferAddress(yBuf));
    auto* uData = static_cast<uint8_t*>(env->GetDirectBufferAddress(uBuf));
    auto* vData = static_cast<uint8_t*>(env->GetDirectBufferAddress(vBuf));
    auto* rgbaData = static_cast<uint32_t*>(env->GetDirectBufferAddress(rgbaBuf));

    for (int y = 0; y < height; y++) {
        uint32_t* rgbaRow = rgbaData + (y * width);
        uint8_t* yRow = yData + (y * yStride);

        // Optimization: Pre-calculate the UV row start
        uint8_t* uRow = uData + (y / 2) * uvRowStride;
        uint8_t* vRow = vData + (y / 2) * uvRowStride;

        for (int x = 0; x < width; x++) {
            int Y = yRow[x];
            int uvIdx = (x / 2) * uvPixelStride;
            int U = uRow[uvIdx] - 128;
            int V = vRow[uvIdx] - 128;

            // Formulas based on standard YUV conversion constants
            int r = Y + ((V * 1436) >> 10);
            int g = Y - ((U * 352 + V * 731) >> 10);
            int b = Y + ((U * 1814) >> 10);


            // ABGR format for Android Bitmaps (Little Endian)
            rgbaRow[x] = (0xFF << 24) | (clamp_int(b) << 16) | (clamp_int(g) << 8) | clamp_int(r);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    return static_cast<jdouble>(std::chrono::duration<double, std::milli>(end - start).count());
}