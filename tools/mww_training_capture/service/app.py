"""FastAPI service that receives near-miss WAVs from the Sat1 firmware.

Endpoints
---------

POST /upload
    Body: WAV bytes.
    Required headers:
        X-Wake-Word         identifier of the wake word that nearly fired
        X-Max-Probability   float 0..1
        X-Avg-Probability   float 0..1
        X-Device-Name       Sat1 ESPHome node_name
        X-Sample-Rate       int (typically 16000)
    Returns: 201 + json metadata.

GET  /captures
    JSON list of all metadata sidecars.
GET  /captures/{capture_id}
    JSON metadata for one capture.
GET  /captures/{capture_id}/wav
    The raw WAV bytes (audio/wav).
POST /captures/{capture_id}/label
    Body: {"label": "...", "notes": "..."}
GET  /
    Tiny single-page review UI.
GET  /export?label=false_negative
    ZIP of WAVs filtered by label, ready to drop into the microWakeWord
    training notebook.
GET  /healthz
    {"ok": true}
"""
from __future__ import annotations

import io
import json
import os
import zipfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, HTTPException, Request, Response
from fastapi.responses import HTMLResponse, JSONResponse, StreamingResponse
from pydantic import BaseModel, Field

from .storage import (
    ALLOWED_LABELS,
    CaptureMetadata,
    CaptureStore,
    LABEL_FALSE_NEGATIVE,
    LABEL_UNLABELED,
    make_capture_id,
)


DATA_DIR = Path(os.environ.get("MWW_CAPTURE_DATA_DIR", "/data/mww_captures"))


class LabelPayload(BaseModel):
    """Payload for POST /captures/{id}/label. Defined at module scope so
    FastAPI can introspect it as a body type - closure-defined Pydantic
    models are not always recognised as request bodies on FastAPI 0.10x+."""
    label: str = Field(..., description="One of: " + ", ".join(sorted(ALLOWED_LABELS)))
    notes: Optional[str] = None


def create_app(data_dir: Optional[Path] = None) -> FastAPI:
    """Factory so tests can pass a tmp dir without touching a singleton."""
    store = CaptureStore(data_dir or DATA_DIR)
    app = FastAPI(title="MWW Training Capture", version="1.0.0")
    app.state.store = store

    @app.get("/healthz")
    def healthz():
        return {"ok": True}

    @app.post("/upload", status_code=201)
    async def upload(request: Request):
        wav_bytes = await request.body()
        if len(wav_bytes) < 44 or wav_bytes[:4] != b"RIFF":
            raise HTTPException(status_code=400, detail="payload is not a RIFF/WAV file")

        try:
            sample_rate = int(request.headers.get("X-Sample-Rate", "16000"))
            max_p = float(request.headers.get("X-Max-Probability", "0"))
            avg_p = float(request.headers.get("X-Avg-Probability", "0"))
        except ValueError:
            raise HTTPException(status_code=400, detail="invalid numeric header")

        device = request.headers.get("X-Device-Name", "unknown_device")
        wake_word = request.headers.get("X-Wake-Word", "unknown")

        when = datetime.now(timezone.utc)
        capture_id = make_capture_id(device, wake_word, when)
        meta = CaptureMetadata(
            capture_id=capture_id,
            device_name=device,
            wake_word=wake_word,
            timestamp_utc=when.isoformat(),
            max_probability=max_p,
            avg_probability=avg_p,
            sample_rate=sample_rate,
        )
        path = store.save(meta, wav_bytes)
        return JSONResponse({
            "capture_id": capture_id,
            "stored_path": str(path.relative_to(store.data_dir)),
            "wav_bytes": len(wav_bytes),
        }, status_code=201)

    @app.get("/captures")
    def list_captures(label: Optional[str] = None, device: Optional[str] = None, wake_word: Optional[str] = None):
        items = []
        for meta in store.iter_metadata():
            if label and meta.label != label:
                continue
            if device and meta.device_name != device:
                continue
            if wake_word and meta.wake_word != wake_word:
                continue
            items.append(meta.__dict__)
        items.sort(key=lambda m: m["timestamp_utc"], reverse=True)
        return {"count": len(items), "items": items}

    @app.get("/captures/{capture_id}")
    def get_capture(capture_id: str):
        try:
            meta, _ = store.get(capture_id)
        except FileNotFoundError:
            raise HTTPException(404, "no such capture")
        return meta.__dict__

    @app.get("/captures/{capture_id}/wav")
    def get_capture_wav(capture_id: str):
        try:
            _, wav_path = store.get(capture_id)
        except FileNotFoundError:
            raise HTTPException(404, "no such capture")
        return Response(content=wav_path.read_bytes(), media_type="audio/wav")

    @app.post("/captures/{capture_id}/label")
    def label_capture(capture_id: str, payload: LabelPayload):
        try:
            meta = store.update_label(capture_id, payload.label, payload.notes)
        except FileNotFoundError:
            raise HTTPException(404, "no such capture")
        except ValueError as e:
            raise HTTPException(400, str(e))
        return meta.__dict__

    @app.delete("/captures/{capture_id}")
    def delete_capture(capture_id: str):
        try:
            store.delete(capture_id)
        except FileNotFoundError:
            raise HTTPException(404, "no such capture")
        return {"deleted": capture_id}

    @app.get("/summary")
    def summary():
        return store.summary()

    @app.get("/export")
    def export(label: str = LABEL_FALSE_NEGATIVE):
        if label not in ALLOWED_LABELS:
            raise HTTPException(400, f"label must be one of {sorted(ALLOWED_LABELS)}")
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            count = 0
            for meta in store.iter_metadata():
                if meta.label != label:
                    continue
                _, wav_path = store.get(meta.capture_id)
                # Layout matches what the microWakeWord training notebooks
                # expect: <label>/<wake_word>/<device>/<id>.wav
                arc = f"{meta.label}/{meta.wake_word}/{meta.device_name}/{meta.capture_id}.wav"
                zf.write(wav_path, arc)
                zf.writestr(arc[:-4] + ".json", json.dumps(meta.__dict__, indent=2))
                count += 1
        buf.seek(0)
        return StreamingResponse(
            buf,
            media_type="application/zip",
            headers={
                "Content-Disposition": f'attachment; filename="mww_export_{label}.zip"',
                "X-Item-Count": str(count),
            },
        )

    @app.get("/", response_class=HTMLResponse)
    def index():
        return _UI_HTML

    return app


# Top-level app instance used by uvicorn / the HA add-on entrypoint.
app = create_app()


_UI_HTML = """<!doctype html>
<html lang=en>
<head>
<meta charset=utf-8>
<title>MWW Training Capture</title>
<style>
:root { color-scheme: light dark; font-family: system-ui, sans-serif; }
body { margin: 0; padding: 1rem 1.5rem; max-width: 980px; }
h1 { margin: 0 0 .25rem; font-size: 1.25rem; }
small.hint { color: #666; }
table { border-collapse: collapse; width: 100%; margin-top: 1rem; font-size: .9rem; }
th, td { padding: .35rem .55rem; border-bottom: 1px solid #ccc4; text-align: left; }
tr:hover { background: #8881; }
button { font: inherit; padding: .25rem .55rem; margin: 0 .15rem; border-radius: 6px; border: 1px solid #888; background: transparent; cursor: pointer; }
button.fn { background: #4a7; color: white; border-color: #382; }
button.tn { background: #c64; color: white; border-color: #532; }
button.dc { background: #888; color: white; border-color: #444; }
audio { vertical-align: middle; }
.filters { margin-top: .5rem; display: flex; gap: .5rem; flex-wrap: wrap; align-items: center; }
.filters label { font-size: .85rem; }
.label-pill { display: inline-block; padding: 1px 8px; border-radius: 999px; font-size: .75rem; }
.unlabeled { background: #888; color: white; }
.true_negative { background: #c64; color: white; }
.false_negative { background: #4a7; color: white; }
.discard { background: #555; color: white; }
.prob { font-variant-numeric: tabular-nums; }
</style>
</head>
<body>
<h1>microWakeWord training capture</h1>
<small class="hint">Listen to each near-miss, label it, then export the
<code>false_negative</code> set to feed the training notebooks.</small>

<div class="filters">
  <label>Label:
    <select id="filter-label">
      <option value="">all</option>
      <option value="unlabeled" selected>unlabeled</option>
      <option value="false_negative">false_negative</option>
      <option value="true_negative">true_negative</option>
      <option value="discard">discard</option>
    </select>
  </label>
  <label>Device: <input id="filter-device" placeholder="(any)"></label>
  <label>Wake word: <input id="filter-ww" placeholder="(any)"></label>
  <button onclick="reload()">Refresh</button>
  <button onclick="window.location='/export?label=false_negative'">Export FN ZIP</button>
</div>

<div id="summary" style="margin-top:1rem;font-size:.85rem;"></div>
<table id="captures">
<thead><tr><th>When (UTC)</th><th>Device</th><th>Wake word</th><th>Max</th><th>Avg</th><th>Audio</th><th>Label</th><th>Action</th></tr></thead>
<tbody></tbody>
</table>

<script>
async function reload() {
  const params = new URLSearchParams();
  const lbl = document.getElementById('filter-label').value;
  const dev = document.getElementById('filter-device').value;
  const ww  = document.getElementById('filter-ww').value;
  if (lbl) params.set('label', lbl);
  if (dev) params.set('device', dev);
  if (ww) params.set('wake_word', ww);
  const r = await fetch('/captures?' + params.toString());
  const j = await r.json();
  const tb = document.querySelector('#captures tbody'); tb.innerHTML='';
  for (const m of j.items) {
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td>${m.timestamp_utc}</td>
      <td>${m.device_name}</td>
      <td>${m.wake_word}</td>
      <td class=prob>${(m.max_probability||0).toFixed(3)}</td>
      <td class=prob>${(m.avg_probability||0).toFixed(3)}</td>
      <td><audio controls preload=metadata src="/captures/${m.capture_id}/wav"></audio></td>
      <td><span class="label-pill ${m.label}">${m.label}</span></td>
      <td>
        <button class=fn onclick="setLabel('${m.capture_id}','false_negative')">FN</button>
        <button class=tn onclick="setLabel('${m.capture_id}','true_negative')">TN</button>
        <button class=dc onclick="setLabel('${m.capture_id}','discard')">discard</button>
        <button onclick="del('${m.capture_id}')">del</button>
      </td>`;
    tb.appendChild(tr);
  }
  document.getElementById('summary').textContent = `${j.count} matching captures`;
}
async function setLabel(id, label) {
  await fetch('/captures/' + id + '/label', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({label})});
  reload();
}
async function del(id) {
  if (!confirm('Delete capture ' + id + '?')) return;
  await fetch('/captures/' + id, {method:'DELETE'});
  reload();
}
reload();
</script>
</body>
</html>
"""
