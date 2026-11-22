# GC550 Kernel Driver — Pseudocode

> **Purpose:** An implementation-oriented, language-independent description of the
> AVerMedia Live Gamer Extreme (GC550) Linux kernel module (`gc550.ko`).
>
> **Source basis:** The current `main` branch implementation in `kernel/gc550-core.c`,
> `gc550-video.c`, `gc550-audio.c`, `gc550-cmd.c`, `gc550-i2c.c`, and `gc550.h`.
>
> **Important:** This is explanatory pseudocode, not compilable C and not a replacement
> for the Linux kernel API contracts. It describes the implementation's main control
> paths and intentionally abstracts locks, reference counting, kernel object details,
> and repetitive V4L2 ioctl plumbing.

---

## 1. System overview

```text
GC550 USB device
    |
    +-- Command transport: Bulk OUT 0x01 / Bulk IN 0x81
    |       |
    |       +-- FX3 command protocol
    |       +-- I2C read/write bridge
    |       +-- Hardware bootstrap / board-state detection
    |
    +-- Stream transport: Bulk IN 0x83
            |
            +-- URB completion callbacks
                    |
                    +-- Optional 16 KiB DMA pair reordering
                    |
                    +-- USB stream parser
                           |
                           +-- SAV / video payload / EAV
                           |       |
                           |       +-- V4L2 video buffers
                           |
                           +-- Audio packet
                                   |
                                   +-- ALSA PCM capture
```

The driver exposes native Linux interfaces:

- **V4L2** for video capture.
- **ALSA PCM** for HDMI audio capture.
- **Linux I2C adapter** for devices behind the FX3 USB-to-I2C bridge.
- **Media Controller graph and I2C subdevices (`adv7604` + `adv7511-v4l2`)**, registered unconditionally on probe.
- **Native kernel diagnostics** via `dev_dbg_ratelimited()` (`dynamic_debug`) and `VIDIOC_LOG_STATUS`.

---

## 2. Module registration and USB device lifecycle

### 2.1 Register the USB driver

```text
PROCEDURE MODULE_START
    Register USB driver named "gc550"
    Register supported USB device IDs:
        AVerMedia vendor ID + GC550 primary product ID
        AVerMedia vendor ID + GC550 alternate product ID
END PROCEDURE
```

### 2.2 Probe a newly connected device

```text
PROCEDURE USB_PROBE(interface, device_id)
    device = Get USB device from interface

    IF interface number is not 0 THEN
        RETURN "Not our interface"
    END IF

    Log detected vendor, product, and USB speed

    IF USB speed is below SuperSpeed THEN
        Warn that USB 3.0 is required for the intended high-bandwidth mode
    END IF

    dev = Allocate and initialize driver state
    IF allocation fails THEN
        RETURN out-of-memory error
    END IF

    Acquire references to USB device and interface
    Initialize:
        V4L2 mutex
        command mutex
        stream mutex
        stream spinlock
    Associate driver state with USB interface

    IF command buffers initialization fails THEN
        GOTO RELEASE_DEVICE
    END IF

    IF Linux I2C adapter registration fails THEN
        GOTO FREE_COMMAND_SUBSYSTEM
    END IF

    IF V4L2 video device registration fails THEN
        GOTO REMOVE_I2C_ADAPTER
    END IF

    IF ALSA audio device registration fails THEN
        GOTO UNREGISTER_VIDEO
    END IF

    Log registered video and audio devices
    RETURN success

UNREGISTER_VIDEO:
    Unregister V4L2 video and associated media resources

REMOVE_I2C_ADAPTER:
    Remove Linux I2C adapter

FREE_COMMAND_SUBSYSTEM:
    Free command buffers

RELEASE_DEVICE:
    Clear interface driver data
    Release driver-state reference
    Release USB references when final reference is dropped
    RETURN failure
END PROCEDURE
```

### 2.3 Disconnect the USB device

```text
PROCEDURE USB_DISCONNECT(interface)
    dev = Get driver state associated with interface

    IF dev does not exist THEN
        RETURN
    END IF

    Clear interface driver data

    Acquire stream lifecycle mutex
    Acquire stream lock
        Mark device as disconnected
        Mark streaming as inactive
    Release stream lock
    Release stream lifecycle mutex

    Disconnect V4L2 device so new userspace operations fail

    Acquire V4L2 mutex
        Mark video buffer queue as erroneous
        Stop and release streaming resources
        Disconnect and unregister ALSA card
        Unregister V4L2 and media resources
    Release V4L2 mutex

    Remove I2C adapter
    Free command buffers

    Drop driver's device reference
    Existing open references may keep state alive until released
END PROCEDURE
```

---

## 3. FX3 command transport and hardware bootstrap

### 3.1 Command transport safety

Command traffic uses Bulk OUT endpoint `0x01` and Bulk IN endpoint `0x81`.
The video/audio stream arrives separately on Bulk IN endpoint `0x83`.

```text
PROCEDURE SEND_COMMAND(dev, transmit_packet)
    REQUIRE command mutex is held

    IF device is disconnected OR transmit buffer is unavailable THEN
        RETURN device-not-present error
    END IF

    IF packet exceeds maximum command packet size THEN
        RETURN invalid-argument error
    END IF

    IF packet opcode is a blocked CryptoMemory operation THEN
        Log blocked operation
        RETURN permission error
    END IF

    Copy packet into command transfer buffer
    Send packet through USB Bulk OUT endpoint 0x01

    IF USB transfer fails THEN
        RETURN USB error
    END IF

    RETURN success
END PROCEDURE
```

The driver blocks the AT88SC CryptoMemory opcodes used for authentication or
protected-memory access. The bootstrap must not replay authentication challenges
that can consume the chip's finite failed-attempt counter.

### 3.2 Send a command and receive its response

```text
PROCEDURE COMMAND_TRANSFER(dev, transmit_packet, expected_response_length)
    REQUIRE command mutex is held

    Validate expected response length
    Send command through SEND_COMMAND

    IF send failed THEN
        RETURN error
    END IF

    Read response from USB Bulk IN endpoint 0x81
    Read into a buffer large enough for the endpoint's maximum packet size

    IF USB read failed THEN
        RETURN USB error
    END IF

    IF actual response length is smaller than expected THEN
        RETURN short-response error
    ELSE IF actual response length is larger than expected THEN
        RETURN protocol-length error
    END IF

    Copy expected response bytes to caller when requested
    RETURN number of received bytes
END PROCEDURE
```

### 3.3 I2C operations over the FX3 bridge

```text
PROCEDURE FX3_I2C_WRITE(slave_address, register, payload)
    REQUIRE payload length is at most 32 bytes

    packet =
        I2C_WRITE opcode
        + 7-bit slave address
        + register-address-width (1 byte)
        + register
        + payload

    Acquire command mutex
    SEND_COMMAND(packet)
    Release command mutex

    RETURN result
END PROCEDURE
```

```text
PROCEDURE FX3_I2C_READ(slave_address, register, requested_length)
    REQUIRE requested length is between 1 and 64 bytes

    packet =
        I2C_READ opcode
        + 7-bit slave address
        + register-address-width (1 byte)
        + requested length
        + register

    Acquire command mutex
    COMMAND_TRANSFER(packet, requested_length)
    Release command mutex

    RETURN received data or error
END PROCEDURE
```

### 3.4 Detect existing board state

```text
PROCEDURE DETECT_BOARD_STATE(dev)
    Drain stale command responses

    Read ADV7604 IO register 0xFA
    Read PCA9539A configuration registers starting at 0x06

    IF either required read fails THEN
        RETURN error
    END IF

    IF observed register values do not match known configured state THEN
        RETURN "Bootstrap required"
    END IF

    Read additional ADV7604 format and sub-map registers (IO[0x03], IO[0xF6], IO[0xF7])
    Read ADV7511 transmitter power register (Main[0x41])

    IF all expected post-bootstrap values match THEN
        RETURN "Already configured"
    END IF

    RETURN "Bootstrap required"
END PROCEDURE
```

### 3.5 Execute the staged hardware initialization sequence

The initialization sequence is organized as 27 ordered stages combining symbolic
board register operations and slices of the compacted binary command blob
(`2,615` total operations). Each blob record contains a transmit length, expected
receive length, and transmit data.

```text
PROCEDURE RUN_INITIALIZATION_SEQUENCE(dev)
    Acquire command mutex

    FOR each stage in the 27-stage initialization schedule
        IF device is disconnected THEN
            result = device-not-present error
            BREAK
        END IF

        IF stage is a symbolic board-register stage THEN
            Execute symbolic I2C/bridge operations for this stage
        ELSE (compacted init-blob slice stage)
            FOR each record in stage blob slice
                Read transmit length, expected receive length, and payload
                IF record extends beyond slice boundary THEN
                    result = invalid-table error
                    BREAK
                END IF

                Apply unconditional subdevice-aware command translation

                IF expected receive length > 0 THEN
                    Send command and validate response length
                ELSE
                    Send command without waiting for a data response
                END IF
            END FOR
        END IF

        IF any operation failed THEN
            Log failed stage/step
            BREAK
        END IF
    END FOR

    Drain stale command responses
    Release command mutex

    RETURN result
END PROCEDURE
```

### 3.6 Bootstrap decision

```text
PROCEDURE BOOTSTRAP_HARDWARE(dev)
    IF dev is already marked bootstrapped THEN
        RETURN success
    END IF

    board_state = DETECT_BOARD_STATE(dev)

    IF device was disconnected THEN
        RETURN device-not-present error
    END IF

    IF board state could not be queried for another reason THEN
        Warn and attempt cold bootstrap
    END IF

    IF board is already configured THEN
        Reuse current hardware state
    ELSE
        RUN_INITIALIZATION_SEQUENCE(dev)

        IF initialization failed THEN
            RETURN error
        END IF
    END IF

    Set ADV7604 input colour-space selection to automatic
    Reset contrast, saturation, brightness, and hue to neutral

    IF colour-space configuration failed THEN
        RETURN error
    END IF

    Mark hardware as bootstrapped
    RETURN success
END PROCEDURE
```

Bootstrap is deliberately not repeated after the device has been marked initialized.
Bootstrap is performed unconditionally during probe before registering the ADV7604 and
ADV7511 I2C clients.

---

## 4. Linux I2C adapter

The driver exposes the FX3 bridge as a Linux I2C adapter. The adapter validates
the Linux I2C message pattern before translating it to the FX3 command format.

```text
PROCEDURE I2C_MASTER_TRANSFER(messages)
    IF device is disconnected THEN
        RETURN device-not-present error
    END IF

    IF message list is empty or invalid THEN
        RETURN invalid-argument error
    END IF

    Reject 10-bit addressing
    Reject slave addresses outside the 7-bit range

    IF exactly one message is supplied THEN
        Require a write operation
        Require at least one byte for register offset
        Set first byte as register address
        Treat remaining bytes as write payload

        IF payload exceeds 32 bytes THEN
            RETURN unsupported-operation error
        END IF

        FX3_I2C_WRITE(address, register, payload)
        RETURN 1 message processed
    END IF

    IF exactly two messages are supplied THEN
        Require same slave address in both messages
        Require first message to be a one-byte register write
        Require second message to be a read
        Require read length from 1 through 64 bytes

        FX3_I2C_READ(address, register, requested length)
        Copy data into second message
        RETURN 2 messages processed
    END IF

    Reject unsupported multi-message sequence
END PROCEDURE
```

Adapter registration creates a Linux I2C adapter with the driver's transfer
algorithm and transfer quirks. Removal unregisters it from the I2C core.

---

## 5. V4L2 video device and unified subdevices

### 5.1 Register video capture and Media Controller graph

```text
PROCEDURE REGISTER_VIDEO_DEVICE(dev)
    Initialize video buffer list and queue lock

    Set default pixel format:
        width = 1920
        height = 1080
        pixel format = YUYV
        field = progressive
        bytes per line = width * 2
        image size = bytes per line * height
        colour metadata = Rec.709, limited range

    Initialize Media Controller device
    Register V4L2 device
    Acquire reference to driver state

    Register ADV7604 and ADV7511 subdevices unconditionally

    Initialize videobuf2 queue:
        capture queue
        MMAP, USERPTR, DMABUF, and read I/O modes
        driver buffer structure
        vmalloc memory operations
        minimum queued buffers = 2

    Initialize video_device:
        capture capabilities
        V4L2 file operations
        ioctl operations
        videobuf2 queue
        media entity sink pad

    Register /dev/video device

    Create immutable enabled Media Controller fan-out links:
        ADV7604 source pad 6 -> GC550 video sink pad 0
        ADV7604 source pad 6 -> ADV7511 sink pad 0

    Register /dev/media device
    RETURN success

ON ANY FAILURE:
    Unregister resources already created, in reverse order
    RETURN error
END PROCEDURE
```

### 5.2 Unconditional subdevice registration

```text
PROCEDURE REGISTER_SUBDEVICES(dev)
    Require registered I2C adapter and V4L2 device

    BOOTSTRAP_HARDWARE(dev)

    IF bootstrap failed OR hardware is not marked initialized THEN
        RETURN error
    END IF

    Register ADV7604 receiver subdevice on its configured I2C address (0x20)
    IF registration fails THEN
        RETURN error
    END IF

    Register ADV7511 transmitter subdevice on its configured I2C address (0x3d)
    IF registration fails THEN
        Unregister ADV7604
        RETURN error
    END IF

    Invoke ADV7511 core s_power(1) to restore power-on state (Main[0x41] = 0x10)
    Configure ADV7604 source-pad format to match current timings/pixel format

    IF configuration fails THEN
        Unregister both subdevices
        RETURN error
    END IF

    RETURN success
END PROCEDURE
```

### 5.3 Pixel format and digital video timings

```text
PROCEDURE SET_VIDEO_FORMAT(requested_format)
    IF video buffer queue is busy THEN
        RETURN busy error
    END IF

    Validate and normalize requested format
    Store accepted pixel format
    RETURN success
END PROCEDURE
```

```text
PROCEDURE ENUM_OR_CAP_DV_TIMINGS(request)
    Require ADV7604 subdevice
    IF request.pad != 0 THEN
        RETURN -EINVAL
    END IF

    Forward pad-zeroed query to ADV7604 subdevice
END PROCEDURE
```

```text
PROCEDURE SET_DIGITAL_VIDEO_TIMINGS(requested_timings)
    Require ADV7604 subdevice
    Require video queue to be idle
    Require BT.656 / BT.1120 timing type
    Require requested dimensions to be supported in gc550_frame_sizes[]

    Read current ADV7604 timings

    IF current timings already match request THEN
        RETURN success
    END IF

    Apply requested timings to ADV7604 HDMI input

    IF receiver rejects timings THEN
        RETURN error
    END IF

    Update V4L2 width, height, line length, and image size
    Reconfigure ADV7604 source-pad format

    RETURN success
END PROCEDURE
```

---

## 6. Video streaming lifecycle

### 6.1 Start streaming

```text
PROCEDURE START_VIDEO_STREAM(dev, queued_buffer_count)
    Reset stream parser
    Reset frame sequence and per-session audio counters

    IF device is disconnected THEN
        Return queued buffers
        RETURN device-not-present error
    END IF

    Require completed bootstrap
    Require ADV7604 and ADV7511 subdevices

    Configure ADV7604 format
    Query ADV7604 input signal status when available

    Acquire stream lifecycle mutex

    Allocate stream resources:
        Allocate DMA pair reorder buffer
        Allocate URBs
        Allocate one transfer buffer for each URB
        Configure each URB to receive from Bulk IN endpoint 0x83

    IF allocation fails THEN
        Free partial allocations
        Release mutex
        Return queued buffers
        RETURN error
    END IF

    Acquire stream lock
    IF device became disconnected THEN
        result = device-not-present error
    ELSE
        Mark streaming active
    END IF
    Release stream lock

    IF no error THEN
        Submit all stream URBs using normal process context
    END IF

    IF submission fails THEN
        Mark streaming inactive
        Kill/free submitted and allocated URBs
        Return queued buffers
    END IF

    Release stream lifecycle mutex

    RETURN success
END PROCEDURE
```

### 6.2 URB completion callback

The completion callback handles received USB data and resubmits the URB.
It must not sleep; resubmission uses atomic allocation context.

```text
PROCEDURE STREAM_URB_COMPLETION(urb)
    dev = urb->context

    IF no device state exists THEN
        RETURN
    END IF

    Acquire stream lock
    IF stream is inactive OR device is disconnected THEN
        Release stream lock
        RETURN
    END IF
    Release stream lock

    IF USB status indicates successful completion THEN
        IF received length > 0 THEN
            PARSE_STREAM_CHUNK(dev, received bytes)
        END IF

    ELSE IF status indicates cancellation or shutdown THEN
        RETURN

    ELSE
        Log USB transfer error (dev_warn_ratelimited)
        Abort active video frame
        Reset stream parser
    END IF

    Acquire stream lock
    IF streaming is still active AND device is connected THEN
        Resubmit same URB using atomic context
    ELSE
        Do not resubmit
    END IF
    Release stream lock

    Log resubmission failure where appropriate (dev_err_ratelimited)
END PROCEDURE
```

The stream lock protects the submission gate. STREAMOFF and disconnect close
that gate before waiting for URBs, preventing completion callbacks from submitting
new URBs after shutdown begins.

### 6.3 Stop streaming

```text
PROCEDURE STOP_STREAM_RESOURCES(dev, buffer_result)
    Acquire stream lifecycle mutex
    Acquire stream lock
        Mark streaming inactive
    Release stream lock

    Kill and free URBs
    Free URB transfer buffers
    Free DMA pair buffer
    Release stream lifecycle mutex

    Return all queued/current video buffers with buffer_result
END PROCEDURE
```

---

## 7. USB stream parser

### 7.1 Stream marker format

The parser consumes the multiplexed byte stream from Bulk IN endpoint `0x83`.

| Marker | Prefix | Header / content |
|---|---|---|
| SAV | `00 FF FF C0` | 4-byte header |
| EAV | `00 FF FF C1` | 5-byte header |
| Audio | `00 FF FF 58` | 4-byte big-endian payload length, then PCM |

SAV checksum:

```text
expected = (header[0] + header[1] + header[2] - 0x40) modulo 256
valid = header[3] equals expected
```

EAV checksum:

```text
expected = (header[0] + header[1] + header[2] + header[3] - 0x3F) modulo 256
valid = header[4] equals expected
```

The video payload is delivered as YUYV/YUY2 4:2:2. The default 1920 × 1080
frame occupies 4,147,200 bytes.

Audio packet length is encoded as a big-endian 32-bit value. Audio samples are
16-bit little-endian stereo PCM at 48 kHz.

### 7.2 Parser state machine

Conceptual states include:

- **IDLE** — search for a recognized marker.
- **SAV_HEADER** — collect and validate the SAV header.
- **VIDEO_DATA** — append video payload until EAV or another marker.
- **EAV_HEADER** — collect and validate the EAV header.
- **AUDIO_HEADER** — collect the four-byte audio payload length.
- **AUDIO_DATA** — consume the declared number of audio bytes.

```text
PROCEDURE PARSE_ORDERED_BYTES(dev, input_bytes)
    FOR each byte in input_bytes
        SWITCH parser state

        CASE SAV_HEADER:
            Append byte to SAV header
            IF four header bytes have been collected THEN
                IF SAV checksum is valid THEN
                    RESERVE_VIDEO_FRAME()
                    Set parser state to VIDEO_DATA
                ELSE
                    Increment desynchronization counter
                    Mark parser idle
                    Abort current video frame as erroneous
                END IF
            END IF

        CASE EAV_HEADER:
            Append byte to EAV header
            IF five header bytes have been collected THEN
                IF EAV checksum is valid THEN
                    COMPLETE_VIDEO_FRAME()
                    Set parser state to IDLE
                ELSE
                    Increment desynchronization counter
                    Abort current video frame as erroneous
                    Set parser state to IDLE
                END IF
            END IF

        CASE AUDIO_HEADER:
            Append byte to audio header
            IF four header bytes have been collected THEN
                Decode declared payload length as big-endian 32-bit integer
                Validate and initialize audio payload tracking
                Set parser state to AUDIO_DATA
            END IF

        CASE AUDIO_DATA:
            Consume no more than remaining declared audio bytes
            Send consumed bytes to ALSA audio path
            Decrease remaining audio byte count

            IF all declared audio bytes are consumed THEN
                Set parser state to IDLE
            END IF

        CASE VIDEO_DATA:
            IF a complete recognized marker begins here THEN
                Handle marker and change parser state
            ELSE
                Append byte to current video frame
            END IF

        CASE IDLE:
            Feed byte to marker-prefix detector
            When a complete recognized marker is found:
                SAV   -> Set state to SAV_HEADER
                EAV   -> Handle only if currently in a valid video context
                AUDIO -> Set state to AUDIO_HEADER
        END SWITCH
    END FOR
END PROCEDURE
```

The implementation includes marker-prefix straddle handling. Partial prefixes
(`00`, `00 FF`, or `00 FF FF`) are retained across URB boundaries so a marker
split between USB transfers is not mistaken for payload data.

### 7.3 Reserve a video frame

```text
PROCEDURE RESERVE_VIDEO_FRAME(dev)
    IF a previous frame buffer is still active THEN
        Complete previous buffer with ERROR
    END IF

    Assign next frame sequence number
    Save SAV header's identifying byte
    Reset current video byte count
    Set parser state to VIDEO_DATA

    Acquire video queue lock
    IF a queued V4L2 buffer exists THEN
        Remove first queued buffer
        Make it the active frame buffer
    ELSE
        Mark that no destination buffer is currently available
    END IF
    Release video queue lock
END PROCEDURE
```

### 7.4 Append video payload

```text
PROCEDURE APPEND_VIDEO_BYTES(dev, bytes)
    IF no active video frame buffer exists THEN
        Do not copy payload
    ELSE
        Copy only bytes that fit in the configured V4L2 frame size
    END IF

    Increase parser video span counter
    Saturate counter above maximum accepted frame span
    Never allow a missing EAV to wrap the span counter
END PROCEDURE
```

### 7.5 Complete a video frame

```text
PROCEDURE COMPLETE_VIDEO_FRAME(dev, EAV_header)
    Detach current buffer from active parser state

    IF frame span is a possible FX3 DMA swap candidate
       AND EAV identifying byte differs from SAV identifying byte THEN

        Increment swap vote counter

        IF two consecutive candidates have been observed THEN
            Toggle DMA swap mode
            Reset swap vote counter and pair accumulator position
        END IF

        Complete current buffer with ERROR, if one exists
        Reset video span counter
        RETURN
    END IF

    Reset swap vote counter

    IF video span is outside configured frame-size tolerance THEN
        Complete current buffer with ERROR, if one exists
        Reset video span counter
        RETURN
    END IF

    IF a frame buffer exists THEN
        IF received payload is shorter than configured image size THEN
            Fill remaining bytes with YUYV black pixels
        END IF

        Set timestamp, field, sequence, and payload size
        Complete V4L2 buffer with DONE
    END IF

    Reset video span counter
END PROCEDURE
```

### 7.6 Abort or replace an incomplete frame

```text
PROCEDURE ABORT_VIDEO_FRAME(dev)
    Detach active video buffer under queue lock

    IF buffer exists THEN
        Set timestamp and frame metadata
        Complete buffer with ERROR
    END IF

    Reset video byte count
END PROCEDURE
```

A new valid SAV also causes an unclosed active frame to be completed with ERROR
before reserving the next frame.

---

## 8. FX3 16 KiB DMA pair reordering

The stream parser supports a detected swapped state in which each 32 KiB pair of
stream data must be consumed as its second 16 KiB half followed by its first
16 KiB half.

```text
PROCEDURE PARSE_STREAM_CHUNK(dev, bytes)
    IF swap mode is disabled OR pair buffer is unavailable THEN
        PARSE_ORDERED_BYTES(bytes)
        RETURN
    END IF

    WHILE input bytes remain
        Copy as many bytes as fit into the 32 KiB pair buffer
        Advance pair-buffer position

        IF pair buffer now contains a full 32 KiB pair THEN
            Reset pair-buffer position

            Parse bytes from second 16 KiB half first
            Parse bytes from first 16 KiB half second
        END IF
    END WHILE
END PROCEDURE
```

The swap detector votes on candidate frame boundaries and changes mode after
two consecutive candidates. A candidate frame is marked erroneous rather than
being delivered as a valid frame.

This describes the current implementation; it does not imply that every possible
DMA phase transition or every resolution has been proven on physical hardware.

---

## 9. ALSA audio capture

Audio is multiplexed into the same USB stream as video. The audio parser extracts
the audio payload and forwards it to the ALSA PCM capture ring buffer.

### 9.1 PCM capabilities

The current PCM hardware description advertises:

- Format: signed 16-bit little-endian (`S16_LE`).
- Sample rate: 48,000 Hz.
- Channels: 2 (stereo).
- Buffer maximum: 64 KiB.
- Period size: 1,600 bytes minimum and 16 KiB maximum.
- Period count: 2 through 32.

### 9.2 Register the ALSA card

```text
PROCEDURE REGISTER_ALSA_DEVICE(dev)
    Initialize ALSA lock and in-flight wait queue

    Allocate ALSA sound card
    Acquire a device-state reference for the card
    Create one PCM capture device
    Install PCM operations:
        open
        close
        hardware parameters
        hardware free
        prepare
        trigger
    Preallocate continuous PCM pages
    Register sound card

    IF any step fails THEN
        Release card and associated reference
        RETURN error
    END IF

    Store sound card and PCM pointers in device state
    RETURN success
END PROCEDURE
```

### 9.3 PCM trigger

```text
PROCEDURE ALSA_PCM_TRIGGER(substream, command)
    Acquire ALSA lock

    IF command is START THEN
        IF ALSA device is disconnected THEN
            result = device-not-present error
        ELSE
            Set active ALSA capture substream
        END IF

    ELSE IF command is STOP THEN
        Clear active ALSA capture substream

    ELSE
        result = invalid-command error
    END IF

    Release ALSA lock
    RETURN result
END PROCEDURE
```

### 9.4 Push audio samples

```text
PROCEDURE ALSA_PUSH_AUDIO_SAMPLES(dev, sample_bytes)
    Under ALSA synchronization:
        Check whether a capture substream is active
        Check that audio is not disconnected
        Account for an in-flight audio callback
        Obtain the ALSA runtime ring buffer and current write position

        Copy sample bytes into the ring buffer, respecting wrap-around
        Advance hardware pointer and period position

        Detect completed PCM periods

    Notify ALSA when one or more periods have elapsed
    Release in-flight accounting and wake any waiting teardown path
END PROCEDURE
```

The V4L2 video stream must be active to receive the multiplexed USB data from
which the audio packets are extracted. Opening the ALSA device alone does not
start a separate USB audio endpoint.

### 9.5 Disconnect or close ALSA

```text
PROCEDURE QUIESCE_ALSA(dev, optional_substream, disconnect)
    Acquire ALSA lock

    IF disconnect is requested THEN
        Mark ALSA device disconnected
    END IF

    Clear active substream when it matches the requested substream
    Release ALSA lock

    Wait until all in-flight audio callbacks have completed
END PROCEDURE
```

---

## 10. Native kernel diagnostics

The driver uses standard Linux kernel logging and V4L2 status reporting rather than
a custom diagnostic ring buffer or debugfs interface:

- **Parser anomalies (`dev_dbg_ratelimited()`, `dynamic_debug`):** Logs unclosed-frame aborts, FX3 DMA swap votes, rejected invalid frame spans, SAV/EAV checksum failures, and EAV sequence mismatches with zero runtime overhead when disabled.
- **State transitions & errors (`dev_info*()` / `dev_warn_ratelimited()` / `dev_err*()`):** Logs FX3 DMA swap mode switches, USB URB transfer/submission errors, ADV7604 input lock on stream start, and aggregate stream counters (`frames`, `audio_bytes`, `desyncs`, `swap_mode`) on stream stop.
- **V4L2 status inspection (`VIDIOC_LOG_STATUS`):** Logs GC550 stream/parser summary counters (`active`, `fmt`, `frames`, `audio_bytes`, `desyncs`, `swap`) followed by `v4l2_device_call_all(..., log_status)` for `adv7604` and `adv7511-v4l2`.

---

## 11. Resource ownership and teardown

The driver uses reference counting because V4L2, ALSA, and open file handles may
outlive the initial USB probe/disconnect callback.

```text
PROCEDURE RELEASE_DEVICE_STATE(dev)
    Called only when final device-state reference is dropped

    Release USB interface reference
    Release USB device reference
    Free device-state memory
END PROCEDURE
```

The major teardown ordering is:

1. Close the stream submission gate.
2. Mark device disconnected or streaming inactive.
3. Stop/kill URBs and wait for callbacks to finish.
4. Return active and queued V4L2 buffers.
5. Quiesce ALSA callbacks and disconnect the sound card.
6. Unregister V4L2/media/subdevice resources.
7. Remove the I2C adapter and command resources.
8. Drop references; free final state only when no users remain.

Error paths during probe and registration unwind resources in reverse order of
successful initialization.

---

## 12. Main execution flow — end-to-end

```text
MAIN DRIVER FLOW

    MODULE_START

    WHEN supported GC550 USB interface is connected:
        USB_PROBE

        INITIALIZE command transport
        REGISTER Linux I2C adapter
        REGISTER V4L2 device, ADV7604/ADV7511 subdevices, and Media Controller links
        REGISTER ALSA PCM capture card

    WHEN userspace queues video buffers and starts V4L2 streaming:
        START_VIDEO_STREAM

        Verify prior bootstrap and subdevice registration
        Reconfigure ADV7604 format
        Query ADV7604 input signal status when available

        Allocate and submit stream URBs to endpoint 0x83

    FOR every completed stream URB:
        STREAM_URB_COMPLETION
            PARSE_STREAM_CHUNK
                Reorder 16 KiB DMA halves when swap mode is active
                PARSE_ORDERED_BYTES
                    SAV -> reserve V4L2 frame
                    video bytes -> append to V4L2 frame
                    EAV -> validate and complete V4L2 frame
                    audio marker -> decode length and extract PCM
                        ALSA_PUSH_AUDIO_SAMPLES
            Resubmit URB while streaming gate remains open

    WHEN userspace stops V4L2 streaming:
        STOP_STREAM_RESOURCES
        Kill URBs
        Return buffers
        Report stream counters

    WHEN USB device is unplugged:
        USB_DISCONNECT
        Quiesce stream and ALSA paths
        Unregister devices and adapters
        Release references
END MAIN DRIVER FLOW
```

---

## 13. Important implementation constraints represented here

| Area | Current implementation constraint |
|---|---|
| USB identity | GC550 product IDs `07ca:4710` and `07ca:0550` |
| Interface | Only interface 0 is bound |
| USB command OUT | Endpoint `0x01` |
| USB command IN | Endpoint `0x81` |
| USB stream IN | Endpoint `0x83` |
| Video format | YUYV / YUY2, 4:2:2 |
| Default video mode | 1920 × 1080 progressive |
| Other advertised dimensions | 1280×720, 720×576, 720×480, 640×480 |
| Audio format | S16_LE, 48 kHz, stereo |
| I2C addressing | 7-bit only |
| I2C write payload | At most 32 bytes |
| I2C read payload | 1–64 bytes |
| I2C transaction forms | Single register-prefixed write, or same-address register-write + read |
| DMA reordering | 32 KiB pair assembled from two 16 KiB halves |
| CryptoMemory | Protected opcodes are blocked |
| Subdevice architecture | Unified unconditional ADV7604/ADV7511 Media Controller graph (zero module parameters) |

---

## 14. What this pseudocode deliberately does not claim

- It does not reproduce every V4L2 ioctl, control, capability query, or format-enumeration branch.
- It does not define the Linux kernel's internal locking, memory barriers, URB, videobuf2, ALSA, I2C, or reference-counting semantics in full.
- It does not claim that source-level I2C support proves every transaction on physical hardware.
- It does not claim that all USB host controllers, kernel versions, resolutions, and DMA phase transitions have been hardware-validated.
- It does not describe the DKMS installer or packaging scripts as part of the kernel module's runtime logic.
- It does not replace the protocol specification, register provenance, hardware lifecycle, or test documentation.

For exact behavior, error codes, synchronization, and kernel API details, the C source remains authoritative.
