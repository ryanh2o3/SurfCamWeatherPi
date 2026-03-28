#!/usr/bin/env bash
# ThreadSanitizer build and manual stress hints for surfcam (run on Pi or host with libcamera + GStreamer).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build-tsan}"

echo "Configuring with ENABLE_TSAN=ON -> $BUILD"
cmake -S "$ROOT" -B "$BUILD" -DENABLE_TSAN=ON
cmake --build "$BUILD" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo ""
echo "Run (example):"
echo "  export API_KEY=... SPOT_ID=..."
echo "  TSAN_OPTIONS=second_deadlock_stack=1:halt_on_error=1 $BUILD/surfcam"
echo ""
echo "Manual stress cases:"
echo "  - SIGTERM / SIGINT while HLS session active and while stream check is mid-HTTP."
echo "  - Start live stream, trigger snapshot interval overlap with streaming."
echo "  - Toggle stream request API rapidly so start/stop churns."
