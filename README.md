# SurfCam Weather Pi - still a work in progress

A Raspberry Pi app that captures surf conditions and streams live video. It takes photos every 30 seconds and can stream live video when someone requests it.

## What it does

This runs on a Raspberry Pi Zero W with a camera. It does two main things:

- **Takes snapshots**: Every 30 seconds it snaps a photo and uploads it to an API
- **Live HLS to S3**: When someone asks for it, it encodes HLS locally (`.m3u8` + `.ts`) and uploads each file to your bucket using **presigned PUT** URLs from your API (no AWS SDK on the Pi)

Since the Pi Zero is pretty limited, the app keeps an eye on things like:

- Memory usage (so it doesn't crash)
- CPU temperature (so it doesn't overheat)
- Automatically stops streaming after a while if no one's watching
- Uses GStreamer to encode video efficiently
- Handles the camera safely across multiple threads

## How it's built

Written in C++17. Here's what the main pieces do:

1. **CameraManager** (`src/CameraManager.cpp`)

   - Talks to the camera using libcamera
   - Takes photos and records video
   - Sets up GStreamer to encode video as H.264
   - Keeps things safe when multiple threads use the camera

2. **ApiClient** (`src/ApiClient.cpp`)

   - Sends HTTP requests to the API
   - Uploads photos
   - Calls `POST /hls/presign` and **PUT**s segment/playlist bytes to S3
   - Checks if anyone wants to watch a stream
   - Uses libcurl per request; **`curl_global_init` runs once in `main`** (not in the client ctor)

3. **Config** (`include/Config.h`, `src/Config.cpp`)

   - Compile-time defaults (intervals, camera size, HLS paths, etc.) live in `Config.h`
   - **`API_KEY`**, **`SPOT_ID`**, and **`SNAPSHOT_PATH`** are read from the environment when the process starts

4. **HlsUploader** (`src/HlsUploader.cpp`)

   - Watches the local HLS directory (`/tmp/surfcam-hls` by default)
   - Uploads stable `.ts` segments first; uploads **`index.m3u8` only after** every segment file listed in the playlist has been uploaded successfully (avoids a playlist that points at missing objects)
   - Tracks uploaded segment names with a bounded cache so memory does not grow forever

5. **Main Application** (`src/main.cpp`)

   - Keeps everything working together
   - Runs threads for taking photos and streaming
   - Watches memory and temperature (low memory / high temp **requests** an HLS stop on the stream thread; only the stream-check loop joins that worker—no `join` from the monitor)
   - Shuts down cleanly when needed (async-signal-safe SIGINT/SIGTERM handler)

## What you need

### Hardware

- Raspberry Pi Zero W (or similar)
- Raspberry Pi Camera Module V2 or V3
- Raspbian OS or similar Linux distro
- Internet connection

### Software to build it

- CMake 3.10+
- C++17 compiler (gcc or clang)
- OpenCV
- libcamera
- GStreamer 1.0 (including **plugins-bad** for `hlssink`)
- libcurl
- pthread

### Installing on Raspberry Pi

```bash
# Update everything
sudo apt update && sudo apt upgrade -y

# Get build tools
sudo apt install -y build-essential cmake git pkg-config

# Camera stuff
sudo apt install -y libcamera-dev libcamera-apps

# OpenCV
sudo apt install -y libopencv-dev

# GStreamer (hlssink lives in plugins-bad)
sudo apt install -y libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev

# HTTP client
sudo apt install -y libcurl4-openssl-dev

# JSON library
sudo apt install -y nlohmann-json3-dev
```

## Building

```bash
# Go to the project directory
cd /path/to/SurfCamWeatherPi

# Make a build folder
mkdir build && cd build

# Configure
cmake ..

# Build it
make -j4

# Install (if you want)
sudo make install
```

## Configuration

**Environment (required at startup)** — read in `main` via `Config::loadFromEnvironment()`. If `API_KEY` or `SPOT_ID` is missing, the program exits with an error (no `exit()` from static initializers).

| Variable | Required | Description |
|----------|----------|-------------|
| `API_KEY` | Yes | API key sent as `Authorization: ApiKey …` |
| `SPOT_ID` | Yes | Surf spot id (e.g. `Ireland_Donegal_Ballymastocker`) |
| `SNAPSHOT_PATH` | No | Local JPEG path for each snapshot (default: `/tmp/surfcam-snapshot.jpg`) |

Set these in `script.sh`, systemd `Environment=`, or your shell before `surfcam`.

**Compile-time defaults** — edit `include/Config.h` (rebuild after changes):

- `SNAPSHOT_INTERVAL`: Photo interval (default: 30 s)
- `STREAM_CHECK_INTERVAL`: How often to poll streaming requests (default: 5 s)
- `STREAM_TIMEOUT`: Keep streaming this long after the API last returned `stream_requested: true` (default: 30 s); enforced in `main`, not duplicated in the API client
- `CAMERA_WIDTH` / `CAMERA_HEIGHT`: Video size (default: 1280×720)
- `STREAM_FPS`: Video frame rate (default: 15)
- `API_ENDPOINT`: Base URL for the API (snapshots + presign), e.g. `https://treblesurf.com/api`
- `HLS_OUTPUT_DIR` / `HLS_PLAYLIST_NAME`: Where GStreamer writes HLS output (default under `/tmp/surfcam-hls`)
- `HLS_PRESIGN_PATH`: Path appended to `API_ENDPOINT` for presigned PUTs (default: `/hls/presign`)

## Running it

### As a service

1. Create `/etc/systemd/system/surfcam.service`:

```ini
[Unit]
Description=SurfCam Weather Pi
After=network.target

[Service]
Type=simple
ExecStartPre=/home/ryanpatton/script.sh
ExecStart=/usr/local/bin/surfcam
Restart=always
RestartSec=10
User=ryanpatton

[Install]
WantedBy=multi-user.target
```

2. Start it up:

```bash
sudo systemctl daemon-reload
sudo systemctl enable surfcam.service
sudo systemctl start surfcam.service
```

3. See if it's running:

```bash
sudo systemctl status surfcam.service
```

### Running manually

The `script.sh` file does a few helpful things:

- Sets up environment variables
- Waits for internet connection
- Waits for the camera to be ready
- Kills any old instances
- Sends output to a log file

To run it:

```bash
chmod +x script.sh
./script.sh
```

## How it works

### Normal operation

It runs three threads that keep things going:

1. **Snapshot thread**: Takes photos and uploads them
2. **Stream check thread**: Looks for streaming requests and handles streams
3. **Monitor thread**: Keeps an eye on memory and temperature

### Streaming (HLS → S3)

When someone wants to watch:

1. Polls the API (`GET …/check-streaming-requested`) and keeps going for `STREAM_TIMEOUT` after the last time the API returned `stream_requested: true`
2. Clears the local HLS directory and starts **libcamera → GStreamer** with **`hlssink`** (H.264 + MPEG-TS segments)
3. **`HlsUploader`** uploads each closed `.ts` file, then an updated **`index.m3u8`**, using **`POST .../hls/presign`** + **HTTPS PUT** (see [AWS_HLS_OPTION_B.md](AWS_HLS_OPTION_B.md))
4. Stops when requests end; your **CloudFront** (or public bucket URL) serves the playlist to browsers

### Keeping things stable

Since the Pi Zero is small, the app watches out for problems:

- **Memory**: Checks every 30 seconds; if free memory is critically low, it **signals** the stream-control thread to stop HLS (no direct `join` from the monitor). If `/proc/meminfo` cannot be read, that cycle’s metrics are skipped (not treated as zero).
- **Temperature**: Same pattern for high CPU temp via `/sys/class/thermal/thermal_zone0/temp`; missing thermal file skips that cycle’s reading
- **Recovery**: Tries to fix itself if the camera or network has issues
- **HLS window**: `hlssink` keeps only a small number of local segment files (`HLS_PLAYLIST_MAX_FILES`)

## Troubleshooting

### Camera not working?

```bash
# See if the camera shows up
libcamera-hello --list-cameras

# Try taking a test photo
libcamera-still -o test.jpg
```

### Build problems

```bash
# Clean everything and rebuild
rm -rf build/*
cmake ..
make clean
make
```

### Something's not working?

Check the logs:

```bash
tail -f /home/ryanpatton/surfcam.log
```

Or if running as a service:

```bash
journalctl -u surfcam.service -f
```

### Network problems

```bash
# Test if you can reach the API (expect 401/405 without a real multipart body)
curl -X POST https://treblesurf.com/api/upload-snapshot
```

### Streaming not working?

- Implement **`POST …/hls/presign`** (or your configured `HLS_PRESIGN_PATH`) on your backend and return a valid S3 presigned **PUT** URL (see [AWS_HLS_OPTION_B.md](AWS_HLS_OPTION_B.md))
- Install **`gstreamer1.0-plugins-bad`** so `hlssink` / `hlssink2` exists (`gst-inspect-1.0 hlssink`)
- Ensure the Pi can reach your API and S3 over HTTPS
- Check if the Pi is overheating

## Pi Zero tweaks

Since the Pi Zero is pretty slow, the defaults lean conservative:

- Lower video quality (400Kbps bitrate in GStreamer)
- Short HLS segment target duration and a small rolling file count
- Longer timeouts for slow internet on uploads
- Single-threaded **x264enc** when that encoder is used; hardware encoders use their own bitrate property

## API endpoints

Paths are relative to **`API_ENDPOINT`** (e.g. `https://treblesurf.com/api`). The code uses:

- **`POST {API_ENDPOINT}/upload-snapshot`**: Upload a photo

  - Body: multipart/form-data with file, timestamp, spot_id
  - Headers: `Authorization: ApiKey <key>`

- **`GET {API_ENDPOINT}/check-streaming-requested?spot_id=<id>`**: Check if someone wants to stream

  - Returns JSON with boolean `stream_requested`

- **`POST {API_ENDPOINT}{HLS_PRESIGN_PATH}`** (default `{API_ENDPOINT}/hls/presign`): Get a presigned S3 PUT URL for one HLS object
  - Body: JSON `{"key":"spots/<spot_id>/live/segment-00001.ts","content_type":"video/mp2t"}` (or `application/vnd.apple.mpegurl` for the playlist)
  - Response: JSON `{"url":"https://...","headers":{}}` — optional `headers` object must be sent on the PUT if your signer requires it

## License

GNU Affero General Public License v3.0. See [LICENSE](LICENSE) for details.

## Author

Ryan Patton
