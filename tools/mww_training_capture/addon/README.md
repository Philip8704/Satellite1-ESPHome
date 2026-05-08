# Home Assistant Add-on

This folder is the installable Home Assistant add-on for MWW Training Capture.

- `config.yaml` is the Home Assistant add-on manifest and version source.
- `Dockerfile` builds the add-on image inside Home Assistant Supervisor.
- `run.sh` reads add-on options and starts the service.
- `service/` is the FastAPI service bundled into the add-on image.

The add-on keeps a local copy of `service/` because Supervisor builds this folder as the Docker context. Keep it in sync with `../service/` when changing the capture API or storage behavior.

## Install

This add-on is intended for Home Assistant OS or Supervised installs with local add-ons enabled.

1. Copy this `addon/` folder to your Home Assistant add-ons directory.

   The final path should look like this:

   ```text
   /addons/mww_training_capture/
     config.yaml
     Dockerfile
     run.sh
     service/
   ```

2. In Home Assistant, go to **Settings > Add-ons > Add-on Store**.
3. Open the overflow menu and choose **Repositories** or **Check for updates**, depending on your HA UI.
4. Open **Local add-ons**.
5. Select **MWW Training Capture**.
6. Click **Install**.
7. Leave `data_subpath` as `mww_captures` unless you want a different folder under `/share/`.
8. Start the add-on.

The service listens on port `8765`. With the default firmware config, the Satellite posts captures to:

```text
http://homeassistant.local:8765/upload
```

The review UI is available at:

```text
http://homeassistant.local:8765/
```

Captured WAV and JSON sidecar files are written to:

```text
/share/mww_captures/
```

## Firmware Setup

The firmware side is configured in `config/common/voice_assistant.yaml`:

```yaml
mww_training_capture:
  upload_url: ${mww_capture_endpoint}
```

Override the endpoint if your Home Assistant host is not reachable as `homeassistant.local`:

```yaml
substitutions:
  mww_capture_endpoint: http://192.168.1.10:8765/upload
```

In Home Assistant, enable the Satellite entity named **Wake word training capture** when you want collection active.

## Update

1. Stop the add-on.
2. Replace the files in `/addons/mww_training_capture/` with the new version of this folder.
3. In **Settings > Add-ons > Add-on Store**, refresh local add-ons.
4. Rebuild or reinstall **MWW Training Capture** if Home Assistant does not detect the version bump automatically.
5. Start the add-on again.

The add-on version is defined in `config.yaml`.

## Troubleshooting

- If the add-on does not appear, confirm the folder contains `config.yaml` directly under `/addons/mww_training_capture/`.
- If the Satellite reports upload failures, confirm port `8765` is exposed and reachable from the device.
- If captures are not saved, check that the add-on has access to `/share` and that `data_subpath` is valid.
- If the UI opens but no captures appear, enable **Wake word training capture** on the Satellite and check the **Wake word captures dropped** diagnostic sensor.
