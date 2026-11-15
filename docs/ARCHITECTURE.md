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
       |  |  - Media Controller Graph & DV Timings (`subdev=1`) |  |
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

## 4. Media Controller & V4L2 Subdevice Architecture (`subdev_mode=1`)

When loaded with `subdev_mode=1`, `gc550.ko` registers a `/dev/mediaX` device and binds the GC550 video node with upstream in-tree subdevice drivers:

```
 +------------------------------------------------------------------------+
 |                       Media Controller Topology                        |
 |                                                                        |
 |   +-----------------+            Pad 6 (Source)                        |
 |   |     adv7604     |  [video] =================> +-----------------+  |
 |   | (HDMI Receiver) |                             | AVerMedia GC550 |  |
 |   +--------+--------+                             |  (/dev/video0)  |  |
 |            |                                      +-----------------+  |
 |            | Link (adv7604:6 -> gc550:0)                               |
 |            | [ENABLED, IMMUTABLE]                                      |
 |            v                                                           |
 |   +-----------------+                                                  |
 |   |     adv7511     |                                                  |
 |   | (HDMI TX Passth)|                                                  |
 |   +-----------------+                                                  |
 +------------------------------------------------------------------------+
```

### 4.1 Subdevice Integration Features
- **V4L2 DV Timings Suite:** Implements `VIDIOC_G_DV_TIMINGS`, `VIDIOC_S_DV_TIMINGS`, `VIDIOC_QUERY_DV_TIMINGS`, `VIDIOC_ENUM_DV_TIMINGS`, and `VIDIOC_DV_TIMINGS_CAP`.
- **Pad Format Propagation:** When timings are negotiated or queried via `v4l2-ctl`, `gc550.ko` invokes `v4l2_subdev_call(adv7604, pad, set_fmt)` to synchronize ADV7604 Pad 6 format with the active capture pipeline.
- **Safe Bootstrap Isolation:** The bootstrap sequence preserves known-good SDR bus ordering (`ADV7604_BUS_ORDER_BGR`) and ensures DV timing operations leave internal routing untouched.

---

## 5. Hardware Bootstrap & Safe Command Bridge (`gc550-cmd.c`)

The GC550 hardware requires an initial wake-up sequence over the Cypress FX3 USB-to-I2C command bridge (`EP 0x01 OUT` / `EP 0x81 IN`):
- **Command Protocol:** Packets are framed with 8-byte command headers (`0xAA 0x08 ...`) specifying I2C bus targets, write lengths, and register addresses.
- **Chip Initialization Table:** A 6,576-step sequence in `kernel/gc550-regtables.h` initializes the ADV7604 HDMI receiver (including loading the 1080p60 EDID binary into internal RAM), ADV7511 HDMI transmitter, TLV320AIC3x audio codec, and Lattice FPGA.
- **AT88SC CryptoMemory Protection:** AVerMedia hardware utilizes an Atmel AT88SC authentication chip. `gc550.ko` strips proprietary authentication handshake replays from the initialization sequence and enforces a hard error (`-EPERM`) in `gc550_cmd_send()` blocking bridge opcodes `0x05..0x07` and `0x09`. This permanently protects the device's physical attempt counters (`0xFF`) from exhaustion or bricking.

---

## 6. Stream Resilience & Desynchronization Recovery

### 6.1 Cross-URB Marker Straddle Tracking
USB 3.0 transfers arrive in 512 KiB URBs. Because HDMI video scanout and audio packets do not align to 512 KiB powers of two, 4-byte marker headers (`00 FF FF C0`, `00 FF FF C1`, `00 FF FF 58`) often span across two adjacent URBs. The parser maintains a 4-stage state machine (`marker_stage`, `saved_marker`) that preserves partial marker bytes across URB boundaries, guaranteeing zero frame corruption at transfer borders.

### 6.2 16 KiB FX3 DMA Swap Engine
During warm restarts or quick stream cycling, the FX3 internal ping-pong DMA socket buffer phase can invert, swapping adjacent 16 KiB blocks. The driver includes an automatic swap detector that:
- Evaluates video line boundaries and frame candidate spans.
- Dynamically pairs and unswaps 16 KiB blocks in memory when swapped phase is detected.
- Reports active swap status through sysfs diagnostic interfaces (`swap_mode`, `desync_count`).
