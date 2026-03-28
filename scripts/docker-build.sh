#!/usr/bin/env bash
# Compile the project in Debian (libcamera + GStreamer), for hosts without those deps (e.g. macOS).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${IMAGE:-surfcam-build}"
docker build -f "$ROOT/Dockerfile.build" -t "$IMAGE" "$ROOT"
echo "OK: image $IMAGE — run binary with:"
echo "  docker run --rm $IMAGE /src/build/surfcam"
