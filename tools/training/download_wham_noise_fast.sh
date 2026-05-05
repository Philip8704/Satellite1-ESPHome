#!/usr/bin/env bash
# WHAM-noise download tuned for a 1 Gbit/s residential line.
#
# Differences vs. download_wham_noise.sh (the conservative default):
#   • 32 parallel TCP streams (was 16) — most CDNs/S3 rate-limit per-conn
#     in the low MB/s; doubling streams pushes aggregate past 100 MB/s.
#   • 20 MB min split size (was 10) — fewer, larger chunks → less per-
#     request overhead, important once each stream has work to do.
#   • 256 MB aria2c disk cache — coalesces writes so the file system
#     isn't the bottleneck on slow SSDs / network volumes.
#   • Hard cap at 120 MB/s (~960 Mbit) — leaves headroom for TCP
#     retransmits and your other LAN traffic. Without a cap, a fully
#     saturated downlink starves SSH/HA/etc.
#   • Drop any stream that stalls below 1 MB/s for 30 s, retry up to 10x
#     — keeps a single bad route from holding back the rest.
#
# Defaults to data/downloads/ as the destination. Override with env vars.

set -euo pipefail

WHAM_URL="${WHAM_URL:-https://my-bucket-a8b4b49c25c811ee9a7e8bba05fa24c7.s3.amazonaws.com/wham_noise.zip}"
OUT_DIR="${OUT_DIR:-./data/downloads}"
OUT_FILE="${OUT_FILE:-wham_noise.zip}"

if ! command -v aria2c >/dev/null 2>&1; then
  echo "aria2c not found. Install with: apt-get install -y aria2 unzip" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

echo "→ Source:      $WHAM_URL"
echo "→ Destination: $OUT_DIR/$OUT_FILE"
echo "→ Profile:     1 Gbit/s (32 streams, ~120 MB/s cap)"
echo

aria2c \
  --max-connection-per-server=32 \
  --split=32 \
  --min-split-size=20M \
  --continue=true \
  --auto-file-renaming=false \
  --allow-overwrite=false \
  --file-allocation=falloc \
  --disk-cache=256M \
  --max-overall-download-limit=120M \
  --lowest-speed-limit=1M \
  --connect-timeout=10 \
  --timeout=30 \
  --max-tries=10 \
  --retry-wait=3 \
  --console-log-level=warn \
  --summary-interval=2 \
  --enable-color=false \
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
