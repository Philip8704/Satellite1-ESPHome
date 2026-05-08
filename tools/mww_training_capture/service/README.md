# Standalone Capture Service

This folder is the standalone FastAPI implementation for MWW Training Capture.

- `app.py` defines the API and review UI.
- `storage.py` manages capture metadata, labels, audio files, and exports.
- `__main__.py` runs the service with `python -m tools.mww_training_capture.service`.

Tests under `../tests/` import this package directly. When behavior changes here, mirror the same service files into `../addon/service/` so the Home Assistant add-on matches.
