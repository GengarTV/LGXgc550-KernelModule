# AVerMedia Live Gamer Extreme (GC550) Linux Kernel Driver (`gc550.ko`)

> **Fork Notice & Credits:** This repository is a Linux kernel-level fork of [**`ChrisAJS/lgx2userspace`**](https://github.com/ChrisAJS/lgx2userspace) by **Chris Sawczuk ([@ChrisAJS](https://github.com/ChrisAJS))** and contributors. Building directly upon their USB 3.0 protocol reverse-engineering, [`PROTOCOL.md`](PROTOCOL.md) specification, and hardware bootstrap research, this project implements a **native Linux kernel driver (`V4L2` + `ALSA`)** for the **AVerMedia Live Gamer Extreme (GC550, `USB ID 07ca:4710` / `07ca:0550`)**.

Native, out-of-tree Linux kernel module providing plug-and-play video (`V4L2`) and audio (`ALSA`) capture for the AVerMedia Live Gamer Extreme (GC550) over USB 3.0 SuperSpeed.

---

## Quick Hardware & Feature Summary

| Component | Status | Details |
|---|---|---|
| **Supported Devices** | **GC550 (`07ca:4710`, `07ca:0550`)** | Fully supported and bare-metal verified on CachyOS / Arch Linux. *(Note: For the LGX2 / GC551 `07ca:0551`, use upstream [`lgx2userspace`](https://github.com/ChrisAJS/lgx2userspace)).* |
| **V4L2 Video** | **1080p60 down to 480p60** | Uncompressed `YUYV` 4:2:2 video capture (`/dev/videoX` & `/dev/avermedia-gc550`). |
| **ALSA Audio** | **48 kHz 16-bit Stereo** | Hardware HDMI LPCM audio demuxed directly to native ALSA card (`hw:GC550Audio,0`), visible in PipeWire / PulseAudio. |
| **Media Controller** | **`/dev/mediaX` (`subdev_mode=1`)** | Optional subdevice graph linking in-tree `adv7604` (RX) and `adv7511` (TX) with V4L2 DV timings. |
| **End-to-End Latency** | **`~50 ms` (~3 frames @ 60 Hz)** | Minimum latency pipeline saving ~750 MB/s DRAM bandwidth over userspace daemons. |

---

## Installation

### 1. Install Prerequisites (Kernel Headers & DKMS)
Ensure you have `git`, `make`, `dkms`, and your distribution's kernel headers installed:

- **Arch Linux / CachyOS**:
  ```bash
  sudo pacman -S base-devel git dkms linux-headers
  # (If using CachyOS kernel: sudo pacman -S linux-cachyos-headers)
  ```
- **Debian / Ubuntu**:
  ```bash
  sudo apt install build-essential git dkms linux-headers-$(uname -r)
  ```
- **Fedora**:
  ```bash
  sudo dnf install kernel-devel kernel-headers make gcc git dkms
  ```

### 2. Clone & Run the Modern DKMS Installer
```bash
git clone https://github.com/GengarTV/lgx2KernelLevel.git
cd lgx2KernelLevel
./install.sh
```

`./install.sh` is an idempotent, distro-aware DKMS installer that:
1. Validates kernel headers, build toolchains (GCC or Clang/LLVM), DKMS, and subdevice dependencies.
2. Isolates a clean 12-file source tree into `/usr/src/gc550-<version>`.
3. Registers with DKMS, builds for the active kernel, and installs via `dkms install --force`.
4. Installs the udev rule (`/etc/udev/rules.d/99-avermedia-gc550.rules`) granting user access and `/dev/avermedia-gc550` symlinks.
5. Preserves running capture streams safely without forced module unloading.

### Installer Command Reference
```bash
./install.sh          # Full automated DKMS build and installation
./install.sh --check  # Non-destructive preflight audit (makes no system changes)
./install.sh --help   # Display available options and usage
./uninstall.sh        # Cleanly and idempotently remove DKMS module, source tree, and udev rules
```

### Persistence Across Kernel Updates (Zero-Touch DKMS)
- **Automatic Rebuilds:** Because `AUTOINSTALL="yes"` is enabled in `dkms.conf`, your package manager (`pacman`, `apt`, `dnf`) automatically triggers DKMS to recompile `gc550.ko` in the background whenever a new kernel is installed.
- **Subdevice Registration (`subdev_mode`):**
  - `subdev_mode=0` (default): Standalone V4L2 and ALSA capture nodes.
  - `subdev_mode=1`: Enables Media Controller subdevice graph (`/dev/mediaX`) and ADV7604/ADV7511 bindings.
  - To enable permanently, create `/etc/modprobe.d/gc550.conf` with:
    ```text
    options gc550 subdev_mode=1
    ```

---

## Operational Verification Checklist

After running `./install.sh` and connecting the GC550 to a **USB 3.0 (SuperSpeed)** port:

1. **Verify USB 3.0 SuperSpeed (`5000M`) Connection:**
   ```bash
   lsusb -t | grep -i gc550
   ```
   *Expected:* `|__ Port X: Dev Y, If 0, Class=Vendor Specific Class, Driver=gc550, 5000M`

2. **Verify Video & Audio Devices:**
   ```bash
   v4l2-ctl --list-devices
   arecord -l | grep -i gc550
   ```
   *Expected:* `/dev/avermedia-gc550` video node and `card X: GC550Audio [AVerMedia GC550]` audio card.

3. **Test 60 Frames of Video Capture:**
   ```bash
   v4l2-ctl -d /dev/avermedia-gc550 --stream-mmap --stream-count=60
   ```

4. **Configure in OBS Studio:**
   - **Video:** Add **Video Capture Device (V4L2)** → Select **AVerMedia Live Gamer Extreme (GC550)**.
   - **Audio:** Add **Audio Input Capture (ALSA / PipeWire)** → Select **AVerMedia GC550**.

If any issue occurs, see **[`TROUBLESHOOTING.md`](TROUBLESHOOTING.md)**.

---

## Known Limitations

1. **SDR Only (No HDR):** The internal Analog Devices ADV7604 is an HDMI 1.4a SDR receiver. Turn **HDR OFF** on your source console/PC.
2. **USB 3.0 Required:** 1080p60 YUYV video requires ~250 MB/s of USB bandwidth. USB 2.0 ports/cables are unsupported.
3. **Audio Requires Video Streaming:** The GC550 hardware multiplexes HDMI audio into the USB 3.0 video stream. The video node must be actively streaming (e.g. open in OBS) for audio samples to flow into ALSA.
4. **Manual Resolution Matching:** The driver supports 1080p, 720p, 576p, 480p, and 640x480. Match your OBS canvas / capture resolution to your input signal.
5. **Cold Bootstrap Delay (~3.3 s):** The very first time `/dev/videoX` is opened after USB insertion, the driver sends 6,576 setup commands (~3.3 s). Subsequent opens start instantaneously.

---

## Documentation & Technical Deep-Dives

Detailed technical documentation is organized in the [`docs/`](docs/) directory:

- **[`docs/HARDWARE-COMPATIBILITY.md`](docs/HARDWARE-COMPATIBILITY.md)** — Comprehensive hardware matrix, tested HDMI sources, bare-metal lab testbeds (Titan & Niri), and V4L2 compliance test results.
- **[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)** — In-depth driver architecture, buffer reordering, ALSA audio demuxing, safe AT88SC crypto isolation, and Media Controller subdevice graph.
- **[`docs/PERFORMANCE.md`](docs/PERFORMANCE.md)** — Latency analysis (`50 ms` glass-to-glass), 3-frame pipeline breakdown, memory bandwidth savings (~750 MB/s DRAM reduction), and endurance benchmarks.
- **[`docs/ADV7604-HARDWARE-LIFECYCLE.md`](docs/ADV7604-HARDWARE-LIFECYCLE.md)** — Register-level hardware lifecycle, bootstrap provenance, and DV timing negotiation.
- **[`docs/GC550-REVERSE-ENGINEERING.md`](docs/GC550-REVERSE-ENGINEERING.md)** — Hardware reverse-engineering reference, I2C bus targets, and protocol analysis.
- **[`TROUBLESHOOTING.md`](TROUBLESHOOTING.md)** — Common build errors, Clang/GCC flags, Secure Boot signing, and OBS configuration fixes.
- **[`PROTOCOL.md`](PROTOCOL.md)** — Upstream USB 3.0 bulk protocol and register specification (by `@ChrisAJS`).
- **[`CONTRIBUTING.md`](CONTRIBUTING.md)** — Developer guide, coding standards, and hardware safety rules.
- **[`CHANGELOG.md`](CHANGELOG.md)** — Release history and notable changes.

---

## Repository Structure

```text
├── 99-avermedia-gc550.rules # udev permissions & /dev/avermedia-gc550 symlink
├── CHANGELOG.md             # Version history and release notes
├── CONTRIBUTING.md          # Developer & maintainer guide
├── Kbuild                   # Kernel build object definitions
├── LICENCE.md               # Upstream MIT license (Chris Sawczuk)
├── LICENSE-GPL-2.0          # GNU General Public License v2 text (original project code)
├── LICENSE-MIT              # MIT License (original project code, Øystein Lie)
├── LICENSES.md              # Project licensing policy & provenance guide
├── Makefile                 # Out-of-tree kernel module Makefile (GCC & Clang/LLVM auto-detect)
├── PKGBUILD                 # Arch Linux / CachyOS pacman package build script (avermedia-gc550-dkms)
├── PROTOCOL.md              # USB 3.0 protocol & hardware specification (by @ChrisAJS)
├── TODO.md                  # Roadmap & technical design notes
├── TROUBLESHOOTING.md       # Common build, USB, video, and audio troubleshooting guide
├── dkms.conf                # DKMS package configuration
├── install.sh               # Idempotent DKMS installer and preflight audit script
├── uninstall.sh             # Idempotent DKMS uninstaller script
├── docs/
│   ├── ARCHITECTURE.md            # Kernel subsystem design, stream parser & media graph
│   ├── HARDWARE-COMPATIBILITY.md  # Hardware matrix, HDMI sources & lab test environments
│   ├── PERFORMANCE.md             # Glass-to-glass latency, DRAM throughput & benchmarks
│   ├── ADV7604-HARDWARE-LIFECYCLE.md # Register provenance & DV timing lifecycle
│   ├── GC550-REVERSE-ENGINEERING.md  # Comprehensive chip registers & I2C reverse-engineering
│   └── Top Overview.png           # Hardware PCB layout reference
└── kernel/
    ├── gc550.h              # Driver structures, constants, and register definitions
    ├── gc550-core.c         # USB probe/disconnect & lifecycle management
    ├── gc550-cmd.c          # FX3 USB Bulk (0x01/0x81) to I2C bridge & safe bootstrap
    ├── gc550-i2c.c          # I2C adapter & ADV7604/ADV7511 subdevice client registration
    ├── gc550-regtables.h    # Complete 6,576-step GC550 hardware wake-up table (Crypto-stripped)
    ├── gc550-video.c        # V4L2 capture device, straddle parser, media graph & DV timings
    ├── gc550-audio.c        # ALSA PCM capture device for demuxed HDMI audio
    ├── gc550-diag.h         # Diagnostic sysfs attribute definitions
    └── gc550-diag.c         # Diagnostic sysfs register and counter interfaces
```

---

## Credits & Upstream Links

- **Chris Sawczuk ([@ChrisAJS](https://github.com/ChrisAJS))** and contributors to [**`ChrisAJS/lgx2userspace`**](https://github.com/ChrisAJS/lgx2userspace):
  - Reverse-engineered the USB-to-I2C bridge, the GC550/GC551 initialisation sequences, the BT.1120 SAV/EAV framing checksums, the ADV7604 automatic colour-space fix, the 16 KiB FX3 DMA swap behaviour, and the AT88SC CryptoMemory anti-clone protection documented in [`PROTOCOL.md`](PROTOCOL.md).
- **Linux Kernel Media Maintainers**:
  - Authors of the upstream `adv7604` and `adv7511` drivers.

---

## Note for Windows, macOS, and Userspace Driver Users

This repository contains **only the native Linux kernel module (`gc550.ko`)**. All Windows, macOS, and cross-platform `libusb` / SDL3 userspace application files have been removed from this fork.

If you are on **Windows**, **macOS**, or want the **userspace driver (`lgx2userspace` / `lgx2d`)** or **LGX2 (GC551)** userspace support, please visit **Chris Sawczuk's (@ChrisAJS) upstream repository**:
👉 [**https://github.com/ChrisAJS/lgx2userspace**](https://github.com/ChrisAJS/lgx2userspace)

---

## Licensing & Provenance

This project contains original Linux kernel driver code and incorporates research, protocol definitions, and hardware initialization data derived from the upstream [**`ChrisAJS/lgx2userspace`**](https://github.com/ChrisAJS/lgx2userspace) project:

1. **Original Project Code (`GPL-2.0-only OR MIT`):** All original Linux kernel driver code, headers, build scripts, and packaging authored for `lgx2KernelLevel` (Copyright (C) 2026 Øystein Lie) are dual-licensed under **GPL-2.0-only** ([`LICENSE-GPL-2.0`](LICENSE-GPL-2.0)) or **MIT** ([`LICENSE-MIT`](LICENSE-MIT)). The kernel module binary declares `MODULE_LICENSE("Dual MIT/GPL")`.
2. **Upstream lgx2userspace Material (`MIT License`):** Foundational USB 3.0 protocol reverse-engineering, hardware initialization tables, BT.1120 SAV/EAV checksum validation formulas, and AT88SC CryptoMemory research are derived from [`ChrisAJS/lgx2userspace`](https://github.com/ChrisAJS/lgx2userspace) (Copyright 2021 Chris Sawczuk and contributors, licensed under the MIT License, [`LICENCE.md`](LICENCE.md)).
3. **Upstream Protocol Specification:** [`PROTOCOL.md`](PROTOCOL.md) is preserved verbatim as historical documentation under its original author's copyright and MIT licensing.

For complete details, see [`LICENSES.md`](LICENSES.md).
