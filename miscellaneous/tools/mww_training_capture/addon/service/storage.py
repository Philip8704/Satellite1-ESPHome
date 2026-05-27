"""Storage layer for the mww_training_capture companion service.

Captures land on disk under ``DATA_DIR/<device>/<wake_word>/<utc-iso>.wav``
plus a sibling ``.json`` sidecar holding probability metadata + the human
label that gets attached during review. We deliberately avoid a SQL DB so
the dataset is just-files-on-disk and easy to feed straight into the
microWakeWord training notebooks.
"""
from __future__ import annotations

import json
import os
import re
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterator, Optional


# Allowed labels - keep tightly scoped so the export step doesn't have to
# reconcile freeform strings.
LABEL_UNLABELED = "unlabeled"
LABEL_TRUE_NEGATIVE = "true_negative"
LABEL_FALSE_NEGATIVE = "false_negative"
LABEL_DISCARD = "discard"
ALLOWED_LABELS = {LABEL_UNLABELED, LABEL_TRUE_NEGATIVE, LABEL_FALSE_NEGATIVE, LABEL_DISCARD}


# Filesystem-safe identifiers for device + wake-word path components.
_SAFE_NAME_RE = re.compile(r"[^a-zA-Z0-9._-]+")


def _safe(value: str, fallback: str) -> str:
    if not value:
        return fallback
    cleaned = _SAFE_NAME_RE.sub("_", value).strip("._")
    return cleaned or fallback


@dataclass
class CaptureMetadata:
    """One near-miss capture's metadata sidecar."""
    capture_id: str
    device_name: str
    wake_word: str
    timestamp_utc: str
    max_probability: float
    avg_probability: float
    sample_rate: int
    label: str = LABEL_UNLABELED
    notes: str = ""
    wav_bytes: int = 0
    extra: dict = field(default_factory=dict)

    @property
    def relative_dir(self) -> Path:
        return Path(_safe(self.device_name, "device")) / _safe(self.wake_word, "wakeword")

    @property
    def basename(self) -> str:
        return self.capture_id


class CaptureStore:
    """File-backed repository of captures."""

    def __init__(self, data_dir):
        self.data_dir = Path(data_dir)
        # Be tolerant of read-only / not-yet-mounted dirs so the module is
        # safe to import in any environment. The first ``save`` will retry
        # the mkdir and surface the real error.
        try:
            self.data_dir.mkdir(parents=True, exist_ok=True)
        except (PermissionError, FileNotFoundError, OSError):
            pass

    # ---------------------------------------------------------------- write
    def save(self, meta: CaptureMetadata, wav_bytes: bytes) -> Path:
        target_dir = self.data_dir / meta.relative_dir
        target_dir.mkdir(parents=True, exist_ok=True)
        wav_path = target_dir / f"{meta.basename}.wav"
        json_path = target_dir / f"{meta.basename}.json"
        wav_path.write_bytes(wav_bytes)
        meta.wav_bytes = len(wav_bytes)
        json_path.write_text(json.dumps(asdict(meta), indent=2, ensure_ascii=False))
        return wav_path

    # ---------------------------------------------------------------- read
    def iter_metadata(self) -> Iterator[CaptureMetadata]:
        for json_path in sorted(self.data_dir.rglob("*.json")):
            try:
                payload = json.loads(json_path.read_text())
                yield CaptureMetadata(**payload)
            except (json.JSONDecodeError, TypeError, ValueError):
                continue

    def get(self, capture_id: str):
        for json_path in self.data_dir.rglob(f"{capture_id}.json"):
            payload = json.loads(json_path.read_text())
            wav_path = json_path.with_suffix(".wav")
            return CaptureMetadata(**payload), wav_path
        raise FileNotFoundError(capture_id)

    def update_label(self, capture_id: str, label: str, notes: Optional[str] = None) -> CaptureMetadata:
        if label not in ALLOWED_LABELS:
            raise ValueError(f"unknown label: {label!r}")
        meta, _ = self.get(capture_id)
        meta.label = label
        if notes is not None:
            meta.notes = notes
        json_path = self.data_dir / meta.relative_dir / f"{meta.basename}.json"
        json_path.write_text(json.dumps(asdict(meta), indent=2, ensure_ascii=False))
        return meta

    def delete(self, capture_id: str) -> None:
        meta, wav_path = self.get(capture_id)
        json_path = self.data_dir / meta.relative_dir / f"{meta.basename}.json"
        for p in (wav_path, json_path):
            if p.exists():
                p.unlink()

    # ------------------------------------------------------------ summary
    def summary(self) -> dict:
        counts = {}
        for meta in self.iter_metadata():
            d = counts.setdefault(meta.device_name, {"total": 0})
            ww = d.setdefault(meta.wake_word, {"total": 0, "by_label": {l: 0 for l in ALLOWED_LABELS}})
            ww["total"] += 1
            d["total"] += 1
            ww["by_label"][meta.label] = ww["by_label"].get(meta.label, 0) + 1
        return counts


def make_capture_id(device_name: str, wake_word: str, when: Optional[datetime] = None) -> str:
    """Deterministic-but-unique capture id used as the on-disk basename."""
    when = (when or datetime.now(timezone.utc)).astimezone(timezone.utc)
    stamp = when.strftime("%Y%m%dT%H%M%S_%f")
    return f"{stamp}_{_safe(device_name, 'dev')}_{_safe(wake_word, 'ww')}"
