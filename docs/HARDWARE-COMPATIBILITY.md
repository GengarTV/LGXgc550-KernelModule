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
- **Kernel:** `7.3.3-1-cachyos` / `7.3.4-1-cachyos` / `7.3.5-2-cachyos` (x86_64)
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

### 5.1 V4L2 Compliance Suite (`v4l2-compliance 1.32.0` — Historical E1.5.2.2 Baseline)
Tested against `/dev/avermedia-gc550` on Titan (`7.3.3-1-cachyos`, pre-U2-B02 E1.5.2.2 baseline):
- **Non-streaming tests:** **48 passed**, 0 failed.
- **Streaming tests (MMAP, USERPTR, REQBUFS, CREATE_BUFS):** **57 passed**, 0 failed, 0 warnings.
- **Monotonic Timestamps:** Verified via `ktime_get_ns()` across all buffers.
- **Sequence Numbering:** Monotonically increasing across both `DONE` and `ERROR` buffers.
- **SAV Alignment:** Buffer reservation locked strictly to SAV boundaries (`video_pos == 0`).
- **DMABUF Export:** Unverified (requires `--expbuf-device`).
- *(Note: `v4l2-compliance` has **not** yet been re-run on the unified U2-B02 working tree; known open finding `REM-B02-002` — DV timing enumeration vs. `gc550_frame_sizes[]` acceptance — is empirically confirmed and expected to affect `testTimings()` until remediated.)*

### 5.2 Phase E1.6 Hardware Test Summary (Titan — Historical Baseline)
- **30-Minute Endurance Capture (TE-01):** **108,000 frames dequeued**, 108,001 driver-observed, **0 dropped**, **0 mid-stream desyncs** over 1,799.985 s at 60.001 fps (345,598,308 audio bytes parsed).
- **Multi-Resolution Functional Checks (TR-01..TR-05):** Short smoke tests across 1080p60 (300 frames), 720p60 (62 frames), 576p50 (51 frames), 480p60 (60 frames), and 640×480p60 (62 frames). All captured with 0 desyncs and correct pixel dimensions.
- **Simultaneous Video & Audio Capture (TS-01):** 1,800 video frames and 5,750,784 PCM bytes over 30.0 s, demonstrating **99.84% audio/video duration correspondence**.
- **Physical Hot-Unplug / Reconnect:** Tested via software unbind/bind and confirmed clean device destruction via `struct kref`; physical cable unplug under active streaming was validated on Niri (`29,629` frames captured before disconnect without kernel fault).

### 5.3 U2-B01-S3 Gate 2 Hardware Validation (Titan, `7.3.5-2-cachyos` — S3 Artifact)
Tested on Titan with the compacted U2-B01-S3 artifact (`srcversion: F322D907D7325C1D83A468C`, `subdev_mode=1`, 27 stages / 2,615 commands / 20,082-byte `gc550_init_blob`):
- **Cold Bootstrap:** Completed in `1,146.62 ms` with `0` USB/I2C errors.
- **Multi-Resolution Gates (C1–C5):** `1920x1080p60`, `1280x720p60`, `720x576p50`, `720x480p59.94`, and `640x480p60` all passed native `1:1` `mpv` visual gates and 120-frame MMAP telemetry (`0` dropped frames, `0` desyncs, `swap_mode=0`, $Y \in [16, 235]$).
- **5-Minute 1080p60 + 48 kHz Audio Stress Test (Phase D):** `33,503` video frames (`60.00 fps`), `107,204,980 B` demuxed audio, `0` ALSA XRUNs, `0` desyncs, `swap_mode=0`.

### 5.4 U2-B02-S3 Gate 2 Hardware Validation (Titan, `7.3.5-2-cachyos` — Unified Architecture Artifact, Phases B–E PASS)
Tested on Titan with the unified U2-B02-S1/S2 artifact (`srcversion: 89F85911AAB5DAEF764B8EF`, loaded **without module parameters**):
- **Phase B — Unified Architecture Bootstrap (`PASS`):** Loaded without parameters (`/sys/module/gc550/parameters` absent; `subdev_mode` removed). `adv7604 13-0020` and `adv7511-v4l2 13-003d` registered unconditionally; `VIDIOC_LOG_STATUS` confirmed `adv7511-v4l2 13-003d: power on` (post-probe `s_power(1)` restoration verified).
- **Phase C — Media Controller Topology (`PASS`):** `media-ctl -d /dev/media1 -p` confirmed 3 entities, 0 orphan entities, and both `[ENABLED,IMMUTABLE]` links from `"adv7604 13-0020":6` to `"AVerMedia GC550":0` (`/dev/video2`) and `"adv7511-v4l2 13-003d":0`.
- **Phase D — Read-Only V4L2 Validation (`PASS`):** Verified live HDMI lock (`1920x1080p60`, VIC 16), `VIDIOC_ENUM_DV_TIMINGS` / `VIDIOC_DV_TIMINGS_CAP` pad validation (`pad = 0` accepted; `pad = 1`, `2`, `0xffffffff` rejected with `-EINVAL`), format restoration to `1920x1080 YUYV`, and empirical confirmation of `REM-B02-002` (`73` enumerated timings; `15` accepted and `58` rejected with `-EINVAL` by `VIDIOC_S_DV_TIMINGS`).
- **Phase E — Single Controlled `1920x1080p60 YUYV` Video Capture & Visual Gate (`PASS`):** Native `1:1` `mpv --video-unscaled=yes --no-audio` presentation (`Video scale: 1.000000/1.000000`) approved by operator (`"Looks perfect"`). Kernel stream teardown logged `frames=6685, audio_bytes=21391700, desyncs=0, swap_mode=0` with `0` USB, I2C, V4L2, or parser errors.
- **Explicitly Not Tested Under U2-B02-S3 Gate 2 Phases B–E:** Additional capture resolutions (`720p`, `576p`, `480p`, `640x480`), ALSA audio capture, combined A/V validation, OBS capture, multi-minute stress testing, and `v4l2-compliance` re-execution on the unified U2-B02 artifact remain **not tested** (detailed execution records are preserved externally under `~/Projects/AGY Documentation/`).

### 5.5 U2-B03-S3 Gate 2 Hardware Validation (Titan, `7.3.5-2-cachyos` — `GC550_DIAG` Removed, Verdict: `ACCEPTED`)
Tested on Titan with the U2-B03-S1 artifact (`srcversion: 2CC7B9DF54D327C29B960DF`, `SHA-256: 45a00af8181b3c9bb22ced6ba08993ae03a98aca7205c7c64e154f48651e48e3`, loaded **without module parameters**; independently audited in U2-B03-S2 with verdict `ACCEPTED WITH FINDINGS` — `0` BLOCKER/HIGH/MEDIUM, `2` deferred LOW findings):
- **Parameter-Free Probe & Footprint (`PASS`):** Loaded without parameters (`/sys/module/gc550/parameters` absent, in-memory `coresize: 94,208 B` vs. `118,784 B` prior to `GC550_DIAG` removal; `20.00 MiB` probe-time ring allocation eliminated). Warm board-state detection confirmed `ADV7513 Main[0x41]=0x10` and reused existing hardware state cleanly.
- **Media Controller Topology (`PASS`):** `media-ctl -d /dev/media1 -p` confirmed 3 entities (`adv7604 13-0020`, `adv7511-v4l2 13-003d`, `AVerMedia GC550`), both `[ENABLED,IMMUTABLE]` fan-out links from `"adv7604 13-0020":6`, and `0` orphan entities.
- **`VIDIOC_LOG_STATUS` (`PASS`):** Confirmed native GC550 stream summary (`Stream: active=0, fmt=1920x1080, frames=0, audio_bytes=0, desyncs=0, swap=0`) alongside locked `1920x1080p60` `adv7604` and `adv7511-v4l2: power on`.
- **Controlled `1920x1080p60 YUYV` Video-Only Capture & Visual Gate (`PASS`):** Exercised the direct `urb->context = dev` path in one video-only native `1:1` `mpv --video-unscaled=yes --no-audio` session (`Video scale: 1.000000/1.000000`, `~62.4 s`). Operator visual checkpoint approved (`"looking good"`). Clean teardown logged `Stream stopped: frames=3743, audio_bytes=11977432, desyncs=0, swap_mode=0` with `0` USB, V4L2, parser, or `mpv` errors; module `refcnt` returned to `1` and ALSA capture PCM remained `closed`.
- **Explicitly Not Tested Under U2-B03-S3:** Audio capture, combined A/V validation, multi-resolution capture (`720p`/`576p`/`480p`/`640x480`), stress testing, cold USB power-cycle bootstrap replay under S1, `v4l2-compliance`, and other kernels or hardware remain **not tested** under U2-B03-S3.
