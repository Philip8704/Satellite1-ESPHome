"""Run with: ``python -m tools.mww_training_capture.service``."""
from __future__ import annotations

import os

import uvicorn

from .app import app


def main() -> None:
    host = os.environ.get("MWW_CAPTURE_HOST", "0.0.0.0")
    port = int(os.environ.get("MWW_CAPTURE_PORT", "8765"))
    uvicorn.run(app, host=host, port=port, log_level="info")


if __name__ == "__main__":
    main()
