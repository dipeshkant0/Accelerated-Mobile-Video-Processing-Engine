
#include <jni.h>
#include <chrono>
#include <algorithm>
#include <arm_neon.h>


//extern "C" JNIEXPORT jdouble JNICALL
//Java_com_example_videoprocessingengine_MainActivity_processFrameNative(JNIEnv* env, jobject,
//                                jobject inRgbaBuf, jobject outRgbaBuf,
//                                jint width, jint height, jint rowStride)
//{
//    auto start = std::chrono::high_resolution_clock::now();
//
//    auto* inData = static_cast<uint8_t*>(env->GetDirectBufferAddress(inRgbaBuf));
//    auto* outData = static_cast<uint8_t*>(env->GetDirectBufferAddress(outRgbaBuf));
//
//    // The output buffer is simply width * height * 4 (RGBA)
//    // The input buffer might have some row padding, so we copy row by row
//    for (int y = 0; y < height; y++) {
//        uint8_t* inRow = inData + (y * rowStride);
//        uint8_t* outRow = outData + (y * width * 4);
//
//        for(int pos = 0;pos<width;pos+=4)
//        {
//            uint8_t r = inRow[pos];
//            uint8_t g = inRow[pos+1];
//            uint8_t b = inRow[pos+2];
//            uint8_t a = inRow[pos+3];
//
//            outRow[pos] = r;
//            outRow[pos+1] = g;
//            outRow[pos+2] = b;
//            outRow[pos+3] = a;
//        }
//    }
//
//    auto end = std::chrono::high_resolution_clock::now();
//    return static_cast<jdouble>(std::chrono::duration<double, std::milli>(end - start).count());
//}
extern "C" JNIEXPORT jdouble JNICALL
Java_com_example_videoprocessingengine_MainActivity_processFrameNative(JNIEnv* env, jobject,
                                                                       jobject inRgbaBuf, jobject outRgbaBuf,
                                                                       jint width, jint height, jint rowStride,
                                                                       jfloatArray lutArray, jint lutSize)
{
    auto start = std::chrono::high_resolution_clock::now();

    // 1. Get memory references from Android
    auto* inData = static_cast<uint8_t*>(env->GetDirectBufferAddress(inRgbaBuf));
    auto* outData = static_cast<uint8_t*>(env->GetDirectBufferAddress(outRgbaBuf));

    // 2. Fetch the LUT Array if the user selected one
    jfloat* lut = nullptr;
    if (lutArray != nullptr && lutSize > 0) {
        lut = env->GetFloatArrayElements(lutArray, nullptr);
    }
    float max_index = static_cast<float>(std::max(lutSize - 1, 1));

    // 3. Process the image pixel by pixel
    for (int y = 0; y < height; y++) {
        uint8_t* inRow = inData + (y * rowStride);
        uint8_t* outRow = outData + (y * width * 4);

        for(int pos = 0; pos < width * 4; pos += 4) {
            uint8_t r_byte = inRow[pos];
            uint8_t a = inRow[pos+3]; // Alpha always stays the same

            if (lut == nullptr) {
                // No LUT selected? Just copy the original camera pixel to the screen
                outRow[pos]   = r_byte;
                outRow[pos+1] = inRow[pos+1];
                outRow[pos+2] = inRow[pos+2];
                outRow[pos+3] = a;
            } else {
                // LUT is active! Find where this color lives inside the 3D Cube.
                float lutX = (r_byte / 255.0f) * max_index;
                float lutY = (inRow[pos+1] / 255.0f) * max_index;
                float lutZ = (inRow[pos+2] / 255.0f) * max_index;

                // Figure out the 8 closest points around our specific color
                int x0 = static_cast<int>(lutX);
                int y0 = static_cast<int>(lutY);
                int z0 = static_cast<int>(lutZ);

                // Make sure we never read past the end of the LUT array
                int x1 = std::min(x0 + 1, lutSize - 1);
                int y1 = std::min(y0 + 1, lutSize - 1);
                int z1 = std::min(z0 + 1, lutSize - 1);

                // How close is our color to each box edge? (Used to mix the final color smoothly)
                float dx = lutX - x0;
                float dy = lutY - y0;
                float dz = lutZ - z0;

                // Grab the 8 neighboring colors from the 1-Dimensional Float Array
                int size2 = lutSize * lutSize;
                int i000 = (z0 * size2 + y0 * lutSize + x0) * 3;
                int i100 = (z0 * size2 + y0 * lutSize + x1) * 3;
                int i010 = (z0 * size2 + y1 * lutSize + x0) * 3;
                int i110 = (z0 * size2 + y1 * lutSize + x1) * 3;
                int i001 = (z1 * size2 + y0 * lutSize + x0) * 3;
                int i101 = (z1 * size2 + y0 * lutSize + x1) * 3;
                int i011 = (z1 * size2 + y1 * lutSize + x0) * 3;
                int i111 = (z1 * size2 + y1 * lutSize + x1) * 3;

                // Mix the colors together (Trilinear Interpolation)
                float finalR =
                        (1 - dz) * ((1 - dy) * ((1 - dx) * lut[i000 + 0] + dx * lut[i100 + 0]) +
                                    dy * ((1 - dx) * lut[i010 + 0] + dx * lut[i110 + 0])) +
                        dz * ((1 - dy) * ((1 - dx) * lut[i001 + 0] + dx * lut[i101 + 0]) +
                              dy * ((1 - dx) * lut[i011 + 0] + dx * lut[i111 + 0]));

                float finalG =
                        (1 - dz) * ((1 - dy) * ((1 - dx) * lut[i000 + 1] + dx * lut[i100 + 1]) +
                                    dy * ((1 - dx) * lut[i010 + 1] + dx * lut[i110 + 1])) +
                        dz * ((1 - dy) * ((1 - dx) * lut[i001 + 1] + dx * lut[i101 + 1]) +
                              dy * ((1 - dx) * lut[i011 + 1] + dx * lut[i111 + 1]));

                float finalB =
                        (1 - dz) * ((1 - dy) * ((1 - dx) * lut[i000 + 2] + dx * lut[i100 + 2]) +
                                    dy * ((1 - dx) * lut[i010 + 2] + dx * lut[i110 + 2])) +
                        dz * ((1 - dy) * ((1 - dx) * lut[i001 + 2] + dx * lut[i101 + 2]) +
                              dy * ((1 - dx) * lut[i011 + 2] + dx * lut[i111 + 2]));

                // Write the newly filtered color to the screen!
                outRow[pos]   = static_cast<uint8_t>(std::min(std::max(finalR * 255.0f, 0.0f), 255.0f));
                outRow[pos+1] = static_cast<uint8_t>(std::min(std::max(finalG * 255.0f, 0.0f), 255.0f));
                outRow[pos+2] = static_cast<uint8_t>(std::min(std::max(finalB * 255.0f, 0.0f), 255.0f));
                outRow[pos+3] = a;
            }
        }
    }

    // Clean up memory
    if (lut != nullptr) {
        env->ReleaseFloatArrayElements(lutArray, lut, JNI_ABORT);
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