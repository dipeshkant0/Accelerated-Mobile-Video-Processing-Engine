
#include <jni.h>
#include <chrono>
#include <algorithm>
#include <arm_neon.h>

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





extern "C" JNIEXPORT jdouble JNICALL
Java_com_example_videoprocessingengine_MainActivity_processFrameNativeSIMD(
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

        for (int x = 0; x < width; x+=8) { // 8 pixels a time
            // Load 8 Y values
            uint8x8_t y8 = vld1_u8(x + yRow);

            //Load 4 U values and 4 V values and duplicate them to match 8 pixels
            uint8x8_t u4 = vld1_u8(uRow + (x/2)), v4 = vld1_u8((vRow + (x/2)));
            uint8x8_t u8 = vzip1_u8(u4, u4), v8 = vzip1_u8(v4, v4);


            int16x8_t y16 = vreinterpretq_s16_u16(vmovl_u8((y8)));

            int16x8_t v16 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(v8)), vdupq_n_s16(128));
            int16x8_t u16 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(u8)), vdupq_n_s16(128));


            // 4. Calculate (V * 1436) >> 10
            // We must split into two 32-bit halves because 16-bit * 16-bit can exceed 32,767
            int32x4_t r_low = vmull_n_s16(vget_low_s16(v16), 1436);
            int32x4_t r_high = vmull_n_s16(vget_high_s16(v16), 1436);
            // 5. Shift right by 10 and combine back into one 16-bit vector
            int16x8_t r_offset = vcombine_s16(vshrn_n_s32(r_low, 10), vshrn_n_s32(r_high, 10));
            // 6. Add Y and saturating-narrow back to 8-bit (this handles the "clamping")
            uint8x8_t r8 = vqmovun_s16(vreinterpretq_s16_u16(vaddq_s16(vreinterpretq_s16_u16(y16), r_offset)));


            r_low = vmull_n_s16(vget_low_s16(u16), 1814);
            r_high = vmull_n_s16(vget_high_s16(u16), 1814);
            // 5. Shift right by 10 and combine back into one 16-bit vector
            r_offset = vcombine_s16(vshrn_n_s32(r_low, 10), vshrn_n_s32(r_high, 10));
            // 6. Add Y and saturating-narrow back to 8-bit (this handles the "clamping")
            uint8x8_t b8 = vqmovun_s16(vreinterpretq_s16_u16(vaddq_s16(vreinterpretq_s16_u16(y16), r_offset)));


            // 2. Calculate the U part: (U * 352)
            int32x4_t g_u_low = vmull_n_s16(vget_low_s16(u16), 352);
            int32x4_t g_u_high = vmull_n_s16(vget_high_s16(u16), 352);

            // 3. Calculate the V part and add it to the U part: (U * 352 + V * 731)
            // vmlal (Multiply-Accumulate) performs the multiply and add in one step
            int32x4_t g_sum_low = vmlal_n_s16(g_u_low, vget_low_s16(v16), 731);
            int32x4_t g_sum_high = vmlal_n_s16(g_u_high, vget_high_s16(v16), 731);

            // 4. Shift right by 10 and combine back into a single 16-bit vector
            int16x8_t g_offset = vcombine_s16(vshrn_n_s32(g_sum_low, 10), vshrn_n_s32(g_sum_high, 10));

            uint8x8_t g8 = vqmovun_s16(vreinterpretq_s16_u16(vsubq_s16(vreinterpretq_s16_u16(y16), g_offset)));

            uint8x8x4_t rgba;
            rgba.val[0] = r8;
            rgba.val[1] = g8;
            rgba.val[2] = b8;
            rgba.val[3] = vdup_n_u8(255);
            vst4_u8(reinterpret_cast<uint8_t*>(rgbaRow + x), rgba);
        }

    }

    auto end = std::chrono::high_resolution_clock::now();
    return static_cast<jdouble>(std::chrono::duration<double, std::milli>(end - start).count());
}