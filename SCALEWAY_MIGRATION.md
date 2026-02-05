# Scaleway Migration Options

The current architecture streams H.264 video from a Pi Zero W to AWS Kinesis Video
Streams (KVS). Scaleway has no KVS equivalent, so switching requires a different
streaming backend. Below are three concrete options, ordered by complexity.

---

## Option A — RTMP relay on a Scaleway instance (recommended)

### How it works

Run a lightweight RTMP/WebRTC relay (MediaMTX or SRS) on a small Scaleway instance.
The Pi pushes H.264 over RTMP. The relay re-serves it as HLS/WebRTC to viewers.

```
Pi Zero ──RTMP──▶  Scaleway DEV1-S (MediaMTX)  ──HLS/WebRTC──▶  Viewers
```

### What changes on the Pi

Replace `KinesisStreamer` with an RTMP push. GStreamer already has `rtmpsink`, so
the pipeline becomes:

```
appsrc → videoconvert → h264enc → flvmux → rtmpsink location=rtmp://host/live/key
```

This means `KinesisStreamer.cpp` gets replaced entirely. The camera, API client,
and main thread architecture stay the same.

### Server side

- Scaleway DEV1-S instance (~€4/mo, 2 vCPU, 2 GB RAM) — more than enough
- MediaMTX (https://github.com/bluenviron/mediamtx): single Go binary, ~15 MB
  - Receives RTMP, re-serves as HLS + WebRTC
  - Config is a single YAML file
  - No external dependencies
- TLS via Let's Encrypt + nginx or Caddy reverse proxy

### Pros / cons

| Pros | Cons |
|------|------|
| Simplest server setup (single binary) | Always-on instance cost (~€4/mo) |
| Low latency (sub-second with WebRTC) | You manage the relay server |
| GStreamer pipeline change is small | Need to handle relay uptime/monitoring |
| Removes all AWS SDK dependencies | |
| No cross-compilation of AWS SDK needed | |

### Rough cost

- DEV1-S: ~€4/mo
- Bandwidth: ~€0.01/GB (400Kbps stream ≈ 130 GB/mo if streaming 24/7 = ~€1.30)
- Total: ~€5-6/mo for always-on, less if streaming is on-demand

---

## Option B — HLS segments to Scaleway Object Storage

### How it works

Encode HLS segments (.ts files + .m3u8 playlist) locally on the Pi, then upload
them to Scaleway Object Storage (S3-compatible). Viewers fetch the playlist URL
directly from the bucket.

```
Pi Zero ──GStreamer hlssink──▶  local .ts + .m3u8
                                    │
                               curl/s3cmd upload
                                    │
                                    ▼
                          Scaleway Object Storage  ◀──GET──  Viewers
```

### What changes on the Pi

- Replace `KinesisStreamer` with an HLS segment uploader
- GStreamer pipeline uses `hlssink` instead of `appsink`
- A small upload loop watches the output directory and pushes new `.ts` segments
  and the updated `.m3u8` to Object Storage via S3-compatible API (libcurl
  with SigV4 or the `aws` CLI pointed at Scaleway's endpoint)

### Server side

- No server to manage — Object Storage is fully managed
- Set bucket policy to allow public reads on the stream prefix
- Optional: Scaleway Serverless Function to clean up old segments

### Pros / cons

| Pros | Cons |
|------|------|
| No server to manage | Higher latency (segment duration + upload, typically 5-10s) |
| Cheapest option at low usage | Not truly "live" — feels like a delayed feed |
| Scales to zero cost when idle | Need segment cleanup logic |
| Simple S3-compatible upload | Playlist refresh timing can be tricky |

### Rough cost

- Object Storage: €0.00002/GB stored, €0.01/GB transferred
- At 400Kbps, 1 hour of streaming ≈ 180 MB ≈ effectively free
- Even heavy use stays under €1/mo

---

## Option C — RTMP to Scaleway + Scaleway Object Storage for recordings

### How it works

Combine Options A and B: live streaming via RTMP relay, with the relay also
writing HLS segments to Object Storage for replay/archive.

```
Pi Zero ──RTMP──▶  Scaleway DEV1-S (MediaMTX)  ──WebRTC──▶  Live viewers
                         │
                    record to HLS
                         │
                         ▼
                Scaleway Object Storage  ◀──GET──  Replay viewers
```

### What changes on the Pi

Same as Option A — RTMP push only.

### Server side

- Same DEV1-S instance as Option A
- MediaMTX configured with `record: yes` and an upload script or
  direct S3 mount (s3fs / rclone) for the recording directory
- Lifecycle policy on the bucket to auto-delete recordings older than N days

### Pros / cons

| Pros | Cons |
|------|------|
| Live + replay in one setup | Most moving parts of the three options |
| Low live latency | Instance cost + storage cost |
| Recordings for free essentially | More config to maintain |

### Rough cost

- DEV1-S: ~€4/mo
- Object Storage for recordings: ~€0.50/mo for a week of retention at 400Kbps
- Total: ~€5/mo

---

## Comparison summary

| | Latency | Pi-side changes | Server infra | Monthly cost |
|---|---|---|---|---|
| **A: RTMP relay** | <1s (WebRTC) | Replace KinesisStreamer | 1 instance | ~€5 |
| **B: HLS to S3** | 5-10s | Replace KinesisStreamer + add uploader | None (managed) | <€1 |
| **C: RTMP + archive** | <1s live, on-demand replay | Replace KinesisStreamer | 1 instance + bucket | ~€5 |

---

## Recommendation

**Option A** is the best starting point. It gives low-latency live streaming with
the smallest amount of code change (swap the KVS SDK for a GStreamer `rtmpsink`
pipeline). It also completely removes the AWS C++ SDK from the build, which
eliminates the cross-compilation pain that is the main blocker today.

If you later want recordings, upgrade to Option C — the Pi-side code stays
identical.

---

## Implementation sketch for Option A

### 1. Pi-side: new `RtmpStreamer` class

Replace `KinesisStreamer.h/.cpp` with an `RtmpStreamer` that builds this pipeline:

```cpp
// GStreamer pipeline string (constructed in initializeGstreamerPipeline)
// appsrc ! videoconvert ! h264enc ! h264parse ! flvmux streamable=true ! rtmpsink location=<url>
```

The `sendFrame()` method just pushes raw RGB frames to `appsrc` — GStreamer
handles encoding, muxing, and network push in one pipeline. No fragment
accumulation needed.

### 2. Server: MediaMTX on Scaleway

```bash
# On the Scaleway DEV1-S instance:
wget https://github.com/bluenviron/mediamtx/releases/latest/download/mediamtx_linux_amd64.tar.gz
tar xzf mediamtx_linux_amd64.tar.gz
./mediamtx
```

Default config accepts RTMP on :1935, serves HLS on :8888 and WebRTC on :8889.

### 3. CMakeLists.txt changes

Remove these lines:
```cmake
find_package(aws-cpp-sdk-core REQUIRED)
find_package(aws-cpp-sdk-kinesisvideo REQUIRED)
find_package(aws-cpp-sdk-kinesis-video-media REQUIRED)
find_package(KinesisVideoProducerCpp REQUIRED)
```

And from `target_link_libraries`:
```cmake
aws-cpp-sdk-core
aws-cpp-sdk-kinesisvideo
aws-cpp-sdk-kinesis-video-media
KinesisVideoProducerCpp
```

No new dependencies — GStreamer already links `rtmpsink` via its plugin system.

### 4. Config changes

Replace `KINESIS_STREAM_NAME` / `AWS_REGION` with:
```cpp
inline static const std::string RTMP_URL = "rtmp://<scaleway-ip>/live/surfcam";
```
