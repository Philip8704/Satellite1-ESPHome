# Custom ESPHome Components

This directory contains local ESPHome components compiled into the Satellite1 firmware.

- `components/mww_training_capture/` is the on-device wake-word near-miss capture component.
- `components/satellite1/` contains Satellite1 board integration code.
- Audio, speaker, mixer, wake-word, Snapcast, DAC, and sensor support live in the sibling component folders.

These are firmware-side components. Host-side capture review UI and Home Assistant add-on code live under `tools/mww_training_capture/`.
