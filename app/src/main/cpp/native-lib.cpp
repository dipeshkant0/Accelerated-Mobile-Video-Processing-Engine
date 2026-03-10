
#include <jni.h>
#include <chrono>
#include <algorithm>
#include <arm_neon.h>


extern "C" JNIEXPORT jdouble JNICALL
Java_com_example_videoprocessingengine_MainActivity_processFrameNative(JNIEnv* env, jobject,
                                jobject inRgbaBuf, jobject outRgbaBuf,
                                jint width, jint height, jint rowStride)
{
    auto start = std::chrono::high_resolution_clock::now();

    auto* inData = static_cast<uint8_t*>(env->GetDirectBufferAddress(inRgbaBuf));
    auto* outData = static_cast<uint8_t*>(env->GetDirectBufferAddress(outRgbaBuf));

    // The output buffer is simply width * height * 4 (RGBA)
    // The input buffer might have some row padding, so we copy row by row
    for (int y = 0; y < height; y++) {
        uint8_t* inRow = inData + (y * rowStride);
        uint8_t* outRow = outData + (y * width * 4);
        
        // Copy 4 bytes per pixel (RGBA)
        std::copy(inRow, inRow + (width * 4), outRow);
        for(int pos = 0;pos<width;pos+=4)
        {
            uint8_t r = inRow[pos];
            uint8_t g = inRow[pos+1];
            uint8_t b = inRow[pos+2];
            uint8_t a = inRow[pos+3];

            outRow[pos] = r;
            outRow[pos+1] = g;
            outRow[pos+2] = b;
            outRow[pos+3] = a;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    return static_cast<jdouble>(std::chrono::duration<double, std::milli>(end - start).count());
}





extern "C" JNIEXPORT jdouble JNICALL
Java_com_example_videoprocessingengine_MainActivity_processFrameNativeSIMD(JNIEnv* env, jobject,
                                    jobject inRgbaBuf, jobject outRgbaBuf,
                                    jint width, jint height, jint rowStride)
{
    auto start = std::chrono::high_resolution_clock::now();

    auto* inData = static_cast<uint8_t*>(env->GetDirectBufferAddress(inRgbaBuf));
    auto* outData = static_cast<uint8_t*>(env->GetDirectBufferAddress(outRgbaBuf));

    for (int y = 0; y < height; y++) {
        uint8_t* inRow = inData + (y * rowStride);
        uint8_t* outRow = outData + (y * width * 4);
        
        // For SIMD, we can load and store 16 bytes (4 pixels of RGBA) at a time
        for (int x = 0; x < width * 4; x += 16) {
            uint8x16_t pixels = vld1q_u8(inRow + x);
            vst1q_u8(outRow + x, pixels);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    return static_cast<jdouble>(std::chrono::duration<double, std::milli>(end - start).count());
}