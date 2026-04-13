#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <chrono>
#include <algorithm>
#include <arm_neon.h>
#include <android/log.h>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>

#include <android/native_window_jni.h>

#define LOG_TAG "NativeGPU"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)


// Sigleton design pattern for Thread Pool
class ThreadPool {
public:
    static ThreadPool& getInstance() {
        static ThreadPool instance;
        return instance;
    }
    void executeAndWait(const std::vector<std::function<void()>>& tasks) {

        std::unique_lock<std::mutex> lock(mtx);
        pending = tasks.size();
        for (auto& t : tasks) queue.push(t);
        // Wake up all threads
        cv_workers.notify_all();
        // Wait for all threads to finish
        cv_main.wait(lock, [this] { return pending == 0; });
    }
private:
    ThreadPool() : stop(false), pending(0) {
        // Determine number of cores
        int cores = std::max(1u, std::thread::hardware_concurrency()/2);
        // Create all threads
        for (int i = 0; i < cores; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mtx);
                        cv_workers.wait(lock, [this] { return stop || !queue.empty(); });
                        if (stop && queue.empty()) return;
                        task = std::move(queue.front()); queue.pop();
                    }

                    task(); // Do the math

                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        if (--pending == 0) cv_main.notify_one();
                    }
                }
            });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            stop = true;
        }
        cv_workers.notify_all();
        // Wait for all threads to finish
        for (auto& w : workers) w.join();
    }
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> queue;
    std::mutex mtx;
    std::condition_variable cv_workers, cv_main;
    int pending;
    bool stop;
};

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    ThreadPool::getInstance();
    return JNI_VERSION_1_6;
}

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
Java_com_example_videoprocessingengine_MainActivity_processFrameNativeSIMD(JNIEnv* env, jobject, jobject inRgbaBuf, jobject outRgbaBuf,jint width, jint height, jint rowStride, jfloatArray lutArray, jint lutSize){
    auto start = std::chrono::high_resolution_clock::now();

    auto* inData = static_cast<uint8_t*>(env->GetDirectBufferAddress(inRgbaBuf));
    auto* outData = static_cast<uint8_t*>(env->GetDirectBufferAddress(outRgbaBuf));

    jfloat* lut = nullptr;
    if (lutArray != nullptr && lutSize > 0) {
        lut = env->GetFloatArrayElements(lutArray, nullptr);
    }

    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    int chunk_size = height / num_threads;
    std::vector<std::function<void()>> tasks;


    // If no LUT is selected, fallback to the super-fast SIMD memory copy
    if (lut == nullptr) {
        for (int t = 0; t < num_threads; t++) {
            tasks.emplace_back([=]() {
                int start_y = t * chunk_size;
                int end_y = (t == num_threads - 1) ? height : start_y + chunk_size;
                for (int y = start_y; y < end_y; y++) {
                    uint8_t* inRow = inData + (y * rowStride);
                    uint8_t* outRow = outData + (y * width * 4);
                    for (int x = 0; x < width * 4; x += 16) {
                        vst1q_u8(outRow + x, vld1q_u8(inRow + x));
                    }
                }
            });
        }
        ThreadPool::getInstance().executeAndWait(tasks);
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


    // Spawn the threads
    for (int t = 0; t < num_threads; t++) {
        // We use [=] to pass all our SIMD constants and pointers into the thread safely
        tasks.emplace_back([=]() {

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
    ThreadPool::getInstance().executeAndWait(tasks);

    if (lut != nullptr) { env->ReleaseFloatArrayElements(lutArray, lut, JNI_ABORT); }

    auto end = std::chrono::high_resolution_clock::now();
    return static_cast<jdouble>(std::chrono::duration<double, std::milli>(end - start).count());
}

class GPUProcessor {
public:
  static GPUProcessor &getInstance() {
    static GPUProcessor instance;
    return instance;
  }

  GLint hasLutLoc = -1;
  GLint lutSamplerLoc = -1;

  void process(uint8_t *inData, uint8_t *outData, int width, int height,
               int rowStride, float *lut, int lutSize, bool lutChanged, ANativeWindow* window) {
    initEGL();
    
    if (window != lastWindow) {
        if (windowEglSurface != EGL_NO_SURFACE) {
            eglDestroySurface(eglDisplay, windowEglSurface);
            windowEglSurface = EGL_NO_SURFACE;
        }
        if (window != nullptr) {
            windowEglSurface = eglCreateWindowSurface(eglDisplay, eglConfig, window, nullptr);
        }
        lastWindow = window;
    }

    if (windowEglSurface != EGL_NO_SURFACE) {
        eglMakeCurrent(eglDisplay, windowEglSurface, windowEglSurface, eglContext);
    } else if (eglContext != EGL_NO_CONTEXT && eglGetCurrentContext() != eglContext) {
        eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);
    }

    initGL(width, height);

    glBindTexture(GL_TEXTURE_2D, frameTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rowStride / 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA,
                    GL_UNSIGNED_BYTE, inData);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    if (lut != nullptr && lutSize > 0) {
      if (lutChanged) {
        glBindTexture(GL_TEXTURE_3D, lutTex);
        std::vector<uint8_t> lut8(lutSize * lutSize * lutSize * 3);
        int totalElements = lutSize * lutSize * lutSize * 3;
        for (int i = 0; i < totalElements; i++) {
          lut8[i] = static_cast<uint8_t>(
              std::max(0.0f, std::min(255.0f, lut[i] * 255.0f)));
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB8, lutSize, lutSize, lutSize, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, lut8.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        hasLut = true;
      }
    } else {
      hasLut = false;
    }

    glUseProgram(computeProgram);
    glUniform1i(hasLutLoc, hasLut ? 1 : 0);

    glBindImageTexture(0, frameTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(1, outTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_3D, lutTex);
    glUniform1i(lutSamplerLoc, 2);

    GLuint groupX = (width + 15) / 16;
    GLuint groupY = (height + 15) / 16;
    glDispatchCompute(groupX, groupY, 1);

    // Wait for the compute shader to finish storing to outTex
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    if (windowEglSurface != EGL_NO_SURFACE) {
        renderToWindow(window);
        eglSwapBuffers(eglDisplay, windowEglSurface);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, width);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, outData);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
  }

  void renderToWindow(ANativeWindow* window) {
      int winW = ANativeWindow_getWidth(window);
      int winH = ANativeWindow_getHeight(window);
      glViewport(0, 0, winW, winH);
      glClear(GL_COLOR_BUFFER_BIT);
      glUseProgram(quadProgram);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, outTex);
      glUniform1i(glGetUniformLocation(quadProgram, "tex"), 0);

      float windowAspectRatio = static_cast<float>(winW) / static_cast<float>(winH);
      // The content is rotated 90 degrees, so aspect is H/W
      float contentAspectRatio = static_cast<float>(texHeight) / static_cast<float>(texWidth);

      float scaleX = 1.0f;
      float scaleY = 1.0f;

      if (contentAspectRatio > windowAspectRatio) {
          // Content is wider than window, scale X (crop horizontal)
          scaleX = contentAspectRatio / windowAspectRatio;
      } else {
          // Window is wider than content, scale Y (crop vertical)
          scaleY = windowAspectRatio / contentAspectRatio;
      }

      float verts[] = {
          -scaleX, -scaleY, 0.0f, 0.0f,
           scaleX, -scaleY, 1.0f, 0.0f,
          -scaleX,  scaleY, 0.0f, 1.0f,
           scaleX,  scaleY, 1.0f, 1.0f
      };
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts);
      glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts + 2);
      glEnableVertexAttribArray(0);
      glEnableVertexAttribArray(1);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

private:
  EGLDisplay eglDisplay = EGL_NO_DISPLAY;
  EGLContext eglContext = EGL_NO_CONTEXT;
  EGLSurface eglSurface = EGL_NO_SURFACE;
  bool initialized = false;

  GLuint computeProgram = 0;
  GLuint quadProgram = 0;
  GLuint frameTex = 0;
  GLuint outTex = 0;
  GLuint lutTex = 0;
  GLuint fbo = 0;
  int texWidth = 0;
  int texHeight = 0;
  bool hasLut = false;
  EGLSurface windowEglSurface = EGL_NO_SURFACE;
  ANativeWindow* lastWindow = nullptr;
  EGLConfig eglConfig;

  GPUProcessor() {}

  void initEGL() {
    if (initialized)
      return;

    eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(eglDisplay, nullptr, nullptr);

    const EGLint configAttribs[] = {EGL_SURFACE_TYPE,
                                    EGL_PBUFFER_BIT | EGL_WINDOW_BIT,
                                    EGL_RENDERABLE_TYPE,
                                    EGL_OPENGL_ES3_BIT,
                                    EGL_RED_SIZE,
                                    8,
                                    EGL_GREEN_SIZE,
                                    8,
                                    EGL_BLUE_SIZE,
                                    8,
                                    EGL_ALPHA_SIZE,
                                    8,
                                    EGL_NONE};

    EGLint numConfigs;
    eglChooseConfig(eglDisplay, configAttribs, &eglConfig, 1, &numConfigs);

    const EGLint pbufferAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    eglSurface = eglCreatePbufferSurface(eglDisplay, eglConfig, pbufferAttribs);

    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    eglContext =
        eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, contextAttribs);

    eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);

    initialized = true;
  }

  void initGL(int w, int h) {
    if (computeProgram == 0) {
      const char *shaderSrc = R"(#version 310 es
                precision mediump float;
                precision mediump image2D;
                precision mediump sampler3D;

                layout(local_size_x = 16, local_size_y = 16) in;
                layout(binding = 0, rgba8) uniform readonly image2D inTexture;
                layout(binding = 1, rgba8) uniform writeonly image2D outTexture;

                uniform sampler3D lutSampler;
                uniform int hasLut;

                void main() {
                    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
                    ivec2 size = imageSize(inTexture);
                    if (pos.x >= size.x || pos.y >= size.y) return;

                    vec4 color = imageLoad(inTexture, pos);
                    
                    if (hasLut == 1) {
                        vec3 lutColor = texture(lutSampler, color.rgb).rgb;
                        imageStore(outTexture, pos, vec4(lutColor, color.a));
                    } else {
                        imageStore(outTexture, pos, color);
                    }
                }
            )";

      GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
      glShaderSource(shader, 1, &shaderSrc, nullptr);
      glCompileShader(shader);

      GLint success;
      glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
      if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        LOGE("Compute Shader Error: %s", infoLog);
      }

      computeProgram = glCreateProgram();
      glAttachShader(computeProgram, shader);
      glLinkProgram(computeProgram);
      glDeleteShader(shader);

      hasLutLoc = glGetUniformLocation(computeProgram, "hasLut");
      lutSamplerLoc = glGetUniformLocation(computeProgram, "lutSampler");
    }

    if (quadProgram == 0) {
        const char* vertSrc = R"(#version 300 es
            layout(location = 0) in vec2 pos;
            layout(location = 1) in vec2 uv;
            out vec2 vUv;
            void main() {
                vUv = uv;
                gl_Position = vec4(pos, 0.0, 1.0);
            }
        )";
        const char* fragSrc = R"(#version 300 es
            precision mediump float;
            uniform sampler2D tex;
            in vec2 vUv;
            out vec4 outColor;
            void main() {
                // Correct for 90-degree counter-clockwise rotation 
                // typically provided by Android camera buffers
                vec2 rotatedUv = vec2(1.0 - vUv.y, 1.0 - vUv.x);
                outColor = texture(tex, rotatedUv);
            }
        )";
        GLuint vShader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vShader, 1, &vertSrc, nullptr);
        glCompileShader(vShader);
        GLuint fShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fShader, 1, &fragSrc, nullptr);
        glCompileShader(fShader);
        quadProgram = glCreateProgram();
        glAttachShader(quadProgram, vShader);
        glAttachShader(quadProgram, fShader);
        glLinkProgram(quadProgram);
    }

    if (texWidth != w || texHeight != h) {
      if (frameTex)
        glDeleteTextures(1, &frameTex);
      if (outTex)
        glDeleteTextures(1, &outTex);
      if (fbo)
        glDeleteFramebuffers(1, &fbo);

      glGenTextures(1, &frameTex);
      glBindTexture(GL_TEXTURE_2D, frameTex);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

      glGenTextures(1, &outTex);
      glBindTexture(GL_TEXTURE_2D, outTex);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

      glGenFramebuffers(1, &fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, outTex, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);

      texWidth = w;
      texHeight = h;
    }

    if (lutTex == 0) {
      glGenTextures(1, &lutTex);
      glBindTexture(GL_TEXTURE_3D, lutTex);
      // Linear filtering is required for hardware trilinear interpolation
      glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    }
  }
};

static jobject globalLastLutArray = nullptr;

extern "C" JNIEXPORT jdouble JNICALL
Java_com_example_videoprocessingengine_MainActivity_processFrameNativeGPU(
    JNIEnv *env, jobject, jobject inRgbaBuf, jobject outRgbaBuf, jint width,
    jint height, jint rowStride, jfloatArray lutArray, jint lutSize, jobject surface) {
  auto start = std::chrono::high_resolution_clock::now();

  auto *inData = static_cast<uint8_t *>(env->GetDirectBufferAddress(inRgbaBuf));
  auto *outData =
      static_cast<uint8_t *>(env->GetDirectBufferAddress(outRgbaBuf));

  bool lutChanged = false;
  if (lutArray == nullptr) {
      if (globalLastLutArray != nullptr) {
          env->DeleteGlobalRef(globalLastLutArray);
          globalLastLutArray = nullptr;
          lutChanged = true;
      }
  } else {
      if (globalLastLutArray == nullptr || !env->IsSameObject(lutArray, globalLastLutArray)) {
          if (globalLastLutArray != nullptr) {
              env->DeleteGlobalRef(globalLastLutArray);
          }
          globalLastLutArray = env->NewGlobalRef(lutArray);
          lutChanged = true;
      }
  }

  jfloat *lut = nullptr;
  if (lutArray != nullptr && lutSize > 0) {
    lut = env->GetFloatArrayElements(lutArray, nullptr);
  }

  ANativeWindow* window = nullptr;
  if (surface != nullptr) {
      window = ANativeWindow_fromSurface(env, surface);
  }

  GPUProcessor::getInstance().process(inData, outData, width, height, rowStride,
                                      lut, lutSize, lutChanged, window);

  if (window != nullptr) {
      ANativeWindow_release(window);
  }

  if (lut != nullptr) {
    env->ReleaseFloatArrayElements(lutArray, lut, JNI_ABORT);
  }

  auto end = std::chrono::high_resolution_clock::now();
  return static_cast<jdouble>(
      std::chrono::duration<double, std::milli>(end - start).count());
}
