# Hardware Compatibility & Verification Matrix (`gc550.ko`)

This document details hardware device support, HDMI input compatibility, verified lab test environments, and compliance results for the **AVerMedia Live Gamer Extreme (GC550)** Linux kernel module.

---

## 1. Supported Capture Devices

| Device Model | USB ID (`VID:PID`) | Driver Status | Hardware Architecture & Notes |
|---|---|---|---|
| **AVerMedia Live Gamer Extreme (GC550 / LGX)** | `07ca:4710` | **Supported & Bare-Metal Verified** | Primary GC550 hardware revision (`bcdDevice 2.07`). Features Cypress FX3 USB 3.0 controller, Analog Devices ADV7604 (HDMI RX) and ADV7511 (HDMI TX), Lattice ECP3 FPGA, TI TLV320AIC3x audio codec, and PCA9539A GPIO expander. Verified over 3.6+ hours continuous 1080p60 capture (776,295 frames, 0 desyncs) + 48 kHz ALSA audio. |
| **AVerMedia Live Gamer Extreme (GC550 / LGX)** | `07ca:0550` | **Supported (Alias Included)** | Alternate USB Product ID documented for GC550 hardware. Uses identical USB endpoints, firmware bootstrap, and I2C address map. |
| **AVerMedia Live Gamer Extreme 2 (GC551 / LGX2)** | `07ca:0551` | **Not Supported in `gc550.ko`** | Features an ITE IT6664 HDMI 2.0 4K front-end splitter (`0x2C`/`0x58`) with different initialization sequences. Use upstream [**`ChrisAJS/lgx2userspace`**](https://github.com/ChrisAJS/lgx2userspace) for GC551 support. |
| **AVerMedia Live Gamer Ultra (GC553)** | `07ca:0553` | **N/A (Standard UVC)** | Uses standard USB Video Class (UVC). Supported natively by the in-tree Linux `uvcvideo` driver. |

---

## 2. HDMI Input Source Compatibility

The GC550 internal receiver is an **Analog Devices ADV7604** (HDMI 1.4a SDR).

| Source Signal Type | Status | Technical Details |
|---|---|---|
| **1920×1080p @ 60/59.94 Hz (SDR)** | **Supported & Verified** | Uncompressed `YUYV` 4:2:2 (Rec. 709 limited range). Full 30-minute and 3.6-hour continuous captures verified with 0 frame desyncs. |
| **1280×720p @ 60/59.94 Hz (SDR)** | **Supported & Verified** | Uncompressed `YUYV` 4:2:2. Functional smoke test verified with Raspberry Pi 3B HDMI generator. |
| **720×576p @ 50 Hz (PAL SDR)** | **Supported & Verified** | Uncompressed `YUYV` 4:2:2. Functional capture verified; observed at ~50 fps with 0 desyncs. |
| **720×480p @ 60/59.94 Hz (NTSC SDR)** | **Supported & Verified** | Uncompressed `YUYV` 4:2:2. Functional capture verified with Raspberry Pi 3B. |
| **640×480p @ 60 Hz (VGA SDR)** | **Supported & Verified** | Uncompressed `YUYV` 4:2:2. Functional smoke test verified. |
| **RGB & YCbCr 4:4:4 / 4:2:2 (SDR)** | **Supported** | `ADV7604 IO[0x02]` is automatically set to `0x0F` (automatic color-space selection from HDMI AVI InfoFrame). |
| **HDR10 / HLG / Dolby Vision (BT.2020)** | **Unsupported (Hardware Limit)** | The ADV7604 does not support HDMI 2.0a HDR metadata or 10-bit colorimetry. Turn **HDR OFF** on your source console or PC. |
| **HDCP-Protected HDMI Sources** | **Unsupported (By Design)** | Encrypted HDCP streams (e.g. PS4/PS5 with HDCP enabled, MacBooks, streaming apps) cannot be captured. Disable HDCP in console system settings. |

---

## 3. Host USB Controller Requirements

- **USB 3.0 SuperSpeed (5 Gbps) Required:** Uncompressed 1080p60 YUYV video requires ~250 MB/s continuous USB transfer bandwidth. Connecting the card to a USB 2.0 port or using a USB 2.0 cable (`480M`) will cause `gc550.ko` to log a warning in `dmesg` and fail probe gracefully.
- **Supported Host Controllers:** Tested extensively on AMD Promontory / X570 xHCI host controllers and Intel Sunrise Point-LP / Tiger Lake xHCI controllers.

---

## 4. Verified Baseline & Hardware Test Environments

Hardware validation was conducted across two distinct bare-metal environments and a virtualized QEMU/KVM reference setup:

### 4.1 Primary Test Platform (`Titan`)
- **CPU:** AMD Ryzen 9 5900X (12 cores / 24 threads @ 3.7 GHz base, 4.8 GHz boost)
- **GPU:** AMD Radeon RX 9070 XT
- **RAM:** 32 GB DDR4-3600 CL16
- **OS:** CachyOS x86_64 (Arch Linux derived)
- **Kernel:** `7.3.3-1-cachyos` / `7.3.4-1-cachyos` (x86_64)
- **Toolchain:** Clang / LLVM 19.x & 23.x (`CONFIG_CC_IS_CLANG=y`)
- **Audio Stack:** PipeWire 1.2.x + WirePlumber

### 4.2 Secondary Test Platform (`Niri`)
- **CPU:** Intel Core i7 (mobile x86_64)
- **GPU:** Intel Iris Plus Graphics
- **OS:** CachyOS x86_64
- **Kernel:** `7.3.1-1-cachyos` (x86_64)
- **Display Server:** Wayland / KWin

### 4.3 Reference HDMI Sources
- **Raspberry Pi 3B / 4B:** Programmable HDMI generator outputting clean 1080p60, 720p60, 576p50, 480p60, and custom patterns via `vc4-kms-v3d`.
- **Nintendo Switch:** 1080p60 SDR gaming output.
- **PC Secondary Output:** NVIDIA / AMD HDMI output.

---

## 5. Test Results & Compliance Attestation

### 5.1 V4L2 Compliance Suite (`v4l2-compliance 1.32.0`)
Tested against `/dev/avermedia-gc550` on Titan (`7.3.3-1-cachyos`):
- **Non-streaming tests:** **48 passed**, 0 failed.
- **Streaming tests (MMAP, USERPTR, REQBUFS, CREATE_BUFS):** **57 passed**, 0 failed, 0 warnings.
- **Monotonic Timestamps:** Verified via `ktime_get_ns()` across all buffers.
- **Sequence Numbering:** Monotonically increasing across both `DONE` and `ERROR` buffers.
- **SAV Alignment:** Buffer reservation locked strictly to SAV boundaries (`video_pos == 0`).
- **DMABUF Export:** Unverified (requires `--expbuf-device`).

### 5.2 Phase E1.6 Hardware Test Summary (Titan)
- **30-Minute Endurance Capture (TE-01):** **108,000 frames dequeued**, 108,001 driver-observed, **0 dropped**, **0 mid-stream desyncs** over 1,799.985 s at 60.001 fps (345,598,308 audio bytes parsed).
- **Multi-Resolution Functional Checks (TR-01..TR-05):** Short smoke tests across 1080p60 (300 frames), 720p60 (62 frames), 576p50 (51 frames), 480p60 (60 frames), and 640×480p60 (62 frames). All captured with 0 desyncs and correct pixel dimensions.
- **Simultaneous Video & Audio Capture (TS-01):** 1,800 video frames and 5,750,784 PCM bytes over 30.0 s, demonstrating **99.84% audio/video duration correspondence**.
- **Physical Hot-Unplug / Reconnect:** Tested via software unbind/bind and confirmed clean device destruction via `struct kref`; physical cable unplug under active streaming was validated on Niri (`29,629` frames captured before disconnect without kernel fault).
