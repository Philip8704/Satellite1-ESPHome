# microWakeWord Training Capture

A self-contained pipeline for harvesting **false-negative** wake-word audio
from a FutureProofHomes Satellite 1 (Sat1) and shipping it back into the
microWakeWord training notebooks.

A "false negative" is the case that hurts an on-device wake word the most:
the user *did* say the wake word, but the model's sliding-window probability
peaked just below the cutoff and the assistant never woke up. By capturing
the audio that came close to firing — but didn't — and letting you label
each clip, you get a steady stream of high-signal training samples that are
specific to your room, your voice, and your microphone.

## How it works

```
+--------------------------+        HTTP POST /upload          +-------------------------+
|  Sat1 ESPHome firmware   |  ─────────────────────────────▶   |   Companion service     |
|                          |     audio/wav + metadata          |   (FastAPI in this dir) |
|  micro_wake_word         |                                   |                         |
|  +-> mww_training_capture|                                   |   /data/mww_captures/   |
|      (near-miss watcher) |                                   |      sat1/okay_nabu/    |
+--------------------------+                                   |        20260428.wav    |
                                                               |        20260428.json   |
                                                               |   …                     |
                                                               |   GET / (review UI)    |
                                                               |   GET /export?label=…  |
                                                               +-------------------------+
```

The new `mww_training_capture` ESPHome component piggybacks on the existing
`micro_wake_word` instance. While MWW runs its inference task, our component:

1. Maintains a 3-second rolling PCM ring buffer fed by the same microphone.
2. Each main-loop tick polls every wake-word model for its latest sliding-
   window max probability (`get_last_max_probability`, atomic so it can't
   tear under the inference task's writes).
3. When `(VAD active) AND (max ≥ lower_cutoff) AND (max < real cutoff)`, it
   marks a pending capture, waits for `post_buffer_ms` more samples to
   arrive, then assembles a 16-bit / 16 kHz / mono WAV.
4. Fires the `on_near_miss_detected` automation with the wake-word name and
   probabilities; the YAML `http_request.post` ships the WAV body to the
   companion service.

The companion service stores everything as plain files on disk
(`<device>/<wake_word>/<utc>_<id>.wav` plus a JSON sidecar with the
probabilities and your eventual label) and exposes:

- `POST /upload` — what the Sat1 calls into.
- `GET /` — a tiny HTML review UI: listen to each clip, label it FN / TN /
  discard, type freeform notes.
- `GET /captures` and `GET /captures/{id}/wav` — programmatic access.
- `POST /captures/{id}/label` — labelling endpoint.
- `GET /export?label=false_negative` — a ZIP with the
  `<label>/<wake_word>/<device>/<id>.wav` directory layout that the
  [microWakeWord training notebook][nb] consumes.

[nb]: https://github.com/kahrendt/microWakeWord/blob/main/notebooks/training_notebook.ipynb

## Running it

### Quick test on any host

```bash
cd tools/mww_training_capture
pip install -r requirements.txt
MWW_CAPTURE_DATA_DIR=/tmp/mww_captures python -m service
# → http://0.0.0.0:8765
```

### Docker

```bash
cd tools/mww_training_capture
docker build -t mww-capture .
docker run --rm -p 8765:8765 -v $PWD/_data:/data mww-capture
```

### Home Assistant add-on

Copy the `addon/` folder into your HA Supervised install's `/addons/`
directory, name it `mww_training_capture/`, then refresh the Add-on Store
and install. The add-on writes to `/share/mww_captures/` so the dataset is
visible from the file editor / Samba share.

## Wiring up the firmware

The firmware changes are already merged into
`config/common/voice_assistant.yaml` and `config/common/home_assistant.yaml`:

- A new top-level `mww_training_capture:` block hooks our component into the
  existing `micro_wake_word` instance and configures per-model lower cutoffs.
- The capture endpoint URL is the substitution
  `${mww_capture_endpoint}` (default `http://homeassistant.local:8765/upload`).
  Override it from your top-level YAML, e.g.:

  ```yaml
  substitutions:
    mww_capture_endpoint: http://192.168.1.10:8765/upload
  ```

- A new HA-controlled switch — *Wake word training capture* — toggles the
  feature at runtime so you can leave it off until you actively want a
  collection session.
- A diagnostic counter — *Wake word captures since boot* — lets you sanity
  check at a glance that captures are flowing.

After flashing, flip the switch on, talk to the satellite for a while
(intentionally including marginal pronunciations, distance, background
noise, etc.), then open the companion service UI and start labelling.

## Tuning

In `config/common/voice_assistant.yaml`, under `mww_training_capture:`:

| Setting             | What it does                                                       |
| ------------------- | ------------------------------------------------------------------ |
| `default_lower_cutoff` | Floor of the near-miss band. Anything below this is silence-ish noise we don't care about. |
| `pre_buffer_seconds`   | How much audio leading up to the near-miss to keep. 2 s is enough to capture a typical "okay nabu". |
| `post_buffer_ms`       | Tail-out audio after the near-miss; helps when the user's mouth was still moving. |
| `cooldown_ms`          | Min gap between captures *per model* so a long sentence doesn't blow up your dataset. |
| `require_vad`          | Only capture when MWW's VAD says someone's talking. Strongly recommended — leaves background music alone. |
| `models[].lower_cutoff`| Per-model override. `hey_jarvis` sits high so its band starts higher. |

The on-device probability cutoff (the *real* one that fires the assistant)
is still controlled by the existing `Wake word sensitivity` selector — we
deliberately *do not* touch that, so toggling the training switch never
changes how often the assistant actually wakes.

## Putting captures into training

The export ZIP is laid out as the microWakeWord training notebook expects:

```
false_negative/
  okay_nabu/
    sat1-living-room/
      20260428T123456_789012_sat1-living-room_okay_nabu.wav
      20260428T123456_789012_sat1-living-room_okay_nabu.json
    …
  hey_jarvis/
    …
```

Drop those WAVs into the notebook's positive samples folder for the model
you're retraining, mark them as positive examples, and re-run the training
loop. The `.json` sidecars carry the original probability + device metadata
so you can later weight or filter them however you like.

## Troubleshooting

- **The switch is on, but the counter never moves.** Either nobody's
  speaking near-miss audio, or the upload is failing. Check the ESPHome log
  for `MWW capture upload failed` warnings, and the companion service for
  `400` / connection errors.
- **Service rejects with `400 payload is not a RIFF/WAV file`.** The WAV
  builder in the firmware is busted — re-flash and check the log for the
  `Captured X.XX s near-miss for '…'` message that should accompany every
  upload.
- **Lots of captures from background noise.** Bump
  `default_lower_cutoff` (or the per-model `lower_cutoff`) up by 0.05 — the
  band you're watching is too wide.
- **Nothing fires for hey_jarvis.** Its real cutoff is *very* high (0.97 by
  default), so its sensible near-miss floor is also high. Try
  `lower_cutoff: 0.65` for hey_jarvis instead of 0.55.

## Tests

```bash
cd /path/to/repo
python -m pytest tools/mww_training_capture/tests/
```

The suite exercises the full upload → list → label → export round trip
against an in-process FastAPI client.
