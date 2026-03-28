#!/usr/bin/env bash
# Run mock API + surfcam_test_http after a normal CMake build with BUILD_TESTING=ON.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${SURFCAM_BUILD_DIR:-build}"
PORT="${SURFCAM_MOCK_PORT:-18080}"
HTTP_BIN="${ROOT}/${BUILD_DIR}/surfcam_test_http"

if [[ ! -f "$HTTP_BIN" ]]; then
  echo "error: $HTTP_BIN not found (build with -DBUILD_TESTING=ON)" >&2
  exit 1
fi

python3 "$ROOT/scripts/mock_api_server.py" "$PORT" &
MOCK_PID=$!

cleanup() {
  kill "$MOCK_PID" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 100); do
  if python3 -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:${PORT}/health', timeout=1)" 2>/dev/null; then
    break
  fi
  sleep 0.05
done

export SURFCAM_TEST_API_BASE="http://127.0.0.1:${PORT}"
export API_KEY="${API_KEY:-test-api-key}"

"$HTTP_BIN"
