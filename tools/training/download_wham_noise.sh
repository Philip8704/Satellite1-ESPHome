#!/usr/bin/env bash
# Fast WHAM-noise download for microWakeWord training.
#
# The default source — the AWS S3 bucket the microWakeWord notebook uses —
# is bandwidth-limited per connection but serves parallel range requests
# happily, so we hammer it with 16 chunked streams via aria2c. On a gigabit
# link that pulls the full ~17 GB in ~5-15 minutes instead of an hour+.
#
# To use a mirror instead, set WHAM_URL before running:
#   WHAM_URL=https://huggingface.co/datasets/<org>/<name>/resolve/main/wham_noise.zip \
#     ./download_wham_noise.sh
#
# Tunables (env vars):
#   WHAM_URL        Source URL (default: original AWS bucket)
#   OUT_DIR         Where to put the zip   (default: ./wham_data)
#   OUT_FILE        Filename               (default: wham_noise.zip)
#   CONNECTIONS     Parallel TCP streams   (default: 16; bump to 32 if your link can take it)

set -euo pipefail

WHAM_URL="${WHAM_URL:-https://my-bucket-a8b4b49c25c811ee9a7e8bba05fa24c7.s3.amazonaws.com/wham_noise.zip}"
OUT_DIR="${OUT_DIR:-./wham_data}"
OUT_FILE="${OUT_FILE:-wham_noise.zip}"
CONNECTIONS="${CONNECTIONS:-16}"

if ! command -v aria2c >/dev/null 2>&1; then
  echo "aria2c not found. Install it first:" >&2
  echo "  Debian/Ubuntu: sudo apt-get install -y aria2" >&2
  echo "  macOS:         brew install aria2" >&2
  echo "  Arch:          sudo pacman -S aria2" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

echo "→ Source:      $WHAM_URL"
echo "→ Destination: $OUT_DIR/$OUT_FILE"
echo "→ Streams:     $CONNECTIONS"
echo

aria2c \
  --max-connection-per-server="$CONNECTIONS" \
  --split="$CONNECTIONS" \
  --min-split-size=10M \
  --continue=true \
  --auto-file-renaming=false \
  --allow-overwrite=false \
  --file-allocation=falloc \
  --console-log-level=warn \
  --summary-interval=5 \
  --dir="$OUT_DIR" \
  --out="$OUT_FILE" \
  "$WHAM_URL"

echo
echo "→ Verifying ZIP integrity..."
if unzip -tq "$OUT_DIR/$OUT_FILE" >/dev/null; then
  size=$(du -h "$OUT_DIR/$OUT_FILE" | cut -f1)
  echo "✓ ZIP OK ($size at $OUT_DIR/$OUT_FILE)"
else
  echo "✗ ZIP corrupted — delete $OUT_DIR/$OUT_FILE and re-run." >&2
  exit 1
fi
