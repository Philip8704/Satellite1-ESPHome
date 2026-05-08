# Satellite1 Firmware Config

This directory is the firmware YAML layer.

- `satellite1.yaml` is the local full build entrypoint.
- `satellite1.base.yaml` defines the shared Satellite1 base package and includes `common/`.
- `satellite1.dashboard.yaml` is the ESPHome Dashboard import entrypoint.
- `satellite1.ld2410.yaml` and `satellite1.ld2450.yaml` are sensor-specific variants.
- `common/` holds reusable ESPHome packages split by feature area.

Keep firmware package paths stable. The dashboard import, GitHub Actions builds, and ESPHome package includes all reference these locations directly.
