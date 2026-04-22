#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <chrono>
#include <algorithm>
#include <arm_neon.h>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <android/native_window.h>
#include <android/native_window_jni.h>

float32x4_t mixColors(float32x4_t c0, float32x4_t c1, float32x4_t d, float32x4_t inv_d)
{
    return vmlaq_f32(vmulq_f32(inv_d, c0), d, c1);
}

class Task
{
public:
    virtual ~Task() {}
    virtual void run() = 0;
};

class ThreadPool
{
public:
    static ThreadPool &get()
    {
        static ThreadPool inst;
        return inst;
    }
    void exec(const std::vector<Task *> &tasks)
    {
        std::unique_lock<std::mutex> lock(mtx);
        pending = (int)tasks.size();
        for (size_t i = 0; i < tasks.size(); i++)
            q.push(tasks[i]);
        cv_w.notify_all();
        while (pending > 0)
            cv_m.wait(lock);
    }

private:
    ThreadPool() : stop(false), pending(0)
    {
        int n = std::max(1u, std::thread::hardware_concurrency()/2);
        for (int i = 0; i < n; ++i)
            workers.emplace_back(&ThreadPool::work, this);
    }
    void work()
    {
        while (true)
        {
            Task *t = nullptr;
            {
                std::unique_lock<std::mutex> lock(mtx);
                while (!stop && q.empty())
                    cv_w.wait(lock);
                if (stop && q.empty())
                    return;
                t = q.front();
                q.pop();
            }
            if (t)
                t->run();
            {
                std::lock_guard<std::mutex> lock(mtx);
                pending--;
                if (pending == 0)
                    cv_m.notify_one();
            }
        }
    }
    std::vector<std::thread> workers;
    std::queue<Task *> q;
    std::mutex mtx;
    std::condition_variable cv_w, cv_m;
    int pending;
    bool stop;
};

class CopyTask : public Task
{
    uint8_t *in, *out;
    int sy, ey, w, rs;

public:
    CopyTask(uint8_t *i, uint8_t *o, int s, int e, int width, int r) : in(i), out(o), sy(s), ey(e), w(width), rs(r) {}
    void run() override
    {
        for (int y = sy; y < ey; y++)
        {
            uint8_t *r_i = in + (y * rs), *r_o = out + (y * w * 4);
            for (int x = 0; x < w * 4; x += 16)
                vst1q_u8(r_o + x, vld1q_u8(r_i + x));
        }
    }
};

class LutTask : public Task
{
    uint8_t *in, *out;
    int sy, ey, w, h, rs, lS, s2;
    jfloat *lut;
    float32x4_t vM, v2I, v2, v1, v0;

public:
    LutTask(uint8_t *i, uint8_t *o, int s, int e, int width, int height, int r, jfloat *l, int ls, int sz2, float32x4_t vm, float32x4_t vi, float32x4_t v255, float32x4_t vo, float32x4_t vz)
        : in(i), out(o), sy(s), ey(e), w(width), h(height), rs(r), lut(l), lS(ls), s2(sz2), vM(vm), v2I(vi), v2(v255), v1(vo), v0(vz) {}
    void run() override
    {
        for (int y = sy; y < ey; y++)
        {

            uint8_t *r_i = in + (y * rs), *r_o = out + (y * w * 4);

            for (int x = 0; x <= (w - 4) * 4; x += 16)
            {

                uint8_t *p = r_i + x;
                uint32_t r[4] = {p[0], p[4], p[8], p[12]}, g[4] = {p[1], p[5], p[9], p[13]}, b[4] = {p[2], p[6], p[10], p[14]}, a[4] = {p[3], p[7], p[11], p[15]};

                float32x4_t rf = vcvtq_f32_u32(vld1q_u32(r)), gf = vcvtq_f32_u32(vld1q_u32(g)), bf = vcvtq_f32_u32(vld1q_u32(b));
                float32x4_t lx = vmulq_f32(vmulq_f32(rf, v2I), vM), ly = vmulq_f32(vmulq_f32(gf, v2I), vM), lz = vmulq_f32(vmulq_f32(bf, v2I), vM);
                uint32x4_t x0u = vcvtq_u32_f32(lx), y0u = vcvtq_u32_f32(ly), z0u = vcvtq_u32_f32(lz);
                float32x4_t dx = vsubq_f32(lx, vcvtq_f32_u32(x0u)), dy = vsubq_f32(ly, vcvtq_f32_u32(y0u)), dz = vsubq_f32(lz, vcvtq_f32_u32(z0u));
                float32x4_t idx = vsubq_f32(v1, dx), idy = vsubq_f32(v1, dy), idz = vsubq_f32(v1, dz);

                uint32_t x0a[4], y0a[4], z0a[4];
                vst1q_u32(x0a, x0u);
                vst1q_u32(y0a, y0u);
                vst1q_u32(z0a, z0u);

                float cR[8][4], cG[8][4], cB[8][4];

                for (int j = 0; j < 4; j++)
                {
                    int x0 = x0a[j], y0 = y0a[j], z0 = z0a[j], x1 = std::min(x0 + 1, lS - 1), y1 = std::min(y0 + 1, lS - 1), z1 = std::min(z0 + 1, lS - 1);
                    int idxs[8] = {(z0 * s2 + y0 * lS + x0) * 3, (z0 * s2 + y0 * lS + x1) * 3, (z0 * s2 + y1 * lS + x0) * 3, (z0 * s2 + y1 * lS + x1) * 3, (z1 * s2 + y0 * lS + x0) * 3, (z1 * s2 + y0 * lS + x1) * 3, (z1 * s2 + y1 * lS + x0) * 3, (z1 * s2 + y1 * lS + x1) * 3};
                    for (int k = 0; k < 8; k++)
                    {
                        cR[k][j] = lut[idxs[k]];
                        cG[k][j] = lut[idxs[k] + 1];
                        cB[k][j] = lut[idxs[k] + 2];
                    }
                }
                auto interp = [&](float c[8][4])
                {
                    float32x4_t m0 = mixColors(vld1q_f32(c[0]), vld1q_f32(c[1]), dx, idx), m1 = mixColors(vld1q_f32(c[2]), vld1q_f32(c[3]), dx, idx);
                    float32x4_t m2 = mixColors(vld1q_f32(c[4]), vld1q_f32(c[5]), dx, idx), m3 = mixColors(vld1q_f32(c[6]), vld1q_f32(c[7]), dx, idx);
                    return mixColors(mixColors(m0, m1, dy, idy), mixColors(m2, m3, dy, idy), dz, idz);
                };

                float32x4_t fr = interp(cR), fg = interp(cG), fb = interp(cB);
                fr = vmaxq_f32(vminq_f32(vmulq_f32(fr, v2), v2), v0);
                fg = vmaxq_f32(vminq_f32(vmulq_f32(fg, v2), v2), v0);
                fb = vmaxq_f32(vminq_f32(vmulq_f32(fb, v2), v2), v0);
                uint32x4_t res = vorrq_u32(vorrq_u32(vorrq_u32(vcvtq_u32_f32(fr), vshlq_n_u32(vcvtq_u32_f32(fg), 8)), vshlq_n_u32(vcvtq_u32_f32(fb), 16)), vshlq_n_u32(vld1q_u32(a), 24));
                vst1q_u32((uint32_t *)(r_o + x), res);
            }
        }
    }
};

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *res)
{
    ThreadPool::get();
    return JNI_VERSION_1_6;
}

float trilinear(float *lut, int i[8], float dx, float dy, float dz, int off)
{
    float c00 = lut[i[0] + off] * (1 - dx) + lut[i[1] + off] * dx, c10 = lut[i[2] + off] * (1 - dx) + lut[i[3] + off] * dx;
    float c01 = lut[i[4] + off] * (1 - dx) + lut[i[5] + off] * dx, c11 = lut[i[6] + off] * (1 - dx) + lut[i[7] + off] * dx;
    return (c00 * (1 - dy) + c10 * dy) * (1 - dz) + (c01 * (1 - dy) + c11 * dy) * dz;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_example_videoprocessingengine_MainActivity_processFrameNative(JNIEnv *env, jobject, jobject inBuf, jobject outBuf, jint w, jint h, jint rs, jfloatArray lArr, jint lSz)
{
    auto start = std::chrono::high_resolution_clock::now();
    uint8_t *in = (uint8_t *)env->GetDirectBufferAddress(inBuf), *out = (uint8_t *)env->GetDirectBufferAddress(outBuf);
    jfloat *lut = (lArr && lSz > 0) ? env->GetFloatArrayElements(lArr, nullptr) : nullptr;
    float max_i = (float)std::max(lSz - 1, 1);
    for (int y = 0; y < h; y++)
    {
        uint8_t *r_i = in + (y * rs), *r_o = out + (y * w * 4);
        for (int x = 0; x < w * 4; x += 4)
        {
            if (!lut)
            {
                *(uint32_t *)(r_o + x) = *(uint32_t *)(r_i + x);
            }
            else
            {
                float lx = (r_i[x] / 255.0f) * max_i, ly = (r_i[x + 1] / 255.0f) * max_i, lz = (r_i[x + 2] / 255.0f) * max_i;
                int x0 = (int)lx, y0 = (int)ly, z0 = (int)lz, x1 = std::min(x0 + 1, lSz - 1), y1 = std::min(y0 + 1, lSz - 1), z1 = std::min(z0 + 1, lSz - 1), s2 = lSz * lSz;
                float dx = lx - x0, dy = ly - y0, dz = lz - z0;
                int idxs[8] = {(z0 * s2 + y0 * lSz + x0) * 3, (z0 * s2 + y0 * lSz + x1) * 3, (z0 * s2 + y1 * lSz + x0) * 3, (z0 * s2 + y1 * lSz + x1) * 3, (z1 * s2 + y0 * lSz + x0) * 3, (z1 * s2 + y0 * lSz + x1) * 3, (z1 * s2 + y1 * lSz + x0) * 3, (z1 * s2 + y1 * lSz + x1) * 3};
                r_o[x] = (uint8_t)(std::max(0.0f, std::min(255.0f, trilinear(lut, idxs, dx, dy, dz, 0) * 255.0f)));
                r_o[x + 1] = (uint8_t)(std::max(0.0f, std::min(255.0f, trilinear(lut, idxs, dx, dy, dz, 1) * 255.0f)));
                r_o[x + 2] = (uint8_t)(std::max(0.0f, std::min(255.0f, trilinear(lut, idxs, dx, dy, dz, 2) * 255.0f)));
                r_o[x + 3] = r_i[x + 3];
            }
        }
    }
    if (lut)
        env->ReleaseFloatArrayElements(lArr, lut, JNI_ABORT);
    auto end = std::chrono::high_resolution_clock::now();
    return (jdouble)(std::chrono::duration<double, std::milli>(end - start).count());
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_example_videoprocessingengine_MainActivity_processFrameNativeSIMD(JNIEnv *env, jobject, jobject inBuf, jobject outBuf, jint w, jint h, jint rs, jfloatArray lArr, jint lSz)
{
    auto start = std::chrono::high_resolution_clock::now();
    uint8_t *in = (uint8_t *)env->GetDirectBufferAddress(inBuf), *out = (uint8_t *)env->GetDirectBufferAddress(outBuf);
    jfloat *lut = (lArr && lSz > 0) ? env->GetFloatArrayElements(lArr, nullptr) : nullptr;
    int n = std::max(1, (int)std::thread::hardware_concurrency()), step = h / n;
    std::vector<Task *> tasks;
    if (!lut)
    {
        for (int i = 0; i < n; i++)
            tasks.push_back(new CopyTask(in, out, i * step, (i == n - 1) ? h : (i + 1) * step, w, rs));
    }
    else
    {
        float32x4_t vM = vdupq_n_f32((float)(lSz - 1)), vi = vdupq_n_f32(1.0f / 255.0f), v255 = vdupq_n_f32(255.0f), v1 = vdupq_n_f32(1.0f), v0 = vdupq_n_f32(0.0f);
        for (int i = 0; i < n; i++)
            tasks.push_back(new LutTask(in, out, i * step, (i == n - 1) ? h : (i + 1) * step, w, h, rs, lut, lSz, lSz * lSz, vM, vi, v255, v1, v0));
    }
    ThreadPool::get().exec(tasks);
    for (size_t i = 0; i < tasks.size(); i++)
        delete tasks[i];
    if (lut)
        env->ReleaseFloatArrayElements(lArr, lut, JNI_ABORT);
    auto end = std::chrono::high_resolution_clock::now();
    return (jdouble)(std::chrono::duration<double, std::milli>(end - start).count());
}

class Gpu
{
public:
    static Gpu &get()
    {
        static Gpu inst;
        return inst;
    }
    GLint hL = -1, lS = -1;
    void proc(uint8_t *cd, uint8_t *od, int w, int h, int rs, float *ld, int lsz, bool up, jobject surf, JNIEnv *env)
    {
        if (!init)
        {
            disp = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            eglInitialize(disp, 0, 0);
            const EGLint a[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT | EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
            EGLint n;
            eglChooseConfig(disp, a, &cfg, 1, &n);
            const EGLint pba[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
            pbs = eglCreatePbufferSurface(disp, cfg, pba);
            const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
            ctx = eglCreateContext(disp, cfg, EGL_NO_CONTEXT, ca);
            eglMakeCurrent(disp, pbs, pbs, ctx);
            init = true;
        }
        if (!surf)
        {
            if (cRef)
            {
                if (wSurf != EGL_NO_SURFACE)
                    eglDestroySurface(disp, wSurf);
                wSurf = EGL_NO_SURFACE;
                freeW(env);
            }
        }
        else if (!cRef || !env->IsSameObject(surf, cRef))
        {
            if (wSurf != EGL_NO_SURFACE)
                eglDestroySurface(disp, wSurf);
            freeW(env);
            cRef = env->NewGlobalRef(surf);
            cWin = ANativeWindow_fromSurface(env, surf);
            wSurf = eglCreateWindowSurface(disp, cfg, cWin, 0);
        }
        if (wSurf != EGL_NO_SURFACE)
            eglMakeCurrent(disp, wSurf, wSurf, ctx);
        else if (ctx != EGL_NO_CONTEXT)
            eglMakeCurrent(disp, pbs, pbs, ctx);
        if (!cP)
        {
            const char *cs = "#version 310 es\nprecision mediump float; precision mediump image2D; precision mediump sampler3D;\nlayout(local_size_x=16, local_size_y=16) in;\nlayout(binding=0, rgba8) uniform readonly image2D iT;\nlayout(binding=1, rgba8) uniform writeonly image2D oT;\nuniform sampler3D s; uniform int h; void main() {\nivec2 p=ivec2(gl_GlobalInvocationID.xy); if(p.x>=imageSize(iT).x || p.y>=imageSize(iT).y) return;\nvec4 c=imageLoad(iT, p); if(h==1) imageStore(oT, p, vec4(texture(s, c.rgb).rgb, c.a)); else imageStore(oT, p, c); }";
            GLuint s = glCreateShader(GL_COMPUTE_SHADER);
            glShaderSource(s, 1, &cs, 0);
            glCompileShader(s);
            cP = glCreateProgram();
            glAttachShader(cP, s);
            glLinkProgram(cP);
            glDeleteShader(s);
            hL = glGetUniformLocation(cP, "h");
            lS = glGetUniformLocation(cP, "s");
        }
        if (!qP)
        {
            const char *vs = "#version 300 es\nlayout(location=0) in vec2 p; layout(location=1) in vec2 u; out vec2 v; void main() { v=u; gl_Position=vec4(p,0,1); }", *fs = "#version 300 es\nprecision mediump float; uniform sampler2D t; in vec2 v; out vec4 o; void main() { o=texture(t, vec2(1.0-v.y, 1.0-v.x)); }";
            GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
            glShaderSource(v, 1, &vs, 0);
            glCompileShader(v);
            glShaderSource(f, 1, &fs, 0);
            glCompileShader(f);
            qP = glCreateProgram();
            glAttachShader(qP, v);
            glAttachShader(qP, f);
            glLinkProgram(qP);
        }
        if (tw != w || th != h)
        {
            if (fT)
                glDeleteTextures(1, &fT);
            if (oT)
                glDeleteTextures(1, &oT);
            if (fbo)
                glDeleteFramebuffers(1, &fbo);
            if (pbos[0])
                glDeleteBuffers(2, pbos);
            glGenTextures(1, &fT);
            glBindTexture(GL_TEXTURE_2D, fT);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glGenTextures(1, &oT);
            glBindTexture(GL_TEXTURE_2D, oT);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, oT, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glGenBuffers(2, pbos);
            for (int i = 0; i < 2; i++)
            {
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos[i]);
                glBufferData(GL_PIXEL_UNPACK_BUFFER, w * h * 4, 0, GL_STREAM_DRAW);
            }
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            tw = w;
            th = h;
        }
        if (!lT)
        {
            glGenTextures(1, &lT);
            glBindTexture(GL_TEXTURE_3D, lT);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos[pIdx]);
        void *ptr = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, w * h * 4, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
        if (ptr)
        {
            if (rs == w * 4)
                memcpy(ptr, cd, w * h * 4);
            else
                for (int y = 0; y < h; y++)
                    memcpy((uint8_t *)ptr + y * w * 4, cd + y * rs, w * 4);
            glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        }
        glBindTexture(GL_TEXTURE_2D, fT);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        pIdx = (pIdx + 1) % 2;
        if (ld && lsz > 0)
        {
            if (up)
            {
                glBindTexture(GL_TEXTURE_3D, lT);
                std::vector<uint8_t> b(lsz * lsz * lsz * 3);
                for (size_t i = 0; i < b.size(); i++)
                    b[i] = (uint8_t)(std::max(0.0f, std::min(255.0f, ld[i] * 255.0f)));
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB8, lsz, lsz, lsz, 0, GL_RGB, GL_UNSIGNED_BYTE, b.data());
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            }
            act = true;
        }
        else
            act = false;
        glUseProgram(cP);
        glUniform1i(hL, act ? 1 : 0);
        glBindImageTexture(0, fT, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
        glBindImageTexture(1, oT, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_3D, lT);
        glUniform1i(lS, 2);
        glDispatchCompute((w + 15) / 16, (h + 15) / 16, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        if (wSurf != EGL_NO_SURFACE)
        {
            int ww = ANativeWindow_getWidth(cWin), wh = ANativeWindow_getHeight(cWin);
            glViewport(0, 0, ww, wh);
            glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(qP);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, oT);
            glUniform1i(glGetUniformLocation(qP, "t"), 0);
            float wr = (float)ww / wh, cr = (float)th / tw, sx = 1, sy = 1;
            if (cr > wr)
                sx = cr / wr;
            else
                sy = wr / cr;
            float v[] = {-sx, -sy, 0, 0, sx, -sy, 1, 0, -sx, sy, 0, 1, sx, sy, 1, 1};
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            eglSwapBuffers(disp, wSurf);
        }
        else
        {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, od);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
    }

private:
    EGLDisplay disp = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface pbs = EGL_NO_SURFACE;
    EGLConfig cfg;
    GLuint cP = 0, qP = 0, fT = 0, oT = 0, lT = 0, fbo = 0, pbos[2] = {0, 0};
    int pIdx = 0, tw = 0, th = 0;
    bool act = false, init = false;
    EGLSurface wSurf = EGL_NO_SURFACE;
    jobject cRef = nullptr;
    ANativeWindow *cWin = nullptr;
    Gpu() {}
    void freeW(JNIEnv *env)
    {
        if (cWin)
            ANativeWindow_release(cWin);
        cWin = nullptr;
        if (cRef && env)
            env->DeleteGlobalRef(cRef);
        cRef = nullptr;
    }
};

static jobject lLArr = nullptr;

extern "C" JNIEXPORT jdouble JNICALL
Java_com_example_videoprocessingengine_MainActivity_processFrameNativeGPU(JNIEnv *env, jobject, jobject iBuf, jobject oBuf, jint w, jint h, jint rs, jfloatArray lArr, jint lSz, jobject surface)
{
    auto start = std::chrono::high_resolution_clock::now();
    uint8_t *in = (uint8_t *)env->GetDirectBufferAddress(iBuf), *out = (uint8_t *)env->GetDirectBufferAddress(oBuf);
    bool up = false;
    if (!lArr)
    {
        if (lLArr)
        {
            env->DeleteGlobalRef(lLArr);
            lLArr = nullptr;
            up = true;
        }
    }
    else if (!lLArr || !env->IsSameObject(lArr, lLArr))
    {
        if (lLArr)
            env->DeleteGlobalRef(lLArr);
        lLArr = env->NewGlobalRef(lArr);
        up = true;
    }
    jfloat *lp = (lArr && lSz > 0) ? env->GetFloatArrayElements(lArr, 0) : nullptr;
    Gpu::get().proc(in, out, w, h, rs, lp, lSz, up, surface, env);
    if (lp)
        env->ReleaseFloatArrayElements(lArr, lp, JNI_ABORT);
    auto end = std::chrono::high_resolution_clock::now();
    return (jdouble)(std::chrono::duration<double, std::milli>(end - start).count());
}
