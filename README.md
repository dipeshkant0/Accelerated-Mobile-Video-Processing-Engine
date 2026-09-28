# Accelerated Video Processing Engine for Android

![Platform](https://img.shields.io/badge/Platform-Android%2010%2B%20(API%2029--35)-3DDC84?logo=android&logoColor=white)
![Language](https://img.shields.io/badge/Language-C%2B%2B17%20%7C%20Kotlin-00599C?logo=cplusplus&logoColor=white)
![SIMD](https://img.shields.io/badge/ARM-NEON%20SIMD-0091BD?logo=arm&logoColor=white)
![GPU](https://img.shields.io/badge/GPU-OpenGL%20ES%203.1%20Compute-76B900?logo=opengl&logoColor=white)
![Institute](https://img.shields.io/badge/IIT%20Delhi-CSE%20Department-B31B1B)

> **A zero-copy, thermally-aware real-time 1080p 3D LUT video processing engine for Android utilizing C++17, ARM NEON SIMD vectorization, OpenGL ES 3.1 Compute Shaders, and an intelligent Hybrid CPU+GPU orchestrator.**

---

## Authors & Affiliation

* **Dipesh Kant** (`2025MCS2110`) — *Core Systems, Zero-Copy JNI, ARM NEON SIMD & Custom Thread Pool*
* **Kunal Dhyani** (`2025MCS2112`) — *Android UI, Camera Pipeline, OpenGL ES 3.1 GPU Compute & Hybrid Orchestrator*

**Department of Computer Science and Engineering**  
**Indian Institute of Technology (IIT) Delhi**

---

## Abstract

Processing high-quality video in real-time is a major challenge on mobile devices due to strict memory bandwidth constraints and passive (fanless) thermal dissipation. Pushing a mobile SoC too hard causes rapid overheating and severe OS-level thermal throttling.

This project implements a high-performance, thermally-aware video processing engine for **Android 10+ (up to Android 15)** that applies **3D cinematic color grading filters (.cube 3D LUTs)** to live $1920 \times 1080$ (FHD) camera feeds at $30\text{ FPS}$. By bypassing the Java Virtual Machine (JVM) memory copy overhead and offloading heavy trilinear interpolation math to native **C++17**, we engineered three distinct compute pipelines:
1. **Baseline Scalar CPU Pipeline**
2. **Multi-Core ARM NEON SIMD Vectorized Pipeline**
3. **OpenGL ES 3.1 GPU Compute Shader Pipeline**

On top of these pipelines, our **Hybrid Mode Orchestrator** monitors battery level and device temperature in real time, dynamically switching between GPU and SIMD CPU execution to prevent thermal throttling while maintaining a smooth $30\text{ FPS}$ video stream.

---

## The Problem: Mobile Memory Wall & Thermal Throttling

Applying a 3D color filter to a live $1080\text{p}$ video stream requires recalculating colors for over $2\text{ million}$ pixels ($1920 \times 1080 = 2,073,600$ pixels), $30$ times every second. This demands **over $1.3\text{ billion}$ floating-point operations per second (FLOP/s)**.

Executing this naively on a standard Android pipeline hits two critical walls:
* **The Memory Wall:** Moving $8\text{ MB}$ $1080\text{p}$ RGBA frames back and forth between the camera sensor, JVM byte arrays, and native C++ memory saturates RAM bandwidth and triggers frequent Garbage Collection (GC) pauses.
* **Thermal Throttling:** Continuous scalar execution forces the CPU into its highest power state (P-state), generating excessive heat until the Android OS throttles clock speeds.

### Our Three Core Design Rules
1. **Zero Memory Copies:** Read pixel data directly from the camera sensor's physical memory buffer and render directly to the native display window without touching the Java heap.
2. **Hardware-Specific Acceleration:** Match the mathematical workload to specialized silicon—128-bit ARM NEON vector units on the CPU and 3D texture filtering hardware on the GPU.
3. **Thermal & Battery Failsafe:** Continuously monitor hardware telemetry to shift workloads proactively before OS thermal throttling occurs.

---

## Mathematical Foundation: 3D Color LUTs & Trilinear Interpolation

A **3D Look-Up Table (3D LUT)** maps an input camera RGB color to a graded cinematic output RGB color. Standard 8-bit video contains $256^3 \approx 16.7\text{ million}$ possible color combinations, which would require a massive $\sim 50\text{ MB}$ lookup table. Instead, industry-standard `.cube` files store a sparse lattice grid—typically $33 \times 33 \times 33$ or $16 \times 16 \times 16$ points ($\sim 430\text{ KB}$)—and interpolate intermediate colors at runtime.

### 1. Normalizing & Scaling Input Pixels to the LUT Grid
For a $33 \times 33 \times 33$ grid (indices $0$ to $32$), each incoming 8-bit pixel channel $(R_{in}, G_{in}, B_{in}) \in [0, 255]$ is mapped to continuous grid coordinates $(x, y, z)$:

$$x = \left(\frac{R_{in}}{255.0}\right) \cdot 32, \quad y = \left(\frac{G_{in}}{255.0}\right) \cdot 32, \quad z = \left(\frac{B_{in}}{255.0}\right) \cdot 32$$

### 2. The 8-Corner Bounding Cube
Using the integer floor and ceiling of $(x, y, z)$, we locate the $8$ surrounding lattice points forming a unit cube around the pixel, denoted from $C_{000}$ (bottom-left-front corner) to $C_{111}$ (top-right-back corner), along with the fractional distances $(d_x, d_y, d_z) \in [0, 1)$:

$$d_x = x - \lfloor x \rfloor, \quad d_y = y - \lfloor y \rfloor, \quad d_z = z - \lfloor z \rfloor$$

### 3. Trilinear Interpolation Equation
To blend the $8$ corner colors, we execute $7$ sequential 1D linear interpolations across the $X$, $Y$, and $Z$ axes:
* **Step 1 ($X$-Axis):** Blend the $4$ pairs of corners along the $X$-axis using weight $d_x$.
* **Step 2 ($Y$-Axis):** Blend the resulting $4$ points into $2$ points along the $Y$-axis using weight $d_y$.
* **Step 3 ($Z$-Axis):** Blend the final $2$ points along the $Z$-axis using weight $d_z$.

For each color channel, the expanded trilinear interpolation equation for the final output color $C_{final}$ is:

$$C_{final} = (1 - d_z)\left[(1 - d_y)\left((1 - d_x)C_{000} + d_x C_{100}\right) + d_y\left((1 - d_x)C_{010} + d_x C_{110}\right)\right] + d_z\left[(1 - d_y)\left((1 - d_x)C_{001} + d_x C_{101}\right) + d_y\left((1 - d_x)C_{011} + d_x C_{111}\right)\right]$$

### 4. Computational Complexity
* **Per Pixel:** $14$ floating-point multiplications + $7$ floating-point additions per color channel ($63$ FLOPs per RGB pixel).
* **At $1080\text{p}$ ($30\text{ FPS}$):** $1920 \times 1080 \times 30 \times 21 \approx 1.3\text{ Billion FLOP/s}$.

---

## System Architecture: Zero-Copy JVM Bypass

In a conventional Android camera app, copying an $8\text{ MB}$ $1080\text{p}$ frame from the camera into a Java `byte[]` array and marshaling it across JNI to C++ introduces crippling latency and memory pressure.

Our architecture completely bypasses the Java heap:

```text
+-----------------------+      GetDirectBufferAddress      +------------------------------+
|  Android Camera2/X    | ==============================>  |   Native C++17 Engine        |
|  (AHardwareBuffer)    |    (Zero-Copy Memory Pointer)    |                              |
+-----------------------+                                  |  +------------------------+  |
                                                           |  | 1. Baseline Scalar C++ |  |
+-----------------------+         Battery & Thermal        |  | 2. ARM NEON SIMD + Pool|  |
|   BroadcastReceiver   | ------------------------------>  |  | 3. OpenGL ES 3.1 GPU   |  |
| (Temp & Battery %)    |       Hybrid Orchestrator        |  +------------------------+  |
+-----------------------+                                  +--------------+---------------+
                                                                          |
                                                                          | Direct Render
                                                                          v
                                                           +------------------------------+
                                                           |        ANativeWindow         |
                                                           |      (Display Surface)       |
                                                           +------------------------------+
```

1. **Direct Buffer Access:** Using Android's `AHardwareBuffer` and JNI `GetDirectBufferAddress`, Kotlin passes only the raw physical memory pointer of the camera frame to the C++ engine.
2. **In-Place / Native Processing:** The C++ engine processes the frame via the selected pipeline (Scalar, NEON SIMD, or OpenGL ES 3.1 PBO DMA).
3. **Direct Surface Rendering:** Processed pixels are pushed straight to the screen via `ANativeWindow`, ensuring pixel payloads never allocate on the JVM heap.

---

## Execution Pipelines

### 1. Baseline Mode (Scalar C++17)
Our reference implementation using sequential single-instruction, single-data (`SISD`) C++17 loops:
* **Execution:** Extracts a single 8-bit RGBA pixel, converts to floating-point coordinates, fetches the $8$ surrounding LUT corners, computes the trilinear interpolation, and writes the output pixel.
* **Hardware Bottleneck:** High intermediate variable count ($8$ float triplets for corners + $3$ float weights) causes severe **L1 register pressure**, spilling data into the L2 cache and stalling the CPU pipeline ($79.6\text{ ms}$ per frame, $\sim 10.8\text{ FPS}$ at $87\%$ CPU load).

### 2. SIMD Mode (ARM NEON Vectorization + Thread Pool)
Vectorized CPU pipeline utilizing 128-bit **ARM NEON** intrinsics (`<arm_neon.h>`) and custom multi-threading:
* **128-Bit Data Packing:** Loads $16$ contiguous bytes ($4$ complete RGBA pixels) simultaneously into 128-bit NEON registers using `vld1q_u32`.
* **Hardware Fused Multiply-Add (FMA):** Maps $A + (B \times C)$ operations directly to the `vmlaq_f32` intrinsic, executing multiplication and addition in a single clock cycle.
* **Custom Singleton Thread Pool & Core Affinity:** Avoids OS thread creation/destruction syscall overhead per frame. Memory segments are cache-line aligned to prevent **false sharing** across L1 caches, and worker threads are pinned via CPU affinity masks to the SoC's high-performance ("big") cores rather than efficiency ("LITTLE") cores.

### 3. GPU Mode (OpenGL ES 3.1 Compute Shaders)
Full hardware offload leveraging mobile GPU parallelism and dedicated texture filtering silicon:
* **Asynchronous DMA (PBO Ping-Pong):** Implements double-buffered Pixel Buffer Objects (PBOs). While the GPU reads Frame $N$ from Buffer A via Direct Memory Access (DMA), the CPU writes Frame $N+1$ into Buffer B, eliminating synchronous CPU-GPU stalls.
* **Spatial Workgroups:** Dispatches modern OpenGL ES 3.1 Compute Shaders (`glDispatchCompute`) partitioned into $16 \times 16$ local workgroups.
* **Hardware-Level Trilinear Filtering:** Binds the 3D LUT as a hardware `GL_TEXTURE_3D` volume. Calling `sampler3D` in GLSL offloads the entire trilinear interpolation equation to the GPU's dedicated Texture Mapping Units (TMUs), achieving an average frame compute latency of **$4.6\text{ ms}$**.

### 4. The Hybrid Orchestrator (Auto Meta-Mode)
Continuous GPU or SIMD execution under high ambient temperatures can still heat up a mobile chassis. The **Hybrid Mode** acts as an intelligent runtime state machine that monitors OS hardware broadcasts (`BatteryManager.EXTRA_TEMPERATURE` and `BatteryManager.EXTRA_LEVEL`):

```kotlin
var mode = curMode
if (curMode == ProcessingMode.HYBRID) {
    // Switch to SIMD if temperature >= 40.0 C or battery <= 15% to save power and reduce heat
    mode = if (tempInCelsius < 40.0 && batPct > 15) {
        ProcessingMode.GPU
    } else {
        ProcessingMode.SIMD
    }
}
```

* **Thermal Management ($\ge 40.0^\circ\text{C}$):** Shifts processing from the power-dense GPU back to the 9%-utilization NEON SIMD CPU pipeline, allowing the GPU silicon to cool down without dropping frames.
* **Battery Preservation ($\le 15\%$):** Avoids high GPU peak power draw when battery capacity is critically low.

---

## 📱 Application Interface & Real-Time Controls

The Android app features a live **Heads-Up Telemetry Dashboard** and interactive runtime drop-down menus that switch modes without restarting the camera session:

* **Live Telemetry Overlay:**
  * `MODE`: Active pipeline (`BASELINE`, `SIMD`, `GPU`, `HYBRID`)
  * `RES`: Active stream resolution (`720p` / `1080p`)
  * `LAT`: Per-frame compute latency in milliseconds ($\text{ms}$)
  * `FPS`: Real-time rendered frames per second
  * `CPU`: Process CPU utilization ($\%$)
  * `MEM`: Total memory footprint ($\text{MB}$)
  * `THERMAL`: Real-time device temperature ($^\circ\text{C}$)
* **Compute Mode Selection:** `Baseline (Scalar)`, `SIMD (NEON)`, `GPU (OpenGL ES / OpenCL)`, `Hybrid (CPU+GPU)`
* **3D LUT Filter Selection:** `None`, `Teal Orange`, `Black and White`, `Night Vision`, `Thermal`
* **Resolution Selection:** `1280x720 (HD)`, `1920x1080 (FHD)`

---

## Benchmarks & Live Telemetry Results

All benchmarks were recorded live on physical Android hardware running real camera streams:

| Execution Mode | Resolution | Compute Latency ($\text{ms}$) | Frame Rate ($\text{FPS}$) | CPU Usage ($\%$) | Memory ($\text{MB}$) | Temperature ($^\circ\text{C}$) | Speedup vs. Baseline |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Baseline (Scalar C++)** | $1920 \times 1080$ | $79.6\text{ ms}$ | $10.8\text{ FPS}$ | $87\%$ | $363\text{ MB}$ | $39.1^\circ\text{C}$ | $1.00\times$ |
| **SIMD (ARM NEON)** | $1920 \times 1080$ | $23.1\text{ ms}$ | $27.6\text{ FPS}$ | **$9\%$** | $363\text{ MB}$ | $39.1^\circ\text{C}$ | **$3.45\times$** |
| **GPU (OpenGL ES 3.1)** | $1920 \times 1080$ | **$4.6\text{ ms}$** | **$29.9\text{ FPS}$** | $13\%$ | $363\text{ MB}$ | $39.1^\circ\text{C}$ | **$17.30\times$** |
| **Hybrid (SIMD Auto)** | $1280 \times 720$ | $16.9\text{ ms}$ | **$30.7\text{ FPS}$** | **$9\%$** | $383\text{ MB}$ | $35.9^\circ\text{C}$ | — |

### Key Performance Takeaways
* **$17.3\times$ Latency Reduction on GPU:** Frame processing time dropped from $79.6\text{ ms}$ (Baseline) to $4.6\text{ ms}$ (GPU), easily meeting the $33.3\text{ ms}$ budget required for $30\text{ FPS}$ real-time video.
* **$90\%$ Reduction in CPU Load with NEON SIMD:** Packing $4$ RGBA pixels per instruction with `vmlaq_f32` FMA and big-core thread pinning reduced CPU usage from $87\%$ to just $9\%$ while boosting frame rates from $10.8\text{ FPS}$ to $27.6\text{ FPS}$.
* **Zero Memory Leaks:** Across all $1080\text{p}$ modes, memory consumption remained rock-solid at $363\text{ MB}$, validating our zero-copy native buffer architecture.

---

## Testing & Validation Methodology

We validated the engine across five strict verification criteria:
1. **The Math Check (Visual & Numerical Accuracy):** Processed identical test frames across `Baseline`, `SIMD`, and `GPU` pipelines to verify bit-accurate color grading across all 3D LUTs.
2. **The Speed Check (Real-Time Telemetry):** Verified per-frame latency reductions ($79.6\text{ ms} \rightarrow 23.1\text{ ms} \rightarrow 4.6\text{ ms}$) using high-resolution native timers on the live dashboard.
3. **The Leak Check (Long-Run Memory Stability):** Ran continuous $1080\text{p}$ sessions to confirm flat memory usage ($363\text{ MB}$) with zero native heap leaks or buffer overruns.
4. **The Hybrid Check (Thermal Failsafe Trigger):** Subjected the device to sustained heavy load until battery/chassis temperature reached $40.0^\circ\text{C}$, confirming instantaneous, glitch-free fallback from `GPU` to `SIMD` mode.
5. **The OS Check (Cross-Version Compatibility):** Tested across physical devices running **Android 10 (API 29)** through **Android 15 (API 35)**.

---

## Getting Started & Build Instructions

### Prerequisites
* **Android Studio:** Ladybug / Koala or newer
* **Android NDK:** r25c+ (with CMake $3.22.1+$)
* **Target Device:** Physical Android device running **Android 10+ (API 29–35)** with ARM64-v8a architecture (ARM NEON support) and OpenGL ES 3.1+ support.

### Build & Run
1. **Clone the repository:**
   ```bash
   git clone https://github.com/dipeshkant0/video-processing-engine.git
   cd video-processing-engine
   ```
2. **Open in Android Studio:**
   * Open the project root directory in Android Studio and allow Gradle and CMake to sync the native C++ build targets.
3. **Deploy to a Physical Device:**
   * Connect your Android phone via USB debugging and click **Run (`Shift + F10`)**, or install via Gradle:
   ```bash
   ./gradlew installDebug
   ```
4. **Grant Permissions:**
   * Grant Camera permissions on launch and use the top-right menu to switch between **Compute Modes**, **3D LUT Filters**, and **Resolutions** in real time.

---

## AI Usage Declaration

In the spirit of academic transparency: AI coding assistants were used to help scaffold the initial Android application template and verify syntax for specific ARM NEON intrinsics and OpenGL ES 3.1 API bindings. However, the core engineering of this project—including the mathematical optimizations, custom thread pool with core affinity, zero-copy memory pipelines, PBO ping-pong architecture, and hybrid thermal orchestration rules—was designed, implemented, and benchmarked by the authors.
