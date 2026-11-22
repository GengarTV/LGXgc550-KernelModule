# Driver Architecture & Hardware Pipeline (`gc550.ko`)

This document provides a deep architectural overview of the **AVerMedia Live Gamer Extreme (GC550)** Linux kernel module (`gc550.ko`).

---

## 1. High-Level System Architecture

Rather than relying on a userspace daemon (`libusb`), virtual v4l2loopback nodes, or virtual audio sinks, `gc550.ko` integrates directly into the core Linux kernel media, sound, and USB subsystems:

```
 +------------------------------------------------------------------------+
 |                          Userspace Applications                        |
 |            (OBS Studio, mpv, FFmpeg, PipeWire, PulseAudio)             |
 +-----------------------------------+------------------------------------+
                                     |
                                     v
       +-----------------------------+-----------------------------+
       |                  Linux Kernel Subsystems                  |
       |  +--------------------------+  +-----------------------+  |
       |  |  V4L2 Video Subsystem    |  |     ALSA Subsystem    |  |
       |  |   (/dev/avermedia-gc550) |  |   (hw:GC550Audio,0)   |  |
       |  +-------------+------------+  +-----------+-----------+  |
       |                |                           |              |
       |  +-------------v---------------------------v-----------+  |
       |  |                gc550.ko Driver Core                 |  |
       |  |  - Stream Parser (SAV/EAV/Audio Demux)              |  |
       |  |  - 16 KiB FX3 DMA Swap Reorderer                    |  |
       |  |  - Unified Media Controller Graph & DV Timings       |  |
       |  |  - FX3 USB-to-I2C Bridge & Safe Bootstrap           |  |
       |  +--------------------------+--------------------------+  |
       |                             |                             |
       |                             v                             |
       |  +-----------------------------------------------------+  |
       |  |                 USB Core (usbcore / xHCI)           |  |
       |  +--------------------------+--------------------------+  |
       +-----------------------------|-----------------------------+
                                     | USB 3.0 SuperSpeed (5 Gbps)
                                     v
 +-----------------------------------+------------------------------------+
 |                      AVerMedia GC550 Hardware                          |
 |  +---------------+  +---------------+  +--------------+  +----------+  |
 |  |  Cypress FX3  |  |   ADV7604     |  |   ADV7511    |  | Lattice  |  |
 |  | USB 3.0 + I2C |  |  (HDMI RX)    |  |  (HDMI TX)   |  | ECP3 FPGA|  |
 |  +---------------+  +---------------+  +--------------+  +----------+  |
 +------------------------------------------------------------------------+
```

---

## 2. V4L2 Video Capture Engine (`gc550-video.c`)

### 2.1 Buffer Management & Memory Model
- **Streaming Framework:** Built on `videobuf2-vmalloc` with standard memory mapping (`V4L2_MEMORY_MMAP`) and user pointer (`V4L2_MEMORY_USERPTR`) support.
- **SAV-Locked Reservation:** Incoming USB video bytes are accumulated into active frame buffers only after detecting a valid `SAV` marker (`00 FF FF C0`). Buffers are never reserved mid-frame, eliminating partial-frame tearing during startup or mode switches.
- **Monotonic Frame Sequence & Timestamps:** Timestamps are captured with `ktime_get_ns()` immediately upon `EAV` marker arrival (`gc550_finish_buffer()`), ensuring zero jitter in downstream OBS video pacing.

### 2.2 Supported Capture Sizes
The driver exposes five discrete capture sizes in `gc550_frame_sizes[]`:
- `1920×1080` (1080p) — `sizeimage = 4,147,200 bytes`
- `1280×720` (720p) — `sizeimage = 1,843,200 bytes`
- `720×576` (576p PAL) — `sizeimage = 829,440 bytes`
- `720×480` (480p NTSC) — `sizeimage = 691,200 bytes`
- `640×480` (VGA) — `sizeimage = 614,400 bytes`

### 2.3 Tail Fill & Short-Frame Protection
If a frame span completes with slightly fewer bytes than the nominal `sizeimage` (within the protocol-allowed ±16 KiB tolerance), the driver fills the remainder with neutral black (`Y=16, U=128, Y=16, V=128`), preventing stale video data from leaking into userspace buffers.

---

## 3. ALSA HDMI Audio Capture Engine (`gc550-audio.c`)

The GC550 multiplexes embedded HDMI LPCM audio directly into the primary USB 3.0 Bulk stream (`EP 0x83 IN`):
- **Audio Framing Marker:** Audio payloads are delimited by `00 FF FF 58` followed by a 4-byte big-endian length header.
- **Audio Format:** 2-channel, 16-bit Little-Endian (`S16_LE`) uncompressed PCM sampled at `48,000 Hz`.
- **Zero-Copy Ring Buffer Delivery:** When `gc550-video.c` encounters an audio packet during video demultiplexing, it hands the raw PCM payload to `gc550_audio_push_samples()`, which copies directly into the ALSA runtime DMA ring buffer and calls `snd_pcm_period_elapsed()`.
- **Desktop Stack Integration:** Appears as a native sound card (`hw:GC550Audio,0`), immediately auto-discovered by PipeWire, PulseAudio, and ALSA without requiring loopback cables or virtual routing daemons.

---

## 4. Unified Media Controller & V4L2 Subdevice Architecture

`gc550.ko` uses a single, unconditional V4L2 subdevice and Media Controller architecture (the legacy `subdev_mode` parameter and standalone mode were removed in U2-B02-S1). On USB probe, the driver registers a `/dev/mediaX` device and binds the GC550 video capture node with upstream in-tree I2C subdevice drivers (`adv7604` and `adv7511-v4l2`):

```text
 +------------------------------------------------------------------------+
 |                       Media Controller Topology                        |
 |                                                                        |
 |   +---------------------+  Pad 6 (SOURCE)  [ENABLED,IMMUTABLE]         |
 |   |   adv7604 13-0020   +========================> +-----------------+ |
 |   |   (HDMI Receiver)   |                          | AVerMedia GC550 | |
 |   |   pad0..pad5: SINK  |                          |  (/dev/videoX)  | |
 |   |   pad6: SOURCE      +----+                     |   pad0: SINK    | |
 |   +---------------------+    |                     +-----------------+ |
 |                              | [ENABLED,IMMUTABLE]                     |
 |                              | ("adv7604":6 -> "adv7511-v4l2":0)       |
 |                              v                                         |
 |                    +-----------------------+                           |
 |                    | adv7511-v4l2 13-003d  |                           |
 |                    | (HDMI TX Passthrough) |                           |
 |                    |      pad0: SINK       |                           |
 |                    +-----------------------+                           |
 +------------------------------------------------------------------------+
```

### 4.1 Subdevice Integration & Compliance Features (U2-B02-S1/S2)
- **Complete Fan-Out Topology (Hardware Verified, Gate 2 Phase C):** `adv7604` Pad 6 (`SOURCE`) fans out via two `[ENABLED,IMMUTABLE]` links to `"AVerMedia GC550":0` (capture path) and `"adv7511-v4l2":0` (HDMI passthrough transmitter), leaving zero orphan entities.
- **ADV7511 Post-Probe Power Restoration (Hardware Verified, Gate 2 Phase B/D):** Because `adv7511_probe()` powers down the transmitter (`Main[0x41] = 0x50`), `gc550_subdev_init()` invokes `v4l2_subdev_call(dev->sd_adv7511, core, s_power, 1)` after subdevice registration to restore `power on` (`Main[0x41] = 0x10`).
- **V4L2 DV Timings Suite & Pad Validation (Hardware Verified, Gate 2 Phase D):** Implements `VIDIOC_G_DV_TIMINGS`, `VIDIOC_S_DV_TIMINGS`, `VIDIOC_QUERY_DV_TIMINGS`, `VIDIOC_ENUM_DV_TIMINGS`, and `VIDIOC_DV_TIMINGS_CAP`. Both `gc550_enum_dv_timings()` and `gc550_dv_timings_cap()` validate `pad == 0` and reject non-zero pads with `-EINVAL`. *(Known open finding `REM-B02-002`: `VIDIOC_ENUM_DV_TIMINGS` delegates unfiltered to `adv7604` [73 timings], while `VIDIOC_S_DV_TIMINGS` filters against the 5-entry `gc550_frame_sizes[]` table [15 accepted, 58 rejected with `-EINVAL`].)*
- **Pad Format Propagation:** When timings are negotiated or applied via `VIDIOC_S_DV_TIMINGS`, `gc550.ko` invokes `v4l2_subdev_call(adv7604, pad, set_fmt)` to synchronize ADV7604 Pad 6 format (`code=0x2011`) with the active capture pipeline.
- **Safe Bootstrap Isolation:** The bootstrap sequence preserves known-good SDR bus ordering (`ADV7604_BUS_ORDER_BGR`) and ensures DV timing operations leave internal routing untouched.

---

## 5. Hardware Bootstrap & Safe Command Bridge (`gc550-cmd.c`)

The GC550 hardware requires an initial wake-up sequence over the Cypress FX3 USB-to-I2C command bridge (`EP 0x01 OUT` / `EP 0x81 IN`) executed during probe prior to subdevice registration:
- **Command Protocol:** Packets are framed with 8-byte command headers (`0xAA 0x08 ...`) specifying I2C bus targets, write lengths, and register addresses.
- **Modular 27-Stage Initialization Sequence (`U2-B01-S1/S2/S3`):** A 27-stage, 2,615-step sequence in `kernel/gc550-regtables.h` (combining symbolic Class 1 board register operations and a compacted 20,082-byte `gc550_init_blob[]` — reduced from the original 6,576-step / 53,146-byte sequence and the intermediate Phase 2C6D/E 21,458-byte blob) initializes the ADV7604 HDMI receiver (including loading the 1080p60 EDID binary into internal RAM), ADV7511 HDMI transmitter, TLV320AIC3x audio codec, PCA9539A GPIO expander, and Lattice FPGA in `~1.15 s`.
- **AT88SC CryptoMemory Protection:** AVerMedia hardware utilizes an Atmel AT88SC authentication chip. `gc550.ko` strips proprietary authentication handshake replays from the initialization sequence and enforces a hard error (`-EPERM`) in `gc550_cmd_send()` blocking bridge opcodes `0x05..0x07` and `0x09`. This permanently protects the device's physical attempt counters (`0xFF`) from exhaustion or bricking.

---

## 6. Stream Resilience & Desynchronization Recovery

### 6.1 Cross-URB Marker Straddle Tracking
USB 3.0 transfers arrive in 512 KiB URBs. Because HDMI video scanout and audio packets do not align to 512 KiB powers of two, 4-byte marker headers (`00 FF FF C0`, `00 FF FF C1`, `00 FF FF 58`) often span across two adjacent URBs. The parser maintains a 4-stage state machine (`marker_stage`, `saved_marker`) that preserves partial marker bytes across URB boundaries, guaranteeing zero frame corruption at transfer borders.

### 6.2 16 KiB FX3 DMA Swap Engine & Native Diagnostics
During warm restarts or quick stream cycling, the FX3 internal ping-pong DMA socket buffer phase can invert, swapping adjacent 16 KiB blocks. The driver includes an automatic swap detector and minimal native kernel diagnostics:
- Evaluates video line boundaries and frame candidate spans directly from Bulk IN `0x83` URB callbacks (`urb->context` is wired directly to `struct gc550_dev *` with no custom diagnostic wrappers or ring buffers).
- Dynamically pairs and unswaps 16 KiB blocks in memory when swapped phase is detected.
- Reports DMA swap mode switches via `dev_info_ratelimited()`, parser anomalies (unclosed-frame aborts, swap votes, invalid spans, SAV/EAV checksum or sequence mismatches) via zero-overhead `dev_dbg_ratelimited()` (`dynamic_debug`), and stream/parser counters via `VIDIOC_LOG_STATUS` (`v4l2-ctl --log-status`) and stream-stop kernel logs.
