"""End-to-end tests that exercise the FastAPI service in-process.

Covers the full pipeline: upload a synthetic WAV that mimics what the Sat1
firmware sends, listen / fetch / label it, then verify the export ZIP
contains the right files. No network — uses fastapi.testclient + a tmp dir.
"""
from __future__ import annotations

import io
import json
import struct
import sys
import zipfile
from pathlib import Path

# Make sure the repo's tools/ folder is on sys.path when this file is run
# directly (e.g. from `python -m pytest`).
ROOT = Path(__file__).resolve().parents[3]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

import pytest
from fastapi.testclient import TestClient

from tools.mww_training_capture.service.app import create_app
from tools.mww_training_capture.service.storage import (
    LABEL_DISCARD,
    LABEL_FALSE_NEGATIVE,
    LABEL_TRUE_NEGATIVE,
    LABEL_UNLABELED,
)


def _make_wav(samples=8000, sample_rate=16000) -> bytes:
    """Build a 0.5 s 16-bit mono WAV of low-amplitude white noise."""
    import random
    rng = random.Random(42)
    pcm = b"".join(struct.pack("<h", rng.randint(-2000, 2000)) for _ in range(samples))
    riff_size = 36 + len(pcm)
    return (
        b"RIFF" + struct.pack("<I", riff_size) + b"WAVE"
        + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, sample_rate, sample_rate * 2, 2, 16)
        + b"data" + struct.pack("<I", len(pcm)) + pcm
    )


@pytest.fixture()
def client(tmp_path: Path):
    app = create_app(data_dir=tmp_path)
    with TestClient(app) as c:
        yield c, tmp_path


def test_healthz(client):
    c, _ = client
    r = c.get("/healthz")
    assert r.status_code == 200
    assert r.json() == {"ok": True}


def test_upload_round_trip(client):
    c, root = client
    wav = _make_wav()
    r = c.post(
        "/upload",
        content=wav,
        headers={
            "X-Wake-Word": "okay_nabu",
            "X-Max-Probability": "0.612",
            "X-Avg-Probability": "0.481",
            "X-Device-Name": "satellite-living-room",
            "X-Sample-Rate": "16000",
            "Content-Type": "audio/wav",
        },
    )
    assert r.status_code == 201, r.text
    body = r.json()
    cid = body["capture_id"]

    # The stored WAV must be byte-identical to what we POSTed.
    wav_files = list(root.rglob("*.wav"))
    assert len(wav_files) == 1
    assert wav_files[0].read_bytes() == wav

    # Sidecar JSON has the metadata we sent.
    json_files = list(root.rglob("*.json"))
    payload = json.loads(json_files[0].read_text())
    assert payload["device_name"] == "satellite-living-room"
    assert payload["wake_word"] == "okay_nabu"
    assert pytest.approx(payload["max_probability"], rel=1e-3) == 0.612
    assert pytest.approx(payload["avg_probability"], rel=1e-3) == 0.481
    assert payload["sample_rate"] == 16000
    assert payload["label"] == LABEL_UNLABELED

    # Listing returns it.
    r = c.get("/captures")
    assert r.status_code == 200
    assert r.json()["count"] == 1
    assert r.json()["items"][0]["capture_id"] == cid

    # WAV streaming endpoint returns the original bytes.
    r = c.get(f"/captures/{cid}/wav")
    assert r.status_code == 200
    assert r.headers["content-type"].startswith("audio/wav")
    assert r.content == wav


def test_upload_rejects_non_wav(client):
    c, _ = client
    r = c.post(
        "/upload",
        content=b"not-a-wav",
        headers={
            "X-Wake-Word": "okay_nabu",
            "X-Max-Probability": "0.6",
            "X-Avg-Probability": "0.4",
            "X-Device-Name": "sat1",
            "X-Sample-Rate": "16000",
        },
    )
    assert r.status_code == 400


def test_label_and_filter(client):
    c, _ = client
    wav = _make_wav()
    captures = []
    for ww, max_p in [("okay_nabu", "0.62"), ("hey_jarvis", "0.71"), ("okay_nabu", "0.55")]:
        r = c.post(
            "/upload", content=wav,
            headers={
                "X-Wake-Word": ww,
                "X-Max-Probability": max_p,
                "X-Avg-Probability": "0.4",
                "X-Device-Name": "sat1",
                "X-Sample-Rate": "16000",
            },
        )
        assert r.status_code == 201
        captures.append(r.json()["capture_id"])

    # Label the first as FN, second as TN, leave third unlabeled.
    assert c.post(f"/captures/{captures[0]}/label", json={"label": LABEL_FALSE_NEGATIVE, "notes": "spoke too fast"}).status_code == 200
    assert c.post(f"/captures/{captures[1]}/label", json={"label": LABEL_TRUE_NEGATIVE}).status_code == 200

    # Filter by label.
    fn = c.get(f"/captures?label={LABEL_FALSE_NEGATIVE}").json()
    assert fn["count"] == 1
    assert fn["items"][0]["capture_id"] == captures[0]
    assert fn["items"][0]["notes"] == "spoke too fast"

    unl = c.get(f"/captures?label={LABEL_UNLABELED}").json()
    assert unl["count"] == 1
    assert unl["items"][0]["capture_id"] == captures[2]

    # Filter by wake word.
    nabu = c.get("/captures?wake_word=okay_nabu").json()
    assert nabu["count"] == 2

    # Bad label rejected.
    r = c.post(f"/captures/{captures[2]}/label", json={"label": "totally_made_up"})
    assert r.status_code == 400


def test_export_zip(client):
    c, _ = client
    wav = _make_wav()
    ids = []
    for ww in ["okay_nabu", "hey_jarvis"]:
        r = c.post(
            "/upload", content=wav,
            headers={
                "X-Wake-Word": ww,
                "X-Max-Probability": "0.6",
                "X-Avg-Probability": "0.4",
                "X-Device-Name": "sat1",
                "X-Sample-Rate": "16000",
            },
        )
        ids.append(r.json()["capture_id"])
    # Label both as FN.
    for cid in ids:
        c.post(f"/captures/{cid}/label", json={"label": LABEL_FALSE_NEGATIVE})

    r = c.get(f"/export?label={LABEL_FALSE_NEGATIVE}")
    assert r.status_code == 200
    assert r.headers["content-type"] == "application/zip"
    assert int(r.headers["x-item-count"]) == 2

    zf = zipfile.ZipFile(io.BytesIO(r.content))
    names = zf.namelist()
    # Each capture contributes one .wav and one .json — both labelled & nested.
    assert sum(1 for n in names if n.endswith(".wav")) == 2
    assert sum(1 for n in names if n.endswith(".json")) == 2
    assert all(n.startswith(f"{LABEL_FALSE_NEGATIVE}/") for n in names)


def test_delete(client):
    c, root = client
    r = c.post(
        "/upload", content=_make_wav(),
        headers={
            "X-Wake-Word": "okay_nabu",
            "X-Max-Probability": "0.6",
            "X-Avg-Probability": "0.4",
            "X-Device-Name": "sat1",
            "X-Sample-Rate": "16000",
        },
    )
    cid = r.json()["capture_id"]
    assert c.delete(f"/captures/{cid}").status_code == 200
    # Files gone.
    assert list(root.rglob("*.wav")) == []
    assert list(root.rglob("*.json")) == []
    assert c.get(f"/captures/{cid}").status_code == 404


def test_summary_aggregates_devices_and_labels(client):
    c, _ = client
    wav = _make_wav()
    for dev, ww in [("sat1", "okay_nabu"), ("sat1", "okay_nabu"), ("sat2", "hey_jarvis")]:
        c.post(
            "/upload", content=wav,
            headers={
                "X-Wake-Word": ww,
                "X-Max-Probability": "0.6",
                "X-Avg-Probability": "0.4",
                "X-Device-Name": dev,
                "X-Sample-Rate": "16000",
            },
        )
    r = c.get("/summary")
    assert r.status_code == 200
    body = r.json()
    assert body["sat1"]["total"] == 2
    assert body["sat2"]["total"] == 1
    assert body["sat1"]["okay_nabu"]["by_label"][LABEL_UNLABELED] == 2
