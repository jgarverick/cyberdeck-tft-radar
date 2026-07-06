#!/usr/bin/env bash
set -euo pipefail

HOME_LAT="${HOME_LAT:-42.9017}"
HOME_LON="${HOME_LON:-78.4919}"
DECODER_JSON_DIR="${DECODER_JSON_DIR:-/run/dump1090-mutability}"
ENABLE_DECODER="${ENABLE_DECODER:-1}"

DECODER_PID=""

if [ "$ENABLE_DECODER" = "1" ]; then
  mkdir -p "$DECODER_JSON_DIR"

  # Start ADS-B decoder in the background and write aircraft.json for radar_feed.py.
  dump1090-mutability \
    --net \
    --lat "$HOME_LAT" \
    --lon "$HOME_LON" \
    --write-json "$DECODER_JSON_DIR" \
    --quiet &

  DECODER_PID=$!
fi

cleanup() {
  if [ -n "$DECODER_PID" ]; then
    kill "$DECODER_PID" 2>/dev/null || true
    wait "$DECODER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

export SOURCE_JSON="${SOURCE_JSON:-$DECODER_JSON_DIR/aircraft.json}"

exec python3 /app/radar_feed.py
