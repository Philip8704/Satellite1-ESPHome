# Satellite1 ESPHome Firmware

This repository contains the Satellite1 ESPHome firmware tree, migrated to the
0.2.0-beta.0 layout and kept compatible with the customized `develop-belo`
configuration.

The root is reserved for firmware files. Non-firmware material belongs under
`miscellaneous/`, including the retained Home Assistant helper packages/blueprints
and the installable microWakeWord training capture add-on.

## Firmware Layout

| Path | Purpose |
| --- | --- |
| `.github/` | Firmware build and release workflows from the 0.2.0 baseline. |
| `assets/` | Firmware assets such as the project logo. |
| `config/` | ESPHome YAML entrypoints and shared package files. |
| `esphome/components/` | Local ESPHome components required by this firmware. |
| `scripts/` | Build environment helper scripts. |
| `miscellaneous/home_assistant/` | Retained Home Assistant helper packages, including Mova, alert TTS, and travel manager examples. |
| `miscellaneous/ha_blueprints/` | Retained Satellite1/Home Assistant voice and media blueprints. |
| `miscellaneous/tools/mww_training_capture/addon/` | Home Assistant add-on for reviewing and storing MWW capture uploads. |
| `requirements.txt` | Python dependency pins for local ESPHome builds. |

## Main Firmware Entrypoints

| File | Purpose |
| --- | --- |
| `config/satellite1.yaml` | Local build entrypoint. Uses local components from `esphome/components/`. |
| `config/satellite1.dashboard.yaml` | Dashboard import entrypoint pointed at `Philip8704/Satellite1-ESPHome@develop-belo`. |
| `config/satellite1.base.yaml` | Shared Satellite1 base package and dashboard import metadata. |
| `config/satellite1.ld2410.yaml` | LD2410 radar variant. |
| `config/satellite1.ld2450.yaml` | LD2450 radar variant. |

## Custom Behavior Preserved

This tree intentionally differs from vanilla 0.2.0-beta.0 in these areas:

| Area | Files | Behavior |
| --- | --- | --- |
| External Assist speaker routing | `config/common/media_player.yaml`, `config/common/voice_assistant.yaml`, `config/common/buttons.yaml`, `config/common/speaker.yaml` | Routes Assist prompts and sounds to a selected Home Assistant media player, keeps local line-out mirroring for HA announcement state, supports external duck/restore, and lets the center button stop external playback. |
| Announcement hold | `config/common/voice_assistant.yaml`, `esphome/components/speaker_source/` | Restores `set_announcement_finish_hold_ms(...)` so external prompt playback keeps the local announcement state padded for Sonos/Music Assistant latency. |
| Assist API stability | `esphome/components/api/`, `config/common/components.external.yaml`, `config/satellite1.yaml` | Vendors the ESPHome 2026.4.5 API component with a small configuration-response patch so Assist/wake-word metadata can be queried without crashing before the active Assist subscription is established. |
| MWW training capture | `config/common/voice_assistant.yaml`, `config/common/home_assistant.yaml`, `esphome/components/micro_wake_word/`, `esphome/components/mww_training_capture/` | Captures near-miss wake-word audio when enabled in Home Assistant. Capture watches only `hey_haeris` with `lower_cutoff: 0.60`. |
| SendSpin | `config/common/sendspin.yaml`, `esphome/components/{const,media_source,sendspin}/` | Keeps the 0.2.0 SendSpin media source/player path for internal/local speaker media. The needed SendSpin components are vendored locally so ESPHome does not need to clone `kahrendt/esphome` during config/compile. It is not used for external Sonos/Music Assistant rerouting. |
| Radar | `config/common/mmwave.yaml`, `esphome/components/satellite1_radar/` | Keeps the 0.2.0 radar package and local radar component. |

The `hey_haeris` model is expected on the Home Assistant ESPHome host at:

```text
/config/esphome/models/hey_haeris.json
```

External Assist output is opt-in by default so a missing or unset Home Assistant
media player cannot silence voice responses. Set these substitutions in the
dashboard YAML when Sonos/Music Assistant routing should start enabled:

```yaml
substitutions:
  external_assist_primary_media_player: media_player.office_2
  external_assist_speaker_restore_mode: RESTORE_DEFAULT_ON
```

If `External Assist Speaker` is on but the selected option is `None` or
`media_player.none`, firmware falls back to the internal/physical line-out path
instead of routing Assist audio to nowhere.

To offer more than the primary player in Home Assistant, extend the select in
the device YAML and keep the primary player in that options list:

```yaml
select:
  - id: !extend external_assist_media_player_select
    options:
      - media_player.office_2
      - media_player.living_room_2
```

## MWW Training Capture Add-on

The only retained host-side tool is the Home Assistant add-on:

```text
miscellaneous/tools/mww_training_capture/addon/
```

Install it as a local Home Assistant add-on by copying that folder to:

```text
/addons/mww_training_capture/
```

The add-on exposes:

| Endpoint | Purpose |
| --- | --- |
| `http://homeassistant.local:8765/upload` | Firmware upload target for WAV near-miss captures. |
| `http://homeassistant.local:8765/` | Review UI for captured clips. |

Captured files are stored under the add-on configured `/share` subpath. The
default firmware endpoint is:

```yaml
substitutions:
  mww_capture_endpoint: http://homeassistant.local:8765/upload
```

Enable or disable collection from Home Assistant with the Satellite entity named
`Wake word training capture`.

## Retained Home Assistant Extras

These are preserved as optional reference assets under `miscellaneous/`; they are
not part of the firmware build:

| Path | Purpose |
| --- | --- |
| `miscellaneous/home_assistant/mova_p50_ultra_scripts.yaml` | Mova P50 Ultra Assist/script package. |
| `miscellaneous/home_assistant/alert_tts_mirror_start_conversation_package.yaml` | Alert TTS / Assist start-conversation package. |
| `miscellaneous/home_assistant/travel_manager_ai_check_in_package.yaml` | Travel manager AI check-in package. |
| `miscellaneous/ha_blueprints/automation_sat1_duck_media_player.yaml` | Satellite1 media-player ducking automation blueprint. |
| `miscellaneous/ha_blueprints/llm_voice_script_sat1_sonos.yaml` | Important: Satellite1 Sonos/Music Assistant voice integration blueprint. Keep this retained. |
| `miscellaneous/ha_blueprints/llm_voice_script_sat1_sonos_standalone.yaml` | Important: standalone Satellite1 Sonos/Music Assistant voice integration script. Keep this retained. |

## Local Build

Create the Python environment:

```bash
source scripts/setup_build_env.sh
```

Validate or build the main firmware:

```bash
esphome config config/satellite1.yaml
esphome compile config/satellite1.yaml
```

Variant checks:

```bash
esphome config config/satellite1.ld2410.yaml
esphome config config/satellite1.ld2450.yaml
```

## Static Migration Notes

The root firmware layout is intended to match the supplied 0.2.0-beta.0 repo.
Files that do not contain custom behavior should remain identical to that
baseline. Custom components and YAML patches are kept only where needed for:

- external Assist media output and ducking;
- microWakeWord near-miss capture forwarding;
- local `speaker_source` announcement hold support;
- local vendored SendSpin components from the pinned 0.2.0 source;
- dashboard/import references to this fork.

Do not reintroduce the old Snapcast path. SendSpin is the current 0.2.0 internal
media path.

## License

Distributed under the ESPHome license. See `LICENSE`.
