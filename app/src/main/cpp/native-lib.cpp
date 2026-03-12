
#include <jni.h>
#include <chrono>
#include <algorithm>
#include <arm_neon.h>
#include <thread>
#include <vector>


extern "C" JNIEXPORT jdouble JNICALL
Java_com_example_videoprocessingengine_MainActivity_processFrameNative(JNIEnv* env, jobject,jobject inRgbaBuf, jobject outRgbaBuf,jint width, jint height, jint rowStride, jfloatArray lutArray, jint lutSize)
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

        for(int pos = 0; pos < width * 4; pos += 4)  {
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
                float finalR = (1 - dz) * ((1 - dy) * ((1 - dx) * lut[i000 + 0] + dx * lut[i100 + 0]) +
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
                                    jint width, jint height, jint rowStride, jfloatArray lutArray, jint lutSize)
{
    auto start = std::chrono::high_resolution_clock::now();

    auto* inData = static_cast<uint8_t*>(env->GetDirectBufferAddress(inRgbaBuf));
    auto* outData = static_cast<uint8_t*>(env->GetDirectBufferAddress(outRgbaBuf));

    jfloat* lut = nullptr;
    if (lutArray != nullptr && lutSize > 0) {
        lut = env->GetFloatArrayElements(lutArray, nullptr);
    }

    // If no LUT is selected, fallback to the super-fast SIMD memory copy
    if (lut == nullptr) {
        for (int y = 0; y < height; y++) {
            uint8_t* inRow = inData + (y * rowStride);
            uint8_t* outRow = outData + (y * width * 4);
            for (int x = 0; x < width * 4; x += 16) {
                vst1q_u8(outRow + x, vld1q_u8(inRow + x));
            }
        }
        auto end = std::chrono::high_resolution_clock::now();
        return static_cast<jdouble>(std::chrono::duration<double, std::milli>(end - start).count());
    }

    float max_index = static_cast<float>(std::max(lutSize - 1, 1));
    int size2 = lutSize * lutSize;

    float32x4_t v_max_index = vdupq_n_f32(max_index);
    float32x4_t v_255_inv   = vdupq_n_f32(1.0f / 255.0f);
    float32x4_t v_255       = vdupq_n_f32(255.0f);
    float32x4_t v_one       = vdupq_n_f32(1.0f);
    float32x4_t v_zero      = vdupq_n_f32(0.0f);

    // SIMD Helper function for Fused-Multiply-Add (FMA) Interpolation
    auto mix_colors = [](float32x4_t c0, float32x4_t c1, float32x4_t d, float32x4_t inv_d) {
        return vmlaq_f32(vmulq_f32(inv_d, c0), d, c1);
    };

    int num_threads = std::thread::hardware_concurrency()-1;
    //default case
    if (num_threads == 0) num_threads = 4;

    std::vector<std::thread> threads;
    int chunk_size = height / num_threads;

    // Spawn the threads
    for (int t = 0; t < num_threads; t++) {
        // We use [=] to pass all our SIMD constants and pointers into the thread safely
        threads.emplace_back([=]() {

            // Calculate which rows this specific core will process
            int start_y = t * chunk_size;
            int end_y = (t == num_threads - 1) ? height : start_y + chunk_size;

            for (int y = start_y; y < end_y; y++) {
                uint8_t* inRow = inData + (y * rowStride);
                uint8_t* outRow = outData + (y * width * 4);

                int x = 0;
                // Process 4 pixels (16 bytes) at a time
                for (; x <= (width - 4) * 4; x += 16) {
                    uint8_t *p = inRow + x;

                    //Extract R, G, B, A for 4 pixels
                    uint32_t r_arr[4] = {p[0], p[4], p[8], p[12]};
                    uint32_t g_arr[4] = {p[1], p[5], p[9], p[13]};
                    uint32_t b_arr[4] = {p[2], p[6], p[10], p[14]};
                    uint32_t a_arr[4] = {p[3], p[7], p[11], p[15]};

                    // Load into SIMD Float Vectors
                    float32x4_t r_f = vcvtq_f32_u32(vld1q_u32(r_arr));
                    float32x4_t g_f = vcvtq_f32_u32(vld1q_u32(g_arr));
                    float32x4_t b_f = vcvtq_f32_u32(vld1q_u32(b_arr));

                    // Normalize (0.0 - 1.0) and multiply by max_index
                    float32x4_t lutX = vmulq_f32(vmulq_f32(r_f, v_255_inv), v_max_index);
                    float32x4_t lutY = vmulq_f32(vmulq_f32(g_f, v_255_inv), v_max_index);
                    float32x4_t lutZ = vmulq_f32(vmulq_f32(b_f, v_255_inv), v_max_index);

                    //Get Base Grid Coordinates (x0, y0, z0)
                    uint32x4_t x0_u = vcvtq_u32_f32(lutX);
                    uint32x4_t y0_u = vcvtq_u32_f32(lutY);
                    uint32x4_t z0_u = vcvtq_u32_f32(lutZ);

                    //Get Distances (dx, dy, dz) & Inverted Distances
                    float32x4_t dx = vsubq_f32(lutX, vcvtq_f32_u32(x0_u));
                    float32x4_t dy = vsubq_f32(lutY, vcvtq_f32_u32(y0_u));
                    float32x4_t dz = vsubq_f32(lutZ, vcvtq_f32_u32(z0_u));

                    float32x4_t inv_dx = vsubq_f32(v_one, dx);
                    float32x4_t inv_dy = vsubq_f32(v_one, dy);
                    float32x4_t inv_dz = vsubq_f32(v_one, dz);

                    //Calculate memory indices and gather LUT floats
                    uint32_t x0_arr[4], y0_arr[4], z0_arr[4];
                    vst1q_u32(x0_arr, x0_u);
                    vst1q_u32(y0_arr, y0_u);
                    vst1q_u32(z0_arr, z0_u);

                    float c000_R[4], c100_R[4], c010_R[4], c110_R[4], c001_R[4], c101_R[4], c011_R[4], c111_R[4];
                    float c000_G[4], c100_G[4], c010_G[4], c110_G[4], c001_G[4], c101_G[4], c011_G[4], c111_G[4];
                    float c000_B[4], c100_B[4], c010_B[4], c110_B[4], c001_B[4], c101_B[4], c011_B[4], c111_B[4];

                    for (int i = 0; i < 4; i++) {
                        int x0 = x0_arr[i]; int y0 = y0_arr[i]; int z0 = z0_arr[i];
                        int x1 = std::min(x0 + 1, lutSize - 1);
                        int y1 = std::min(y0 + 1, lutSize - 1);
                        int z1 = std::min(z0 + 1, lutSize - 1);

                        int i000 = (z0 * size2 + y0 * lutSize + x0) * 3;
                        int i100 = (z0 * size2 + y0 * lutSize + x1) * 3;
                        int i010 = (z0 * size2 + y1 * lutSize + x0) * 3;
                        int i110 = (z0 * size2 + y1 * lutSize + x1) * 3;
                        int i001 = (z1 * size2 + y0 * lutSize + x0) * 3;
                        int i101 = (z1 * size2 + y0 * lutSize + x1) * 3;
                        int i011 = (z1 * size2 + y1 * lutSize + x0) * 3;
                        int i111 = (z1 * size2 + y1 * lutSize + x1) * 3;

                        c000_R[i] = lut[i000];   c000_G[i] = lut[i000 + 1]; c000_B[i] = lut[i000 + 2];
                        c100_R[i] = lut[i100];   c100_G[i] = lut[i100 + 1]; c100_B[i] = lut[i100 + 2];
                        c010_R[i] = lut[i010];   c010_G[i] = lut[i010 + 1]; c010_B[i] = lut[i010 + 2];
                        c110_R[i] = lut[i110];   c110_G[i] = lut[i110 + 1]; c110_B[i] = lut[i110 + 2];
                        c001_R[i] = lut[i001];   c001_G[i] = lut[i001 + 1]; c001_B[i] = lut[i001 + 2];
                        c101_R[i] = lut[i101];   c101_G[i] = lut[i101 + 1]; c101_B[i] = lut[i101 + 2];
                        c011_R[i] = lut[i011];   c011_G[i] = lut[i011 + 1]; c011_B[i] = lut[i011 + 2];
                        c111_R[i] = lut[i111];   c111_G[i] = lut[i111 + 1]; c111_B[i] = lut[i111 + 2];
                    }

                    // RED Channel
                    float32x4_t mixX0_R = mix_colors(vld1q_f32(c000_R), vld1q_f32(c100_R), dx, inv_dx);
                    float32x4_t mixX1_R = mix_colors(vld1q_f32(c010_R), vld1q_f32(c110_R), dx, inv_dx);
                    float32x4_t mixX2_R = mix_colors(vld1q_f32(c001_R), vld1q_f32(c101_R), dx, inv_dx);
                    float32x4_t mixX3_R = mix_colors(vld1q_f32(c011_R), vld1q_f32(c111_R), dx, inv_dx);
                    float32x4_t mixY0_R = mix_colors(mixX0_R, mixX1_R, dy, inv_dy);
                    float32x4_t mixY1_R = mix_colors(mixX2_R, mixX3_R, dy, inv_dy);
                    float32x4_t final_R = mix_colors(mixY0_R, mixY1_R, dz, inv_dz);

                    // GREEN Channel
                    float32x4_t mixX0_G = mix_colors(vld1q_f32(c000_G), vld1q_f32(c100_G), dx, inv_dx);
                    float32x4_t mixX1_G = mix_colors(vld1q_f32(c010_G), vld1q_f32(c110_G), dx, inv_dx);
                    float32x4_t mixX2_G = mix_colors(vld1q_f32(c001_G), vld1q_f32(c101_G), dx, inv_dx);
                    float32x4_t mixX3_G = mix_colors(vld1q_f32(c011_G), vld1q_f32(c111_G), dx, inv_dx);
                    float32x4_t mixY0_G = mix_colors(mixX0_G, mixX1_G, dy, inv_dy);
                    float32x4_t mixY1_G = mix_colors(mixX2_G, mixX3_G, dy, inv_dy);
                    float32x4_t final_G = mix_colors(mixY0_G, mixY1_G, dz, inv_dz);

                    // BLUE Channel
                    float32x4_t mixX0_B = mix_colors(vld1q_f32(c000_B), vld1q_f32(c100_B), dx, inv_dx);
                    float32x4_t mixX1_B = mix_colors(vld1q_f32(c010_B), vld1q_f32(c110_B), dx, inv_dx);
                    float32x4_t mixX2_B = mix_colors(vld1q_f32(c001_B), vld1q_f32(c101_B), dx, inv_dx);
                    float32x4_t mixX3_B = mix_colors(vld1q_f32(c011_B), vld1q_f32(c111_B), dx, inv_dx);
                    float32x4_t mixY0_B = mix_colors(mixX0_B, mixX1_B, dy, inv_dy);
                    float32x4_t mixY1_B = mix_colors(mixX2_B, mixX3_B, dy, inv_dy);
                    float32x4_t final_B = mix_colors(mixY0_B, mixY1_B, dz, inv_dz);

                    //Clamp, Scale, and Pack back into Integers
                    final_R = vmaxq_f32(vminq_f32(vmulq_f32(final_R, v_255), v_255), v_zero);
                    final_G = vmaxq_f32(vminq_f32(vmulq_f32(final_G, v_255), v_255), v_zero);
                    final_B = vmaxq_f32(vminq_f32(vmulq_f32(final_B, v_255), v_255), v_zero);

                    uint32x4_t out_R = vcvtq_u32_f32(final_R);
                    uint32x4_t out_G = vcvtq_u32_f32(final_G);
                    uint32x4_t out_B = vcvtq_u32_f32(final_B);
                    uint32x4_t out_A = vld1q_u32(a_arr);

                    // Reconstruct 4 Pixels
                    uint32x4_t out_pixels = out_R;
                    out_pixels = vorrq_u32(out_pixels, vshlq_n_u32(out_G, 8));
                    out_pixels = vorrq_u32(out_pixels, vshlq_n_u32(out_B, 16));
                    out_pixels = vorrq_u32(out_pixels, vshlq_n_u32(out_A, 24));

                    vst1q_u32(reinterpret_cast<uint32_t*>(outRow + x), out_pixels);
                }
            }
        });
    }

    // Wait for all CPU cores to finish their chunk of the image
    for (auto& thread : threads) {
        thread.join();
    }

    if (lut != nullptr) { env->ReleaseFloatArrayElements(lutArray, lut, JNI_ABORT); }

    auto end = std::chrono::high_resolution_clock::now();
    return static_cast<jdouble>(std::chrono::duration<double, std::milli>(end - start).count());
}