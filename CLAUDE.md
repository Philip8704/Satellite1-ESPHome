# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository purpose

ESPHome firmware for the FutureProofHomes **Satellite1 Core Board** (ESP32-S3 + XMOS audio co-processor + optional HAT board with mics, mmWave radar, environmental sensors, LEDs, and TAS2780 internal speaker / PCM5122 line-out). Pairs with Home Assistant as a voice assistant satellite. The firmware is a combination of:

1. YAML packages under `config/` that compose the device feature set.
2. Custom ESPHome C++/Python components under `esphome/components/` (loaded via `external_components`).
3. Companion Home Assistant artifacts under `miscellaneous/` (ha_blueprints, HA packages and scripts).

## Build / lint / flash commands

Bootstrap a build env (creates `.venv`, installs pinned ESPHome from `requirements.txt`):
```bash
source scripts/setup_build_env.sh   # bash; on Windows use a WSL/git-bash shell or replicate the venv steps manually
```

The pinned ESPHome version in `requirements.txt` is authoritative — CI derives the build version from it, and Dashboard Builder users must keep their ESPHome version compatible with the branch they target (see `Customizing the Firmware` in [README.md](README.md)).

Standard ESPHome cycle against the dev entrypoint `config/satellite1.yaml` (which `!include`s `satellite1.base.yaml`):
```bash
esphome compile config/satellite1.yaml
esphome upload  config/satellite1.yaml
esphome logs    config/satellite1.yaml
```

`satellite1.ld2410.yaml` and `satellite1.ld2450.yaml` are the alternate top-level configs for boards with the respective mmWave radar; CI builds all three (`config/satellite1.yaml`, `config/satellite1.ld2410.yaml`, `config/satellite1.ld2450.yaml`).

Lint (same checks CI runs in `.github/workflows/lint.yaml`):
```bash
yamllint config/                                         # YAML
clang-format --dry-run --Werror --style=file esphome/**  # C/C++ (clang-format 18)
pre-commit run --all-files                               # both, via hooks defined in .pre-commit-config.yaml
```

CI workflow notes: `build_latest` runs on push to `develop`/`staging`/`main` and on PRs to `staging`/`main`. `build_release` is tag-driven (`vX.Y.Z`, `vX.Y.Z-beta.N`). See [docs/pr-review-merge-and-release-workflow.md](docs/pr-review-merge-and-release-workflow.md) for branch / labeling / promotion rules — release notes are generated from original-PR labels via tag commit ranges, so label PRs into `develop` with one of `feature` / `improvement` / `fix` / `breaking` / `internal` (or `skip-changelog`).

## Two YAML entrypoint styles — important when changing structure

The firmware has **two different entrypoint patterns** and changes that affect device behavior often need to be made consistent in both:

1. **Local / CI build** — `config/satellite1.yaml` uses `!include satellite1.base.yaml` and points `external_components` at the **local** `../esphome/components` path. This is what `esphome compile` builds and what CI builds.
2. **Dashboard import** — `config/satellite1.dashboard.yaml` loads `satellite1.base.yaml` and `config/common/components.external.yaml` over HTTPS as a `packages:` entry. End users' YAML follows this same pattern, pulling the package from the `staging` branch by default. `components.external.yaml` fetches the same custom components from GitHub at a configurable `ext_comp_repo_ref`.

`config/satellite1.base.yaml` is the shared core: it sets up `esphome:`, `safe_mode`, `status_led`, the `satellite1:` component (XMOS comms over SPI), the `memory_flasher` (XMOS firmware OTA, embedded image URL pinned to `${xmos_fw_version}`), and pulls in all `config/common/*.yaml` feature packages.

## Common-package layer (`config/common/`)

Each file is a self-contained feature slice with its own globals/scripts/components. Reading these is the fastest way to understand how a feature works end-to-end:

- **`core_board.yaml`** — ESP32-S3 board + sdkconfig, PSRAM (octal/80MHz), I2C bus, SPI bus, `i2s_audio` (shared, secondary mode), UART for mmWave.
- **`speaker.yaml`** — Audio output stack: `pcm5122` (line-out DAC), `tas2780` (internal speaker amp), `satellite1` `dac_proxy` (multiplexes between them), `i2s_audio_speaker`, **mixer speaker** with two source inputs (`announcement_mixing_input`, `media_mixing_input`) and **resampler speakers** in front of each to normalize to 48 kHz / 16-bit. Also defines the line-out jack-detect binary sensor and the `activate_internal_speaker` / `activate_line_out` scripts. Exposes `activate_speaker_dac` / `activate_lineout_dac` HA actions.
- **`media_player.yaml`** — `audio_file:` IDs for local UI sounds (hosted at `fph-firmware-assets.s3...`), three `media_source` instances (`audio_file_announcement_source`, `http_announcement_source`, `http_media_source`), and the `speaker_source` `external_media_player` with separate `announcement_pipeline` (mono FLAC) and `media_pipeline` (stereo FLAC) — both 48 kHz. Defines the `play_sound` script (used everywhere a UI tone is played) and `control_volume`. Ducking on/off is driven by `on_announcement` / `on_state` against `media_mixing_input`.
- **`voice_assistant.yaml`** — `microphone:` (satellite1 platform reading from XMOS over I2S), `micro_wake_word:` (default models `hey_jarvis`, `okay_nabu`, `stop`), and the `voice_assistant:` block wired to `external_media_player`. Encodes the **voice assistant phase state machine** as the `voice_assistant_phase` global (`idle=1`, `waiting_for_command=2`, `listening_for_command=3`, `thinking=4`, `replying=5`, `not_ready=10`, `error=11`) — every state transition fires `script.execute: control_leds`. Defines the wake-sound + ducking interactions, the `stop` wake word activation logic during TTS, and the sensitivity `select` (`mww_sensitivity_select`).
- **`home_assistant.yaml`** — `api:` (HA-managed encryption), restart / factory-reset buttons, `wake_sound` switch, `master_mute_switch` (software mute that respects the hardware mute pin), and `pd_state_text` / `xmos_firmware_version_text` diagnostics.
- **`led_ring.yaml`** — LED ring + the `control_leds` script that the rest of the codebase calls whenever any user-visible state changes. Most other packages do `script.execute: control_leds` as their LED hook.
- **`buttons.yaml`** / **`timer.yaml`** / **`sendspin.yaml`** / **`mmwave*.yaml`** / **`hat_sensors.yaml`** — feature slices following the same convention.
- **`wifi_improv.yaml`** — Default Wi-Fi + improv-over-BLE provisioning. The top-level `satellite1.yaml` adds `on_client_connected` waits for BLE + memory flasher to finish before the voice assistant connects.
- **`external_speaker.yaml`** — External-speaker routing framework (see "External speaker routing & MWW capture" below).
- **`mww_capture.yaml`** — Wake-word training capture: uploads both detections and near-misses to the microWakeWord trainer (see same section).
- **`xvf_control.yaml`** — XMOS XVF DSP runtime control plane (beamforming, AEC/AGC/NS tuning, DOA, mic gain). **Opt-in, not loaded by base.yaml** — requires the Phase 3 custom XMOS firmware (see "XVF DSP control plane" section below).
- **`debug.yaml`** / **`developer.yaml`** — opt-in extras (memory/wifi/xmos debug, dev-only switches).

When you add a feature, decide whether it belongs as a new `common/*.yaml` package (then include it from `satellite1.base.yaml`) or as a substitution-overridable block — user YAML can append to lists from the base package, override substitutions, or `!remove` individual entities (see how `mww_sensitivity_select` is removed in the user-YAML example at the top of the goal in this conversation).

## Custom ESPHome components (`esphome/components/`)

These are loaded via `external_components:` in `satellite1.yaml` (local path for CI) and `components.external.yaml` (git URL for dashboard users). Both entrypoints **must list the same components**, plus one pinned upstream component (`http_request` from an esphome PR commit).

Active components:

- **`satellite1/`** — Top-level component (`spi`-dependent) that owns the XMOS SPI link (`xmos_rst_pin`, MODE3 @ 8 MHz). Subcomponents:
  - `satellite1/audio_dac/` — `dac_proxy` virtual DAC that switches between PCM5122 (line-out) and TAS2780 (speaker).
  - `satellite1/microphone/` — `satellite1` microphone platform that reads I2S audio from XMOS.
  - `satellite1/memory_flasher/` — XMOS firmware flasher (drives `xmos_flashing_state` global + LED progress).
  - `satellite1/light/`, `satellite1/runtime_testing/`, `sat_gpio.*` — LED ring, dev-only runtime tests, GPIO helpers.
  - Triggers: `on_xmos_no_response`, `on_xmos_connected` — used to auto-reflash a mismatched XMOS image at boot (logic lives in `satellite1.yaml`, not the base).
- **`satellite1_radar/`** — Driver for the LD2410 / LD2450 HAT-attached mmWave radars.
- **`memory_flasher/`** — Generic flasher framework; `platform: satellite1` is the XMOS implementation.
- **`pcm5122/`**, **`tas2780/`** — DAC / amp drivers used by the `audio_dac:` block.
- **`i2s_audio/`** — Forked/vendored I2S audio (note `i2s_mode: secondary` in `core_board.yaml` — XMOS is the I2S master).
- **`fusb302b/`** — USB-C PD negotiation. Drives `pd_power_contract` global; `tas2780.activate` mode is gated on whether the contract is ≥ 9 V.
- **`speaker_source/`** — Custom `media_player` platform exposing `announcement_pipeline` + `media_pipeline` over speakers (driven by `media_player.yaml`).
- **`media_source/`** — Vendored/extended media sources used by `media_player.yaml`.
- **`micro_wake_word/`** — Vendored microWakeWord with `WakeWordModel` exposing the cutoff/probability hooks used by `mww_training_capture`.
- **`mww_training_capture/`** — Records short WAV clips around wake-word activity and streams them to the [TaterTotterson microWakeWord trainer](https://github.com/TaterTotterson/microWakeWord) (Docker, `POST /api/upload_captured_audio_raw` on port 8789) for retraining. Captures **both** event types: `wake_detected` (the model fired — candidate positive samples) and `close_miss` (probability crossed `lower_cutoff` but never reached the detection cutoff — candidate negatives); the event is reported in the `X-Event-Type` header. Config schema is documented in [esphome/components/mww_training_capture/__init__.py:1](esphome/components/mww_training_capture/__init__.py). Exposes `mww_training_capture.enable` / `.disable` actions and an `is_enabled` condition.
- **`api/`**, **`const/`**, **`sendspin/`** — supporting overrides / helpers.

C++ code is formatted with **clang-format 18 against the `.clang-format` at repo root** (Werror in CI).

## State-machine globals you'll see across packages

These globals are declared in `satellite1.base.yaml` or feature packages and read/written from many places — search for them before changing their meaning:

- `init_in_progress` (`speaker.yaml`, `voice_assistant.yaml`, …) — true at boot until HA API connects (10-min timeout fallback in `on_boot`).
- `voice_assistant_phase` — see phase IDs in `voice_assistant.yaml`. `control_leds` reads this to pick the LED animation.
- `xmos_flashing_state` (`0=idle`, `1=flashing`, `2=success`, `3=fail`) — set by the `memory_flasher` triggers.
- `pd_power_contract` — voltage from `fusb302b` PD contract; gates TAS2780 power mode.
- `warning`, `tas2780_active`, `jack_plugged_recently` / `jack_unplugged_recently`, `timer_ringing` (switch), `factory_reset_requested`.

Standard pattern: any time one of these changes, the change handler runs `script.execute: control_leds`.

## Audio pipeline cheat sheet

```
HA TTS / media URL ──► http_media_source / http_announcement_source ──┐
local UI sound (FLAC/MP3) ──► audio_file_announcement_source ─────────┤
                                                                       ▼
                                              speaker_source::external_media_player
                                                  ├─ announcement_pipeline ──► announcement_resampling_speaker ──► announcement_mixing_input ─┐
                                                  └─ media_pipeline       ──► media_resampling_speaker       ──► media_mixing_input          ─┤
                                                                                                                                                ▼
                                                                                                                            mixer_speaker (48k/16-bit)
                                                                                                                                                │
                                                                                                                                                ▼
                                                                                                                            i2s_audio_speaker ──► dac_proxy
                                                                                                                                                        ├─ tas2780 (internal)
                                                                                                                                                        └─ pcm5122 (line-out)
```

Ducking is applied to `media_mixing_input` (announcements and voice assistant don't get ducked; music does). Wake / button / timer tones go through `audio_file_announcement_source` via the `play_sound` script with `priority: true|false`.

## External speaker routing & MWW capture (framework substitutions)

`config/satellite1.base.yaml` exposes substitutions that let user YAML opt into two features without forking. All default to empty/safe values, so a flash with no overrides behaves identically to upstream.

```yaml
substitutions:
  external_assist_primary_media_player: "media_player.kitchen"   # HA entity to route to
  external_sound_base: "http://homeassistant.local:8123/local/sounds"  # serves UI tones for the routed player
  external_speaker_restore_volume: "0.5"                          # volume to restore the player to after STT
  mww_capture_endpoint: "http://192.168.1.227:8789/api/upload_captured_audio_raw"  # microWakeWord trainer URL
  mww_capture_lower_cutoff: "0.7"                                  # default near-miss cutoff
```

### External speaker routing — `config/common/external_speaker.yaml`

Adds the **"External Speaker Routing"** template switch (restore-default-off) plus an `External Speaker Configured` template binary_sensor (true iff `external_assist_primary_media_player` is non-empty). When the switch is ON:

- `apply_external_speaker_routing` script pins the `dac_proxy` to **line-out** (PCM5122). With nothing plugged into the 3.5mm jack the analog signal goes nowhere, so the internal TAS2780 amp never sees audio. Toggling the switch off restores the DAC based on the `line_out_sensor` jack-detect (line-out if cable present, internal speaker if not). The script is also re-run after `tas2780_ensure_active` (USB-PD renegotiation) and `on_flashing_success`; `satellite1.base.yaml`'s `on_boot` syncs the `external_speaker_routing_on` global from the restored switch state *before* the jack-fallback lambda runs, so a device that booted with routing on doesn't briefly flip to internal speaker.
- `activate_internal_speaker` is **gated**: if routing is on it short-circuits and calls `apply_external_speaker_routing` instead. This matters for the jack-unplug `on_release` handler, which would otherwise flip the DAC back to TAS2780.
- `play_on_external(url)` script forwards an arbitrary URL to the configured media_player with `announce: true`. Used by `voice_assistant.yaml`'s `on_intent_progress` (streaming pipeline TTS) and `on_tts_end` (canonical fallback for non-streaming pipeline TTS *and* `assist_satellite.announce` / `start_conversation`, which never fire `on_intent_progress`). A `external_tts_mirrored_this_cycle` global guards against double-mirroring when both fire in the same cycle; it's reset in `on_start` / `on_end` / `on_error`. **Do not mirror via `on_tts_start`** — its `x` is the spoken text, not a URL (voice_assistant.cpp:730 + :926).
- `play_ui_sound_on_external(sound_file)` maps the audio_file IDs declared in `config/common/media_player.yaml` (e.g. `wake_word_triggered_sound`) to their filenames under `${external_sound_base}/` (e.g. `${external_sound_base}/wake_word_triggered.flac`). The mapping lives inline in the script's lambda — when you add a new local audio_file you must also extend that mapping or routed playback won't find it.
- `capture_volume_for_cycle` snapshots the external player's exact pre-announcement volume into the volatile `external_player_conversation_volume`. It runs before normal TTS and is also the first `on_tts_start` action for `assist_satellite.announce` / `start_conversation`; its valid flag keeps the first value across follow-up turns so MA's temporary announce volume can never replace it. A new turn cancels the previous delayed restore, while the final `restore_external_after_cycle` waits for the external `playing -> non-playing` transition (or a conservative start timeout), restores the conversation snapshot, repeats guarded late corrections, then clears it. The persisted `user_target_volume` remains only a Duck-mode fallback/music baseline and is never used by the late announcement corrector when no fresh conversation snapshot exists.
- The `homeassistant.sensor` entity_id is read from `${external_assist_primary_media_player}` at compile time; the substitution's default `media_player.satellite1_unconfigured` is a parseable placeholder so the sensor still validates when the user hasn't configured a real target. `external_speaker_configured` compares against this magic value to gate all routing actions.
- The wake chime plays at **`on_listening`, not `on_wake_word_detected`** — i.e. only after HA has granted this satellite the pipeline. In a multi-satellite home both devices detect the wake word, but HA rejects the race loser's `voice_assistant.start` with `duplicate_wake_up_detected` (already excluded from the error LED in `on_error`), so the loser never reaches `on_listening` and stays silent. `voice_assistant.start` is called immediately on detection with no pre-chime delay. The mic is already streaming when the chime plays; HA's pipeline VAD ignores it as non-speech, and XMOS AEC cancels the local playback.

The local `speaker_source` `external_media_player` (yes, naming collision with the user-facing concept of "external") is **intentionally kept live** even when routing is on, because the voice assistant's phase state machine (`on_intent_progress`, `on_tts_end`, `on_listening`) depends on the local pipeline tracking TTS playback. With the DAC pinned to line-out, the local pipeline produces no audible output. Known limitation: AEC reference is the local stream so the XMOS echo-cancellation can't subtract what the external MA speaker is actually emitting — the duck-during-listen mitigation reduces but doesn't eliminate TTS bleed.

### MWW training capture — `config/common/mww_capture.yaml`

Configures the `mww_training_capture` custom component (vendored at `esphome/components/mww_training_capture/`) with `default_lower_cutoff: ${mww_capture_lower_cutoff}`, `pre_buffer_seconds: 2.0`, `post_buffer_ms: 500`, and `enabled_by_default: false`. The "Wake Word Training Capture" template switch (restore-default-off) calls the component's `enable` / `disable` actions; diagnostic sensors expose `get_capture_count()`, `get_detection_capture_count()`, `get_close_miss_capture_count()`, `get_dropped_count()`, `get_last_wake_word()`, and `get_last_event_type()`.

Captures **both** classes a trainer needs, each gated by its own substitution and both defaulting on: `wake_detected` (`${mww_capture_wake_detected}`) for candidate positives, and `close_miss` (`${mww_capture_close_miss}`) for candidate negatives — probability crossed `${mww_capture_lower_cutoff}` but never reached the model's real cutoff. Detections get a shorter cooldown and a short 250 ms post-roll, because `voice_assistant` stops `micro_wake_word` as soon as the pipeline starts and the audio tap goes silent.

Uploads are 16-bit/16 kHz mono WAV POSTed with the header set the [TaterTotterson microWakeWord trainer](https://github.com/TaterTotterson/microWakeWord) expects (`X-Event-Type`, `X-Source-Device`, `X-Wake-Word`, `X-Audio-Format`, `X-Notes`, …) — that trainer is the only supported receiver. Two gates must both be true for an upload: the switch is ON **and** `${mww_capture_endpoint}` is non-empty. The switch's `on_turn_on` refuses to enable (and bounces itself off with an ERROR log) when the endpoint substitution was never overridden.

Key behavior — with an empty explicit `models:` list, the component **auto-registers every non-internal model exposed by `micro_wake_word`** at setup (see `mww_training_capture.cpp:74-82`). This is why the framework can ship without naming `hey_haeris` (or any other user-defined wake word) — the user's YAML adds the model under `micro_wake_word:` and capture picks it up automatically with the default 0.7 cutoff.

## XVF DSP control plane (`xvf_control` — Phase 2 scaffolding for Phase 3 XMOS work)

[esphome/components/xvf_control/](esphome/components/xvf_control/) plus the opt-in [config/common/xvf_control.yaml](config/common/xvf_control.yaml) package implement the ESP32 side of a beamforming / AEC / AGC / NS runtime control plane. The wire protocol is defined in [docs/xmos_dsp_control_protocol.md](docs/xmos_dsp_control_protocol.md) — that document is the canonical contract.

**Three-phase split** — important for understanding what works against which firmware:

- **Phase 1 — substitution-driven tuning knobs (works with stock FPH XMOS).** `mic_gain_factor`, `va_noise_suppression_level`, `va_auto_gain`, `va_volume_multiplier` in [satellite1.base.yaml](config/satellite1.base.yaml). Pure ESPHome side; no XMOS dependency. Always loaded.
- **Phase 2 — `xvf_control` component + HA entities (opt-in, requires Phase 3 firmware).** Adds the C++ `XvfControl` class talking to the audio servicer at RESID `0xE0` and a YAML package exposing the entities (number/select/sensor/switch). The component **probes capabilities at boot** by reading `CMD_CAPABILITY_FLAGS` + `CMD_FW_FEATURE_VERSION`; if the XMOS returns zeros (stock firmware doesn't implement the servicer), `is_servicer_present()` stays false and every setter is a guarded no-op. **Do not include `xvf_control.yaml` in a flash YAML unless the XMOS firmware implements the servicer** — the entities work but don't actually do anything, which is confusing UX.
- **Phase 3 — custom XMOS firmware (not in this repo).** Fork [github.com/FutureProofHomes/Satellite1-XMOS](https://github.com/FutureProofHomes/Satellite1-XMOS), add a new `src/audio/audio_servicer.{c,h}` registered at RESID `0xE0`, add a beamforming stage to a forked pipeline variant (`audio_pipelines/reference/beamforming/`). Spec is in `docs/xmos_dsp_control_protocol.md`. Build with XMOS XTC Tools 15.x, host the `.factory.bin`+`.md5`, override `xmos_fw_version` / `xmos_fw_image_url` / `xmos_fw_md5_url` substitutions in the flash YAML, reflash.

**SPI command map** ([esphome/components/satellite1/satellite1.h](esphome/components/satellite1/satellite1.h)):

```
AUDIO_SERVICER_RESID = 0xE0          // new in Phase 3 firmware
audio_cmd::CAPABILITY_FLAGS    = 110 // R/O u32 bitmask
audio_cmd::FW_FEATURE_VERSION  = 111 // R/O 4 bytes maj.min.pat.0
audio_cmd::BEAM_MODE           = 112 // R/W 1 byte (0=fixed,1=adaptive,2=tracking)
audio_cmd::BEAM_ANGLE          = 113 // R/W i16 deg -180..+180
audio_cmd::DOA_ANGLE           = 114 // R/O i16 deg
audio_cmd::DOA_CONFIDENCE      = 115 // R/O u8 0..255
audio_cmd::AEC_MODE            = 116 // R/W 1 byte (0=bypass,1=linear,2=lin+residual)
audio_cmd::AEC_REF_GAIN_DB     = 117 // R/W i8 dB -24..+12
audio_cmd::AGC_ENABLE          = 118 // R/W bool
audio_cmd::AGC_TARGET_DBFS     = 119 // R/W i8 dBFS -60..0
audio_cmd::NS_ENABLE           = 120 // R/W bool
audio_cmd::NS_LEVEL            = 121 // R/W u8 0..3
audio_cmd::MIC_GAIN_L          = 122 // R/W i8 dB -12..+24
audio_cmd::MIC_GAIN_R          = 123 // R/W i8 dB -12..+24
audio_cmd::PIPELINE_STATS      = 124 // R/O 16-byte packed struct
```

**Capability bits** in `audio_capability::` — `BEAM_FIXED`, `BEAM_ADAPTIVE`, `BEAM_TRACKING`, `DOA_REPORTING`, `AEC_TUNING`, `AGC_TUNING`, `NS_TUNING`, `MIC_GAIN_RUNTIME`, `PIPELINE_STATS_AVAILABLE`. Each setter in `XvfControl` gates on the matching bit, so a Phase 3 firmware can ship with partial features (e.g. beam control without DOA reporting) and the ESPHome side cleanly hides the inapplicable entities.

When extending the protocol: append new command IDs (don't renumber), bump `FW_FEATURE_VERSION`, add a new capability flag bit, and bump version in `xmos_dsp_control_protocol.md`. Backward-compat is straightforward because the capability probe drives all entity visibility.

### External components — both lists must match

Adding a new directory under `esphome/components/` requires registering it in **both** `config/satellite1.yaml`'s local-path entry and `config/common/components.external.yaml`'s git entry — the local one is for CI/`esphome compile`, the git one is for users who pull the dashboard import package. `mww_training_capture` is the most recent example of why both must be kept in sync.

## Conventions when editing

- New custom components belong under `esphome/components/<name>/` with the standard `__init__.py` + `*.h`/`*.cpp` layout (see existing components for examples). Then add the component name to **both** `config/satellite1.yaml`'s local `external_components:` entry **and** `config/common/components.external.yaml`'s git entry, otherwise dashboard-import users won't get it.
- YAML files under `config/` must pass `yamllint`. `esphome/`-tracked C/C++ must pass clang-format 18 against `.clang-format`.
- Sound files referenced from `media_player.yaml` are URLs by default. You can override with substitutions in user YAML, point at local files (e.g. `/config/esphome/sounds/...`), or at a self-hosted HTTP base — the audio_file IDs (`wake_word_triggered_sound`, etc.) are the stable handle.
- The user-overridable surface area is mostly: `substitutions:` (filenames, model versions, friendly_name, log_level), the `micro_wake_word:` block (add/`!remove` models), `select: - id: !remove …` (remove unwanted dropdowns), and adding extra packages. The base config is structured to allow append-only customization for most features.
