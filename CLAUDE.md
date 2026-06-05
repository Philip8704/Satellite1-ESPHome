# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository purpose

ESPHome firmware for the FutureProofHomes **Satellite1 Core Board** (ESP32-S3 + XMOS audio co-processor + optional HAT board with mics, mmWave radar, environmental sensors, LEDs, and TAS2780 internal speaker / PCM5122 line-out). Pairs with Home Assistant as a voice assistant satellite. The firmware is a combination of:

1. YAML packages under `config/` that compose the device feature set.
2. Custom ESPHome C++/Python components under `esphome/components/` (loaded via `external_components`).
3. Companion Home Assistant artifacts under `miscellaneous/` (ha_blueprints, an `mww_training_capture` HA add-on).

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
- **`mww_capture.yaml`** — Near-miss training capture (see same section).
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
- **`mww_training_capture/`** — Records short WAV "near-miss" snippets when wake-word probability lands in a configurable band (above `lower_cutoff`, below the detection cutoff), streams them to a FastAPI add-on (`miscellaneous/tools/mww_training_capture/addon`) for retraining. Config schema is documented in [esphome/components/mww_training_capture/__init__.py:1](esphome/components/mww_training_capture/__init__.py). Exposes `mww_training_capture.enable` / `.disable` actions and an `is_enabled` condition.
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
  external_speaker_wake_sound_delay: "1500ms"                     # wake-chime landing window before listening starts
  external_speaker_restore_volume: "0.5"                          # volume to restore the player to after STT
  mww_capture_endpoint: "http://homeassistant.local:8765/upload"  # FastAPI add-on URL
  mww_capture_lower_cutoff: "0.7"                                  # default near-miss cutoff
```

### External speaker routing — `config/common/external_speaker.yaml`

Adds the **"External Speaker Routing"** template switch (restore-default-off) plus an `External Speaker Configured` template binary_sensor (true iff `external_assist_primary_media_player` is non-empty). When the switch is ON:

- `apply_external_speaker_routing` script pins the `dac_proxy` to **line-out** (PCM5122). With nothing plugged into the 3.5mm jack the analog signal goes nowhere, so the internal TAS2780 amp never sees audio. Toggling the switch off restores the DAC based on the `line_out_sensor` jack-detect (line-out if cable present, internal speaker if not). The script is also re-run after `tas2780_ensure_active` (USB-PD renegotiation) and `on_flashing_success`; `satellite1.base.yaml`'s `on_boot` syncs the `external_speaker_routing_on` global from the restored switch state *before* the jack-fallback lambda runs, so a device that booted with routing on doesn't briefly flip to internal speaker.
- `activate_internal_speaker` is **gated**: if routing is on it short-circuits and calls `apply_external_speaker_routing` instead. This matters for the jack-unplug `on_release` handler, which would otherwise flip the DAC back to TAS2780.
- `play_on_external(url)` script forwards an arbitrary URL to the configured media_player with `announce: true`. Used by `voice_assistant.yaml`'s `on_intent_progress` (streaming pipeline TTS) and `on_tts_end` (canonical fallback for non-streaming pipeline TTS *and* `assist_satellite.announce` / `start_conversation`, which never fire `on_intent_progress`). A `external_tts_mirrored_this_cycle` global guards against double-mirroring when both fire in the same cycle; it's reset in `on_start` / `on_end` / `on_error`. **Do not mirror via `on_tts_start`** — its `x` is the spoken text, not a URL (voice_assistant.cpp:730 + :926).
- `play_ui_sound_on_external(sound_file)` maps the audio_file IDs declared in `config/common/media_player.yaml` (e.g. `wake_word_triggered_sound`) to their filenames under `${external_sound_base}/` (e.g. `${external_sound_base}/wake_word_triggered.flac`). The mapping lives inline in the script's lambda — when you add a new local audio_file you must also extend that mapping or routed playback won't find it.
- `duck_external_for_listening` / `restore_external_after_listening` scripts call `media_player.volume_set` to 0.05 and back to the **observed pre-duck volume** (not a fixed value). The pre-duck volume is captured from a `homeassistant.sensor` subscribed to the external player's `volume_level` attribute and stashed in `external_player_pre_duck_volume`; restore writes that exact value back so a track that was mid-playback keeps playing at its original level — no `media_player.snapshot` or `scene.create` is used, which means the currently-playing song does **not** restart on most players. The substitution `external_speaker_restore_volume` is only a fallback for the very first listening cycle after boot (before HA has pushed a volume state) or if the entity is unreachable. The duck/restore are hooked into `on_listening` (duck) and `on_stt_vad_end` / `on_end` / `on_error` (restore).
- The `homeassistant.sensor` entity_id is read from `${external_assist_primary_media_player}` at compile time; the substitution's default `media_player.satellite1_unconfigured` is a parseable placeholder so the sensor still validates when the user hasn't configured a real target. `external_speaker_configured` compares against this magic value to gate all routing actions.
- Wake-sound timing is **conditional**: when routing is on `voice_assistant.yaml`'s `on_wake_word_detected` waits `${external_speaker_wake_sound_delay}` (default 1500ms) instead of the local-pipeline 300ms before calling `voice_assistant.start`, so the wake chime actually lands on the MA speaker before the satellite mic opens.

The local `speaker_source` `external_media_player` (yes, naming collision with the user-facing concept of "external") is **intentionally kept live** even when routing is on, because the voice assistant's phase state machine (`on_intent_progress`, `on_tts_end`, `on_listening`) depends on the local pipeline tracking TTS playback. With the DAC pinned to line-out, the local pipeline produces no audible output. Known limitation: AEC reference is the local stream so the XMOS echo-cancellation can't subtract what the external MA speaker is actually emitting — the duck-during-listen mitigation reduces but doesn't eliminate TTS bleed.

### MWW training capture — `config/common/mww_capture.yaml`

Configures the `mww_training_capture` custom component (vendored at `esphome/components/mww_training_capture/`) with `default_lower_cutoff: ${mww_capture_lower_cutoff}`, `pre_buffer_seconds: 2.0`, `post_buffer_ms: 500`, and `enabled_by_default: false`. The "Wake Word Training Capture" template switch (restore-default-off) calls the component's `enable` / `disable` actions; diagnostic sensors expose `get_capture_count()`, `get_dropped_count()`, and `get_last_wake_word()`.

Key behavior — with an empty explicit `models:` list, the component **auto-registers every non-internal model exposed by `micro_wake_word`** at setup (see `mww_training_capture.cpp:74-82`). This is why the framework can ship without naming `hey_haeris` (or any other user-defined wake word) — the user's YAML adds the model under `micro_wake_word:` and capture picks it up automatically with the default 0.7 cutoff.

The companion FastAPI add-on lives under `miscellaneous/tools/mww_training_capture/addon/`.

### External components — both lists must match

Adding a new directory under `esphome/components/` requires registering it in **both** `config/satellite1.yaml`'s local-path entry and `config/common/components.external.yaml`'s git entry — the local one is for CI/`esphome compile`, the git one is for users who pull the dashboard import package. `mww_training_capture` is the most recent example of why both must be kept in sync.

## Conventions when editing

- New custom components belong under `esphome/components/<name>/` with the standard `__init__.py` + `*.h`/`*.cpp` layout (see existing components for examples). Then add the component name to **both** `config/satellite1.yaml`'s local `external_components:` entry **and** `config/common/components.external.yaml`'s git entry, otherwise dashboard-import users won't get it.
- YAML files under `config/` must pass `yamllint`. `esphome/`-tracked C/C++ must pass clang-format 18 against `.clang-format`.
- Sound files referenced from `media_player.yaml` are URLs by default. You can override with substitutions in user YAML, point at local files (e.g. `/config/esphome/sounds/...`), or at a self-hosted HTTP base — the audio_file IDs (`wake_word_triggered_sound`, etc.) are the stable handle.
- The user-overridable surface area is mostly: `substitutions:` (filenames, model versions, friendly_name, log_level), the `micro_wake_word:` block (add/`!remove` models), `select: - id: !remove …` (remove unwanted dropdowns), and adding extra packages. The base config is structured to allow append-only customization for most features.
