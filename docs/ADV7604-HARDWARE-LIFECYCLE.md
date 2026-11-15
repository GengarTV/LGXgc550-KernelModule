# ADV7604 Hardware Lifecycle, Register Provenance & V4L2 DV Timings Reference

**Project:** AVerMedia Live Gamer Extreme (GC550) Native Linux Kernel Driver (`lgx2KernelLevel`)  
**Target Hardware:** AVerMedia GC550 (`07ca:4710` / `07ca:0550`)  
**Authoritative Reference:** Phase 2C5D Read-Only Register Provenance Audit & Hardware Validation  
**Evidence Classification:** `Confirmed` (Register dumps, I2C traces, and kernel source audits)

---

## 1. Overview & Purpose

The AVerMedia Live Gamer Extreme (GC550) uses an **Analog Devices ADV7604** HDMI/analog front-end receiver to capture video signals and extract embedded HDMI audio.

During the development of the native Linux kernel driver (`gc550.ko`), the integration of upstream V4L2 subdevice infrastructure (`adv7604.c`), Media Controller topology (`/dev/mediaX`), and V4L2 DV timings ioctls required a rigorous hardware lifecycle investigation.

This document establishes the verified hardware lifecycle and register state progression of the ADV7604 within the GC550 architecture. It serves as the authoritative project reference for:
1. The exact cold-bootstrap register state programmed by `gc550_init_blob`.
2. Post-probe driver behavior and subdevice configuration.
3. The automatic timing negotiation mechanics of userspace `v4l2-ctl`.
4. The physical register changes resulting from `VIDIOC_S_DV_TIMINGS`.
5. The verified Media Controller topology and multi-resolution capture performance.

---

## 2. ADV7604 Cold-Bootstrap Register State

The GC550 hardware is initialized over the Cypress FX3 USB-to-I2C bridge (`EP 0x01 OUT` / `EP 0x81 IN`) by replaying `gc550_init_blob` (53,146 bytes, 6,576 discrete steps in `kernel/gc550-regtables.h`).

An exhaustive, read-only programmatic audit of all 6,576 operations confirmed the exact write points for the primary timing and mode registers:

| Register | Address Map | Cold-Bootstrap Value | Meaning | Source in `gc550_init_blob` |
|---|:---:|:---:|---|---|
| `IO[0x00]` | IO Map (`0x20`) | `0x08` | Video Standard: `HDMI-GR` (Graphics mode) | Step 2681 (Offset 21881): `[0x01, 0x20, 0x01, 0x00, 0x08]` |
| `IO[0x01]` | IO Map (`0x20`) | `0x06` | Primary Mode: `1080p60` graphics mode | Step 2683 (Offset 21897): `[0x01, 0x20, 0x01, 0x01, 0x06]` |
| `CP[0x91]` | CP Map (`0x22`) | `0x50` | Bit 6 set (Interlaced / Scan configuration) | Step 2653 (Offset 21646): `[0x01, 0x22, 0x01, 0x91, 0x50]` |

### Audit Evidence Across Bootstrap Operations:
- Earlier in the bootstrap sequence, intermediate values are written (`IO[0x01] = 0x01` at step 444; `IO[0x01] = 0x06` at step 452; `IO[0x00] = 0x02` at step 454).
- Steps 2653, 2681, and 2683 establish the final bootstrap state.
- Across steps 2684 through 6576, **no further writes** to `IO[0x00]`, `IO[0x01]`, or `CP[0x91]` occur anywhere in `gc550_init_blob`.

### Subsequent Driver Probe & Initialization:
An audit of the driver's subsequent initialization stages confirmed:
1. `gc550_bootstrap_hw()`: Replays `gc550_init_blob` and calls `gc550_set_auto_input_colorspace()` (which adjusts `IO[0x02]` and `CP[0x3A..0x3D]`). It does **not** write to `IO[0x00]`, `IO[0x01]`, or `CP[0x91]`.
2. `adv76xx_probe()` / `adv76xx_core_init()`: Programs sub-maps, interrupts, drive strength (`IO[0x14]`), and format modes (`IO[0x03] = 0x80`, `IO[0x04] = 0x42`). It does **not** write to `IO[0x00]`, `IO[0x01]`, or `CP[0x91]`.
3. `gc550_configure_adv7604()`: Propagates pad format via `adv76xx_set_format()`. It does **not** write to `IO[0x00]`, `IO[0x01]`, or `CP[0x91]`.

**Conclusion:** On a cold system immediately following driver probe and subdevice binding, the hardware registers hold `IO[0x00] = 0x08`, `IO[0x01] = 0x06`, and `CP[0x91] = 0x50`.

---

## 3. Userspace V4L2 Timing Negotiation Mechanics (`v4l2-ctl`)

A crucial operational discovery from Phase 2C5D is how standard V4L2 userspace utilities interact with capture drivers that support DV timings.

### Automatic Timing Negotiation in `v4l2-ctl --stream-mmap`
By default, when `v4l2-ctl --stream-mmap` is executed, userspace automatically:
1. Invokes `VIDIOC_QUERY_DV_TIMINGS` to detect the live HDMI TMDS signal via the ADV7604 STDI block.
2. Invokes `VIDIOC_G_DV_TIMINGS` to retrieve the current timings configured in the driver/subdevice.
3. Compares the two timing structures.
4. If they differ, logs `"New timings found"` and issues `VIDIOC_S_DV_TIMINGS` to configure the subdevice before allocating streaming buffers.

When `adv76xx_s_dv_timings()` executes for an HDMI standard, it selects an entry from the predefined table `adv76xx_prim_mode_hdmi_comp[]`:
```c
static const struct adv76xx_video_standards adv76xx_prim_mode_hdmi_comp[] = {
    { V4L2_DV_BT_CEA_720X480P59_94, 0x0a, 0x00 },
    { V4L2_DV_BT_CEA_720X576P50,    0x0b, 0x00 },
    { V4L2_DV_BT_CEA_1280X720P50,   0x13, 0x01 },
    { V4L2_DV_BT_CEA_1280X720P60,   0x13, 0x00 },
    { V4L2_DV_BT_CEA_1920X1080P60,  0x1e, 0x00 },
    { },
};
```
And performs the following writes:
```c
io_write(sd, 0x00, predef_vid_timings[i].vid_std);
io_write(sd, 0x01, (predef_vid_timings[i].v_freq << 4) + prim_mode); /* prim_mode = 0x05 (HDMI-COMP) */
cp_write_clr_set(sd, 0x91, 0x40, bt->interlaced ? 0x40 : 0x00);
```

### Critical Testing Distinction: `--stream-no-query` vs Default
When testing or troubleshooting the capture pipeline, developers must distinguish between isolated streaming tests and automatic timing negotiation tests:

- **Isolated Capture Test (No Automatic Timing Changes):**
  ```bash
  v4l2-ctl -d /dev/video2 --stream-mmap --stream-count=60 --stream-no-query
  ```
  Use `--stream-no-query` to prevent `v4l2-ctl` from calling `VIDIOC_QUERY_DV_TIMINGS` and `VIDIOC_S_DV_TIMINGS`. This isolates buffer allocation, URB scheduling, and frame parsing from timing reconfigurations.

- **Standard Capture Test (With Timing Negotiation):**
  ```bash
  v4l2-ctl -d /dev/video2 --stream-mmap --stream-count=60
  ```
  Omitting `--stream-no-query` exercises the normal V4L2 workflow where userspace automatically queries and applies incoming timings.

### Fundamental Distinction: `VIDIOC_S_FMT` vs `VIDIOC_S_DV_TIMINGS`
- **`--set-fmt-video` (`VIDIOC_S_FMT`)**: Configures the pixel format, frame width, height, and buffer `sizeimage` on the GC550 video capture node (`/dev/video2`). It does **not** program the ADV7604 front-end receiver or alter HDMI receiver timing registers (`IO[0x00]`, `IO[0x01]`, `CP[0x91]`).
- **`--set-dv-bt-timings` (`VIDIOC_S_DV_TIMINGS`)**: Directly programs the ADV7604 hardware timing registers, adjusts the CP core's Standard Timing Identification (STDI), and synchronizes the subdevice source pad format.

---

## 4. Verified Register Transition History

The table below documents the complete hardware register progression across all phases of driver initialization, multi-resolution streaming (Phase 2C5D-A), and explicit timing experiments (Phase 2C5D-B):

| Lifecycle Stage | `IO[0x00]` (`VID_STD`) | `IO[0x01]` (`PRIM_MODE`) | `CP[0x91]` (`SCAN`) | Mechanism & Provenance |
|---|:---:|:---:|:---:|---|
| **1. Cold Bootstrap** | `0x08` (HDMI-GR) | `0x06` (1080p60) | `0x50` (Interlaced bit set) | `gc550_init_blob` steps 2653, 2681, 2683 |
| **2. Post-Probe Baseline** | `0x08` (HDMI-GR) | `0x06` (1080p60) | `0x50` (Interlaced bit set) | `adv76xx_core_init()` leaves registers untouched |
| **3. Post-Probe Configuration** | `0x08` (HDMI-GR) | `0x06` (1080p60) | `0x50` (Interlaced bit set) | `gc550_configure_adv7604()` leaves registers untouched |
| **4. 2C5D-A: 1080p60 Stream** | `0x1e` (1080p60) | `0x05` (HDMI-COMP) | `0x10` (Progressive) | `v4l2-ctl --stream-mmap` auto-negotiated 1080p60 |
| **5. 2C5D-A: 720p60 Stream** | `0x13` (720p60) | `0x05` (HDMI-COMP) | `0x10` (Progressive) | `v4l2-ctl --stream-mmap` auto-negotiated 720p60 |
| **6. 2C5D-A: 576p50 Stream** | `0x0b` (576p50) | `0x05` (HDMI-COMP) | `0x10` (Progressive) | `v4l2-ctl --stream-mmap` auto-negotiated 576p50 |
| **7. 2C5D-A: 480p60 Stream** | `0x0a` (480p60) | `0x05` (HDMI-COMP) | `0x10` (Progressive) | `v4l2-ctl --stream-mmap` auto-negotiated 480p60 |
| **8. 2C5D-B: Pre-Measurement** | **`0x0a` (480p60)** | **`0x05` (HDMI-COMP)** | **`0x10` (Progressive)** | **Retained state from 480p capture in Stage 7** |
| **9. 2C5D-B: Post S_DV_TIMINGS** | **`0x1e` (1080p60)** | **`0x05` (HDMI-COMP)** | **`0x10` (Progressive)** | Explicit `v4l2-ctl --set-dv-bt-timings query` (1080p60) |

### Explanation of the Phase 2C5D-B Pre-Measurement:
In Phase 2C5D-B, direct hardware register inspection on `i2c-13` before running the 1080p timing experiment showed:
- `IO[0x00] = 0x0a`
- `IO[0x01] = 0x05`
- `CP[0x91] = 0x10`

This observation was initially flagged because it differed from the cold-bootstrap baseline (`0x08`, `0x06`, `0x50`). As proven by the register provenance audit, this was **not** hardware drift, register corruption, or an erroneous bootstrap. It was the exact state programmed by `v4l2-ctl --stream-mmap` during the final 480p60 capture test in Phase 2C5D-A. Subsequent format changes with `v4l2-ctl --set-fmt-video=1920x1080` updated the V4L2 pixel format but did not issue an `S_DV_TIMINGS` call, leaving the ADV7604 hardware in its valid 480p state until Phase 2C5D-B explicitly reprogrammed it.

---

## 5. Hardware Validation of `VIDIOC_S_DV_TIMINGS` (Phase 2C5D-B)

Phase 2C5D-B conducted a controlled, physical test of explicit `VIDIOC_S_DV_TIMINGS` execution for 1080p60 following the retained 480p60 state.

### Observed Physical Register Changes:
1. **`IO[0x00]` (`VID_STD`)**: Successfully transitioned from `0x0a` (480p) to `0x1e` (1080p60 predefined mode in `adv76xx_prim_mode_hdmi_comp[]`).
2. **`IO[0x01]` (`PRIM_MODE`)**: Remained `0x05` (`HDMI-COMP`).
3. **`CP[0x91]` (`SCAN`)**: Remained `0x10` (progressive scan; bit 6 cleared).
4. **`CP[0xff]` (`CP Status`)**: Successfully transitioned from `0x28` to `0x08`. Bit 4 cleared confirms that the CP core locked to the incoming TMDS signal and disabled free-run.
5. **Critical Register Preservation**:
   - `IO[0x04]` remained strictly `0x42` (`ADV7604_BUS_ORDER_BGR`).
   - `IO[0x03]` remained strictly `0x80` (16-bit 4:2:2 SDR output mode).
   - `IO[0x02]` remained `0xf4` (auto color-space).
6. **Subdevice Pad Format Synchronization**:
   - Kernel logged: `gc550 2-3:1.0: Configured adv7604 pad 6 format: code=0x2011 1920x1080`.
   - `VIDIOC_G_DV_TIMINGS` and `VIDIOC_QUERY_DV_TIMINGS` subsequently agreed completely (1920x1080p60, pixel clock 148.5 MHz, VIC 16).

### Physical Video & Audio Verification:
- **Video Capture:** 120 frames captured with 0 mid-stream desyncs, 0 dropped frames, and `swap_mode = 0`. Broadcast range verified ($Y \in [16..235]$, mean 68.22, $Y=255$ saturated pixels: 0.00%).
- **ALSA Audio Capture:** Continuous stereo PCM captured simultaneously (48 kHz S16_LE) with dynamic RMS amplitude ~4000 and 0 XRUNs.
- **Visual Display:** Live presentation in `mpv` at 1:1 native scale confirmed clear, artifact-free video and synchronized audio.

> [!NOTE]
> **Scope of Validation:** The physical `S_DV_TIMINGS` experiment explicitly validated the transition from 480p60 to 1080p60. It demonstrated that `s_dv_timings` safely programs the ADV7604 CP core without corrupting bus order or pixel formatting. This does not claim that every conceivable video standard transition has been exhaustively tested.

---

## 6. Media Controller Topology & Multi-Resolution Validation (Phase 2C5D-A)

Phase 2C5D-A established and verified the complete Media Controller topology in `subdev_mode=1`.

### Media Graph Architecture (`/dev/media1`):
```text
- entity 1: adv7604 13-0020 (7 pads, 1 link)
            type V4L2 subdev
            pad0..pad5: SINK (HDMI / Component inputs)
            pad6: SOURCE
                -> "AVerMedia GC550":0 [ENABLED,IMMUTABLE]

- entity 9: adv7511-v4l2 13-003d (1 pad, 0 links)
            type V4L2 subdev
            pad0: SINK (HDMI Passthrough TX)

- entity 11: AVerMedia GC550 (1 pad, 1 link)
             type Node (V4L2 video capture)
             device node: /dev/video2
             pad0: SINK
                 <- "adv7604 13-0020":6 [ENABLED,IMMUTABLE]
```

### Multi-Resolution Capture Results (Phase 2C5D-A):
Video capture was verified across all four supported broadcast resolutions using a Raspberry Pi HDMI source:

| Mode | Resolution | Frame Rate | Captured Frames | Desyncs | Swap Mode | Luma Range ($Y$) | Saturated White ($Y=255$) |
|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| 1 | `1920x1080` | 60.02 fps | 120 | **0** | **0** | Min 16, Max 235 (mean 67.33) | **0.00%** |
| 2 | `1280x720` | 60.08 fps | 120 | **0** | **0** | Min 16, Max 235 (mean 70.26) | **0.00%** |
| 3 | `720x576` | 50.00 fps | 100 | **0** | **0** | Min 16, Max 235 (mean 66.97) | **0.00%** |
| 4 | `720x480` | 59.98 fps | 120 | **0** | **0** | Min 16, Max 235 (mean 67.84) | **0.00%** |

In all modes, USB transport remained completely error-free, SAV/EAV frame parsing achieved 100% boundary alignment, and visual inspection in `mpv` confirmed correct color rendering and aspect ratio.
