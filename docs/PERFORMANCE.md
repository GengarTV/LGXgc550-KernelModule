# Performance, Latency & Architectural Payoff (`gc550.ko`)

This document analyzes the latency characteristics, memory throughput, and architectural performance advantages of the native Linux kernel driver (`gc550.ko`) compared to userspace implementations (`libusb` + `v4l2loopback`).

---

## 1. Glass-to-Glass Latency Analysis

### 1.1 Project Latency Target
The design target for `gc550.ko` is **$\le 50\text{ ms}$ end-to-end glass-to-glass latency** at 1080p60 without sacrificing stream stability or frame pacing.

### 1.2 Point-in-Time Measurement (Niri Reference Rig)
During baseline testing on the Niri laptop (Intel Core i7, CachyOS `7.3.1-1-cachyos`, Wayland/KWin), a millisecond count-up timer was displayed on a reference monitor via the GC550's zero-latency `HDMI OUT` passthrough (`50.283 s`) and compared against an `mpv` live preview window flushed to the live edge (`50.233 s`):

$$\text{Glass-to-Glass Latency} = 50.283\text{ s} - 50.233\text{ s} = \mathbf{50\text{ ms}}\quad (3 \times 16.67\text{ ms}\text{ frames at } 60\text{ Hz})$$

### 1.3 Analytical Three-Frame Pipeline Breakdown
The observed 50 ms latency decomposes into a standard 3-frame pipeline:

| Stage | Duration | Underlying Mechanism |
|---|---|---|
| **Frame 1: Scanout & USB DMA** | `~16.7 ms` | The GC550 hardware scans out 1080 lines of HDMI video (4.14 MB) over USB 3.0 Bulk `EP 0x83 IN`. The final `EAV` marker arrives on the trailing 512 KiB URB and triggers `vb2_buffer_done()`. |
| **Frame 2: GPU Shader & Compositor** | `~16.7 ms` | Userspace (`mpv` or OBS) uploads the `YUYV` frame via OpenGL/Vulkan shaders, converts `YUYV → RGB`, and submits the frame to the Wayland/X11 compositor. |
| **Frame 3: Display VSync & Scanout** | `~16.7 ms` | The compositor presents the surface on the next 60 Hz vertical blanking interval, and the display panel scans out the pixels. |

*Driver Capture Latency Estimate:* Because the driver operates with small 512 KiB URBs (`~1.5–2.5 ms` transfer interval), the driver's internal processing overhead is negligible ($< 2\text{ ms}$), achieving the theoretical minimum capture delay of ~1 frame scanout duration.

---

## 2. Kernel Module (`gc550.ko`) vs. Userspace Daemon Pipeline Comparison

Capturing uncompressed 1080p60 `YUYV` video requires sustaining **~248.8 MB/s** of raw video data plus interleaved audio packets. Moving the parser into the kernel drastically reduces CPU cache pollution, memory bandwidth, and scheduling latency:

| Metric / Architectural Feature | Userspace Architecture (`libusb` → `v4l2loopback`) | Native Kernel Module (`gc550.ko`) |
|---|---|---|
| **USB Transfer Granularity** | `2,080,768 B` (`~2 MiB`, half a frame per transfer) | **`512 KiB`** (`1/8th` of a frame per URB) |
| **`EAV` Frame-Completion Delay** | Up to **`~8.3 ms`** (waits for full 2 MiB buffer into next frame) | **`~1.5–2.5 ms`** (completes immediately on 512 KiB boundary) |
| **Memory Copies (into OBS / mpv)** | **3–4 copies per frame** (`libusb` → `BufferReorderer` → `FrameBuilder` → `v4l2loopback` → OBS; **~1.0 GB/s** RAM bandwidth) | **1 copy** (Kernel URB buffer → `vb2` mmap buffer mapped directly into OBS; **~250 MB/s** RAM bandwidth) |
| **Context Switches per Frame** | Multiple Kernel ↔ Userspace round-trips per frame | **Zero** intermediate userspace daemon hops |
| **Audio Routing Pipeline** | Internal SDL3 queue; requires virtual sink setup to route to OBS | **Native ALSA Card (`hw:GC550Audio,0`)** visible to PipeWire / PulseAudio |
| **Plug-and-Play Experience** | Manual CLI launch; claims raw USB interface via `libusb` | **100% Plug-and-Play** (udev auto-loads module on plug-in) |
| **Fault Isolation Model** | Userspace isolated (daemon crash only kills capture process) | Ring-0 execution (driver requires rigorous correctness hardening) |

---

## 3. High-Throughput Memory Bandwidth Payoff

By eliminating intermediate daemon buffers and the `v4l2loopback` pipe, `gc550.ko` eliminates approximately **750 MB/s** of redundant DRAM traffic:
- **Userspace Daemon Approach:**
  $$\text{DRAM Bandwidth} \approx 4 \times 248.8\text{ MB/s} \approx \mathbf{995.2\text{ MB/s}}$$
- **Native Kernel Driver (`gc550.ko`):**
  $$\text{DRAM Bandwidth} \approx 1 \times 248.8\text{ MB/s} \approx \mathbf{248.8\text{ MB/s}}$$

On systems with integrated graphics (e.g. laptops, handheld PCs) where CPU and GPU share memory bandwidth, saving ~750 MB/s of bus traffic translates directly into smoother frame pacing, reduced thermal throttling, and lower CPU utilization.

---

## 4. Long-Duration Endurance Validation

- **Titan 30-Minute Endurance Test (TE-01):**
  - **Total Frames Dequeued:** `108,000`
  - **Driver-Observed Frames:** `108,001`
  - **Dropped Frames:** `0`
  - **Mid-Stream Desyncs:** `0`
  - **Effective Frame Rate:** `60.001 fps`
  - **Total Audio Bytes Parsed:** `345,598,308 bytes`
- **Niri 3.6-Hour Long-Duration Test:**
  - **Total Frames Captured:** `776,295` continuous frames
  - **Frame Desyncs:** `0`
  - **Audio XRUNs:** `0`
