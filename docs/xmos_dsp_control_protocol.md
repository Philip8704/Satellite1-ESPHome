# XMOS DSP Control Protocol (Audio Servicer)

Canonical specification for the audio DSP control surface exposed by the
Satellite1 XMOS firmware to the ESP32 host over SPI.

This document is the contract. The ESPHome `xvf_control` component issues
commands defined here against the XMOS audio servicer. A Satellite1-XMOS
firmware build that registers a servicer at RESID `0xE0` and implements
the command IDs below is automatically discovered and exposes the matching
runtime entities in Home Assistant.

The current FPH stock firmware (`satellite1_firmware_fixed_delay.factory.bin`
v1.0.3) does **not** implement this servicer. Reads return `BAD_RESOURCE`
and the `xvf_control` component flags itself as unsupported, hiding the
HA entities. Implementing this servicer is the Phase 3 work-track described
in CLAUDE.md.

## Transport

Per FPH's existing `device_control` framework
([`modules/fph/rtos_device_control/api/device_control_shared.h`](https://github.com/FutureProofHomes/Satellite1-XMOS/blob/main/modules/fph/rtos_device_control/api/device_control_shared.h)):

- SPI MODE3, MSB first, ≤ 8 MHz (ESPHome uses 8 MHz).
- Each transaction is a `[resource_id, command, payload...]` frame.
- High bit of `command` (`0x80`) marks a read. Write commands have the
  high bit clear.
- Status byte returned in the first byte of the read response. `0` =
  success, `CONTROL_BAD_RESOURCE` (5) = servicer not registered for that
  RESID, `CONTROL_BAD_COMMAND` (1) = command not implemented, etc.

## Resource ID assignment

| RESID    | Servicer                            | Status                          |
|----------|-------------------------------------|---------------------------------|
| `0x01`   | Special / control library           | stock — `CONTROL_SPECIAL_RESID` |
| `0xD2-D4`| GPIO controller (210, 211, 212, 221)| stock                           |
| `0xE0`   | **Audio DSP control**               | **Phase 3 — this document**     |
| `0xF0`   | DFU controller (240)                | stock                           |

`0xE0` is chosen because it sits in the free range 0xD5–0xEF and is the
conventional placement for XMOS audio control services in XVF reference
designs.

## Command map

Customer-defined commands per `cmd_map.h` start at offset `110` (0x6E).
All commands listed below are write-capable unless marked R/O; the high
bit of the command byte (`0x80`) issued by the host indicates read.

| Cmd ID | Name                       | Payload (write)              | Payload (read)             | Notes                                                                       |
|--------|----------------------------|------------------------------|----------------------------|-----------------------------------------------------------------------------|
| `110`  | `CMD_CAPABILITY_FLAGS`     | —                            | 4 bytes, little-endian u32 | R/O. Bit-mask of supported features (see below). Probe at boot.             |
| `111`  | `CMD_FW_FEATURE_VERSION`   | —                            | 4 bytes: `[maj,min,pat,0]` | R/O. Independent of XMOS bootloader version; tracks audio servicer SemVer.  |
| `112`  | `CMD_BEAM_MODE`            | 1 byte: `0` fixed / `1` adaptive / `2` tracking | 1 byte               | Tracking = adaptive beamformer with DOA-driven re-steer.                    |
| `113`  | `CMD_BEAM_ANGLE`           | 2 bytes signed int16, degrees `-180..+180` (0° = HAT front) | 2 bytes | Only acted upon when mode is `fixed`. Adaptive/tracking modes ignore writes. |
| `114`  | `CMD_DOA_ANGLE`            | —                            | 2 bytes signed int16, degrees | R/O. Last estimated direction-of-arrival.                                |
| `115`  | `CMD_DOA_CONFIDENCE`       | —                            | 1 byte, `0..255`           | R/O. SRP-PHAT peak energy / mean ratio scaled to byte.                     |
| `116`  | `CMD_AEC_MODE`             | 1 byte: `0` bypass / `1` linear / `2` linear+residual | 1 byte | Residual = nonlinear post-filter. `linear` matches stock fixed_delay behavior. |
| `117`  | `CMD_AEC_REF_GAIN_DB`      | 1 byte signed int8, dB `-24..+12` | 1 byte                 | Trim on the loudspeaker reference signal feeding AEC. Default 0 dB.        |
| `118`  | `CMD_AGC_ENABLE`           | 1 byte `0/1`                 | 1 byte                     | `0` disables AGC stage entirely. Stock = `1`.                              |
| `119`  | `CMD_AGC_TARGET_DBFS`      | 1 byte signed int8, dBFS `-60..0` | 1 byte                | Target output level. Stock pipeline targets approx. -23 dBFS.              |
| `120`  | `CMD_NS_ENABLE`            | 1 byte `0/1`                 | 1 byte                     | `0` disables NS stage entirely. Stock = `1`.                               |
| `121`  | `CMD_NS_LEVEL`             | 1 byte, `0..3`               | 1 byte                     | `0` = lightest, `3` = most aggressive. Stock pipeline tunes ~level 2.       |
| `122`  | `CMD_MIC_GAIN_L`           | 1 byte signed int8, dB `-12..+24` | 1 byte                | Per-channel digital gain on PDM mic L *before* AEC/IC/NS/AGC.              |
| `123`  | `CMD_MIC_GAIN_R`           | 1 byte signed int8, dB `-12..+24` | 1 byte                | Per-channel digital gain on PDM mic R.                                     |
| `124`  | `CMD_PIPELINE_STATS`       | —                            | 16 bytes (struct below)    | R/O. Per-frame DSP load and clip counters. Polled at ~1 Hz.                 |

### Capability flag bits (`CMD_CAPABILITY_FLAGS`)

```
bit 0 : BEAM_FIXED              fixed-angle beamforming supported
bit 1 : BEAM_ADAPTIVE           adaptive beamforming supported (online estimation)
bit 2 : BEAM_TRACKING           adaptive beam + DOA-driven re-steer
bit 3 : DOA_REPORTING           CMD_DOA_ANGLE / CMD_DOA_CONFIDENCE return non-zero
bit 4 : AEC_TUNING              CMD_AEC_MODE / CMD_AEC_REF_GAIN_DB honored
bit 5 : AGC_TUNING              CMD_AGC_* honored
bit 6 : NS_TUNING               CMD_NS_* honored
bit 7 : MIC_GAIN_RUNTIME        CMD_MIC_GAIN_* honored at runtime
bit 8 : PIPELINE_STATS          CMD_PIPELINE_STATS returns valid struct
bits 9..31 : reserved, zero
```

Firmware that implements `CMD_CAPABILITY_FLAGS` but no other commands
returns `0` — the ESPHome side treats that as "audio servicer exists but
all features compiled out" and hides every runtime entity.

### `CMD_PIPELINE_STATS` struct

```c
struct __attribute__((packed)) pipeline_stats {
    uint32_t frames_since_boot;
    uint16_t dsp_load_q8_8;       // tile DSP load, fixed-point 8.8 (%)
    uint16_t clip_count_mic_l;    // PDM mic L clip events since boot
    uint16_t clip_count_mic_r;    // PDM mic R clip events since boot
    uint16_t aec_ref_clip_count;  // loudspeaker reference clip events
    int16_t  doa_angle_deg;       // most recent DOA, signed
    uint8_t  doa_confidence;      // 0..255
    uint8_t  reserved;            // pad to 16 bytes
};
```

## Behavior contract

1. Audio servicer task runs on tile 0 (same tile as the existing GPIO and
   DFU servicers per FPH's `servicer.c` dispatch). Registration occurs in
   `app_xvf3800_init` via `device_control_servicer_register()` with
   `resource_id = 0xE0`.
2. All writes are applied atomically at the next pipeline frame boundary
   (so a `BEAM_ANGLE` change doesn't take effect mid-block-process). Cmd
   ack returns `CONTROL_SUCCESS` before the change has actually taken
   effect — host polls reads to confirm.
3. Reads are always served from the most recent stable state (no
   half-applied parameter snapshots).
4. Bounds are clamped silently. A write of `CMD_BEAM_ANGLE = 270` is
   stored as `+180`, read returns `+180`. Host is expected to validate
   before sending, but firmware must never crash on out-of-range input.
5. Capabilities are checked once per ESP32 boot. A capability flag bit
   may NOT change across the lifetime of a single XMOS firmware version
   (so the ESPHome side can cache and not re-poll).
6. On firmware mismatch (capability flags = 0 OR `BAD_RESOURCE` returned)
   the ESPHome side falls back to the stock pipeline behavior — no
   entities are exposed, no SPI traffic is generated post-probe.

## ESPHome-side SPI servicer ID

The ESPHome `xvf_control` component uses these constants
(see [`esphome/components/satellite1/satellite1.h`](../esphome/components/satellite1/satellite1.h)):

```cpp
static const uint8_t AUDIO_SERVICER_RESID = 0xE0;

namespace audio_cmd {
constexpr uint8_t CAPABILITY_FLAGS    = 110;
constexpr uint8_t FW_FEATURE_VERSION  = 111;
constexpr uint8_t BEAM_MODE           = 112;
constexpr uint8_t BEAM_ANGLE          = 113;
constexpr uint8_t DOA_ANGLE           = 114;
constexpr uint8_t DOA_CONFIDENCE      = 115;
constexpr uint8_t AEC_MODE            = 116;
constexpr uint8_t AEC_REF_GAIN_DB     = 117;
constexpr uint8_t AGC_ENABLE          = 118;
constexpr uint8_t AGC_TARGET_DBFS     = 119;
constexpr uint8_t NS_ENABLE           = 120;
constexpr uint8_t NS_LEVEL            = 121;
constexpr uint8_t MIC_GAIN_L          = 122;
constexpr uint8_t MIC_GAIN_R          = 123;
constexpr uint8_t PIPELINE_STATS      = 124;
}
```

## Recommended Phase 3 implementation plan (XMOS side)

For whoever implements the XMOS firmware against this spec:

1. **Pipeline:** Fork `satellite-xmos-firmware/audio_pipelines/reference/fixed_delay`
   into a new `beamforming` variant. Insert a `bf_stage_ctx_t` between
   mic_array decimation and the AEC stage. Use the XVF3800 SDK's
   adaptive beamformer reference (lib_aec / lib_ic ship the building
   blocks; the GSC topology is in the XVF3800 SDK example
   `xvf3800-far-field-voice-assistant`).
2. **Servicer:** Add `satellite-xmos-firmware/src/audio/audio_servicer.{c,h}`
   following the layout of `src/gpio/gpio_servicer.{c,h}`. Register at
   RESID `0xE0` with `audio_cmd_t` table per this doc.
3. **Parameter store:** Single `rtos_osal_mutex`-guarded `audio_params_t`
   struct, written by servicer, read at the start of each `audio_pipeline_input`
   block by the pipeline task. Apply changes only at block boundaries.
4. **DOA:** XVF's SRP-PHAT peak picker (`lib_doa`) feeds a 50 ms moving
   average into `doa_angle_deg`. Confidence is `peak / mean * 255` clamped.
5. **Validation:** Add `tests/audio_servicer/` with one test per command
   (write known value, read back, assert equal after one block period).

The Phase 3 effort is roughly 2–3 weeks for an engineer with XCore / XVF
experience, dominated by the beamformer integration and the tuning passes
against real-room recordings.
