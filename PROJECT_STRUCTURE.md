# Project Structure

This repository has four main ownership areas:

## Satellite1 Firmware

- `config/` contains ESPHome YAML entrypoints and package files.
- `config/satellite1.yaml` is the local full firmware entrypoint.
- `config/satellite1.dashboard.yaml` is the ESPHome Dashboard import entrypoint.
- `config/common/` contains the firmware packages used by those entrypoints.

## Custom ESPHome Components

- `esphome/components/` contains local ESPHome components compiled into the firmware.
- `esphome/components/mww_training_capture/` is the on-device near-miss capture component.
- These components are referenced by `config/common/components.external.yaml` and the local package setup.

## MWW Capture Service

- `tools/mww_training_capture/service/` is the standalone FastAPI capture service.
- `tools/mww_training_capture/addon/` is the Home Assistant add-on wrapper and manifest.
- The add-on carries its own service copy because Home Assistant Supervisor builds add-ons from the add-on directory as the Docker context.
- `tools/mww_training_capture/tests/` tests the standalone service implementation.

## Developer Utilities

- `tools/training/` contains dataset/training helper scripts.
- `scripts/` contains repository build/setup helper scripts.
- `.github/workflows/` contains CI, firmware build, and release automation.

Generated caches such as `.esphome/`, `__pycache__/`, `.pytest_cache/`, `.venv/`, and `pytest-cache-files-*` are not source and should stay out of commits.
