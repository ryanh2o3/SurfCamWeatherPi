# Continuous integration and testing strategy

This document describes how to validate **SurfCamWeatherPi** in CI (no Raspberry Pi camera required) and how to layer **hardware soak tests** and **sanitizers** for production confidence.

The default GitHub Actions workflow lives at [`.github/workflows/ci.yml`](.github/workflows/ci.yml) and runs the Docker build described below.

## What CI can prove today

| Layer                                                       | Runs in CI?               | What it catches                                                                               |
| ----------------------------------------------------------- | ------------------------- | --------------------------------------------------------------------------------------------- |
| **Compile / link** (full stack: libcamera, GStreamer, curl) | Yes, via Docker           | API drift, missing includes, Debian/Pi-like dependency mismatches                             |
| **Threading / shutdown races**                              | Optional (TSAN build)     | Data races, lock-order issues between `requestComplete`, GStreamer teardown, and main threads |
| **HTTP client behavior**                                    | Partial (needs harness)   | URL encoding, timeouts, malformed JSON handling (best with a mock server)                     |
| **HLS upload ordering**                                     | Partial (extracted tests) | “Playlist only after segments” policy (`HlsUploader`)                                         |
| **End-to-end camera + encode + S3**                         | No (device + credentials) | Run on a Pi or lab host, not in generic cloud CI                                              |

The repository already ships **`Dockerfile.build`**, which installs Debian Bookworm–style dependencies and builds the `surfcam` binary. That is the **primary CI gate**: if this image fails to build, the tree does not match the supported Linux target.

## Required CI job: Docker release build

**Goal:** Every push and pull request must compile cleanly in an environment that mirrors the Pi toolchain (same major library versions as Bookworm where possible).

**Command:**

```bash
docker build -f Dockerfile.build -t surfcam-build .
```

Or use the wrapper:

```bash
./scripts/docker-build.sh
```

**Success criteria:** CMake configure completes and `surfcam` links without errors.

**Notes:**

- The image architecture follows the Docker host (e.g. `amd64` on GitHub Actions, `arm64` on Apple Silicon). The goal is **API compatibility**, not bit-identical binaries.
- If you cross-compile for Pi Zero with a sysroot, add a **second** CI matrix entry that uses your `toolchain-pi-zero.cmake` and `PI_SYSROOT` (documented in `CMakeLists.txt`).

## Optional CI job: ThreadSanitizer (TSAN)

**Goal:** Shake out races between libcamera completion callbacks, `pipelinePushMutex_`, and GStreamer teardown.

CMake already supports:

```bash
cmake -S . -B build-tsan -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-tsan -j"$(nproc)"
```

**CI caveats:**

- TSAN adds significant overhead; keep runs **short** (process start, immediate shutdown, or a few scripted API calls if you add a test binary).
- You need a Linux environment with TSAN-enabled libstdc++ (typical on `ubuntu-latest` / Debian builders).
- Full `surfcam` runtime under TSAN still needs a camera; for CI, prefer a **small test executable** that links only the thread-sensitive modules once you extract them (see “Roadmap” below).

**Suggested acceptance:** TSAN build succeeds and a minimal smoke binary runs for a few seconds without reported races.

## Optional CI job: static analysis

Reasonable additions that do not require hardware:

- **`clang-tidy`** on `src/*.cpp` with a checked-in `.clang-tidy` (focus: concurrency, `mutex`, `atomic`, `curl` usage).
- **`cppcheck`** with aggressive warning levels (may require suppressions for third-party headers).

These are **advisory** unless you ratchet warnings to zero over time.

## HTTP / API testing (mock server)

**Goal:** Verify behaviors that do not depend on libcamera:

- `spot_id` is URL-encoded in `GET /check-streaming-requested`.
- Presign + PUT flow handles non-2xx responses and timeouts.
- Malformed JSON does not abort the process (exceptions are contained inside `ApiClient`).

**Approach:**

1. Add a tiny HTTP server in Go (or use **WireMock** / **httptest**-style fixtures) that listens on `127.0.0.1` in CI.
2. Build a **`surfcam_test_http`** binary (or use `curl` scripts) that points `API_ENDPOINT` at `http://127.0.0.1:PORT` and drives:
   - 200 + valid JSON `{"stream_requested": false}`
   - 200 + invalid JSON (client should return safe default / false, not crash)
   - Connection reset / timeout (client surfaces failure without terminating other threads)

**CI wiring:** Start the mock server as a background step, run the test client, then tear down the server.

## HLS policy testing (unit-level)

**Goal:** Lock invariants for `HlsUploader`:

- `.ts` files are only uploaded when size-stable.
- `index.m3u8` uploads only when every referenced `.ts` line is in the uploaded set.
- Segment name cache eviction (`kMaxUploadedSegmentNames`) does not break ordering assumptions for your segment naming scheme.

**Approach:**

1. Refactor pure logic into **free functions** or a **`HlsUploader`** method that accepts an **`std::filesystem::path`** root and an **`ApiClient` interface** (abstract base or template) so tests can fake presign/PUT.
2. Add **GoogleTest** or **Catch2** via `FetchContent` in CMake, and register `ctest` in CI.

**CI command:**

```bash
cmake -S . -B build-test -DBUILD_TESTING=ON
cmake --build build-test
ctest --test-dir build-test --output-on-failure
```

_(The optional `BUILD_TESTING` target is a roadmap item until tests are added.)_

## Hardware and soak testing (outside default CI)

Run on a real Pi with the production systemd unit (or equivalent):

| Test                           | Duration | Pass criteria                                                                 |
| ------------------------------ | -------- | ----------------------------------------------------------------------------- |
| Idle + snapshots only          | 24 h     | No crash; memory stable; snapshots uploaded on schedule                       |
| Continuous HLS request         | 4–24 h   | No deadlock; encoder errors recover; S3 objects consistent                    |
| SIGTERM during upload          | Manual   | Clean shutdown within bounded time; no TSAN/ASAN reports on a sanitizer build |
| Low-memory simulation          | Manual   | `emergencyStopHls` stops encode; process survives                             |
| Network flakiness (`tc netem`) | 2–4 h    | API backoff without camera reinit storm; HLS resumes when network recovers    |

Capture **structured logs** (timestamps, session boundaries, HTTP status, encoder failure flag) so failures are diagnosable without reproducing on the bench.

## Recommended GitHub Actions shape

A minimal workflow runs the Docker build on every push:

```yaml
name: build
on:
  push:
    branches: [main]
  pull_request:
    branches: [main]

jobs:
  docker-build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build (Debian Bookworm toolchain)
        run: docker build -f Dockerfile.build -t surfcam-ci .
```

Add parallel jobs when ready:

- **`tsan`** (self-hosted or larger runner, or compile-only if no smoke binary yet).
- **`integration-mock-api`** (mock server + future test binary).

## Traceability to recent hardening changes

| Change                                                          | How to validate in CI                                                                                    |
| --------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| `pipelinePushMutex_` + `shutdownGstreamerPipeline`              | TSAN soak on hardware; future unit test of lock ordering is difficult—favor sanitizer + code review      |
| GStreamer `GST_MESSAGE_ERROR` → `consumeEncoderPipelineFailure` | Mock pipeline not trivial in CI; hardware test: kill encoder plugin or corrupt caps                      |
| API vs camera recovery split                                    | Mock API returning 500 / garbage JSON: process stays up; `camera.reinitialize` not called on JSON errors |
| Snapshot `mmap` + `curl_mime_data`                              | Build in Docker; optional integration test uploads a small JPEG to mock server                           |
| `curl_easy_escape` for `spot_id`                                | Mock server asserts query string decoding                                                                |

## Summary

- **Today:** Treat **`docker build -f Dockerfile.build .`** as the mandatory CI gate.
- **Next:** Add a **mock HTTP** job and **`ctest`**-driven unit tests for `HlsUploader` and URL building.
- **Parallel track:** **TSAN** and long **hardware soaks** for anything involving libcamera, GStreamer, and real S3 presigned PUTs.

This split keeps CI **fast and deterministic** while still mapping to **field reliability** for the full Pi deployment.
