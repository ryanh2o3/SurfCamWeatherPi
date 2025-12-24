# SurfCam Weather Pi

A Raspberry Pi app that captures surf conditions and streams live video. It takes photos every 30 seconds and can stream live video when someone requests it.

## What it does

This runs on a Raspberry Pi Zero W with a camera. It does two main things:

- **Takes snapshots**: Every 30 seconds it snaps a photo and uploads it to an API
- **Streams video**: When someone asks for it, it streams live video to AWS Kinesis

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
   - Gets AWS credentials when needed
   - Checks if anyone wants to watch a stream

3. **KinesisStreamer** (`src/KinesisStreamer.cpp`)

   - Connects to AWS Kinesis Video Streams
   - Encodes and uploads video frames
   - Breaks video into chunks that Kinesis can handle

4. **Main Application** (`src/main.cpp`)
   - Keeps everything working together
   - Runs threads for taking photos and streaming
   - Watches memory and temperature
   - Shuts down cleanly when needed

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
- GStreamer 1.0
- AWS C++ SDK
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

# GStreamer
sudo apt install -y libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev

# HTTP client
sudo apt install -y libcurl4-openssl-dev

# AWS SDK
# You'll need to follow the AWS C++ SDK installation guide:
# https://github.com/aws/aws-sdk-cpp/blob/master/Docs/CMake_Parameters.md

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

Settings are in `include/Config.h`. You can change:

```cpp
- SNAPSHOT_INTERVAL: How often to take photos (default: 30 seconds)
- STREAM_CHECK_INTERVAL: How often to check if someone wants to stream (default: 5 seconds)
- CAMERA_WIDTH/HEIGHT: Video size (default: 1280x720)
- STREAM_FPS: Video frame rate (default: 15 fps)
- API_ENDPOINT: Where to send photos
- KINESIS_STREAM_NAME: AWS Kinesis stream name
- AWS_REGION: AWS region
```

### Environment Variables

- `API_KEY`: Your API key (set in `script.sh` or as an environment variable)
- `SPOT_ID`: The identifier for your surf spot (e.g., "Ireland_Donegal_Ballymastocker")

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

### Streaming

When someone wants to watch:

1. Gets AWS credentials from the API
2. Connects to Kinesis Video Streams
3. Starts recording video with H.264 encoding
4. Sends video chunks to Kinesis
5. Stops automatically after 30 seconds if no one's watching

### Keeping things stable

Since the Pi Zero is small, the app watches out for problems:

- **Memory**: Checks every 30 seconds and stops streaming if memory gets too low
- **Temperature**: Watches CPU temp and stops streaming if it gets too hot
- **Recovery**: Tries to fix itself if the camera or network has issues
- **Fragment limits**: Keeps video chunks small so it doesn't run out of memory

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
# Test if you can reach the API
curl -X POST https://treblesurf.com/api/upload-snapshot
```

### Streaming not working?

- Make sure AWS credentials are correct
- Check the Kinesis stream exists in the right region
- Make sure your internet is fast enough
- Check if the Pi is overheating

## Pi Zero tweaks

Since the Pi Zero is pretty slow, I've made some adjustments:

- Smaller video chunks (256KB instead of the default)
- Less storage used (32MB instead of 128MB)
- Lower video quality (400Kbps bitrate)
- Longer timeouts for slow internet
- Less frame buffering
- Tries to prevent CPU throttling

## API endpoints

The app talks to these endpoints:

- `POST /api/upload-snapshot`: Upload a photo

  - Body: multipart/form-data with file, timestamp, spot_id
  - Headers: `Authorization: ApiKey <key>`

- `GET /api/is-streaming-requested?spot_id=<id>`: Check if someone wants to stream

  - Returns JSON saying yes or no

- `GET /api/get-streaming-credentials`: Get AWS credentials for streaming
  - Returns JSON with AWS credentials

## License

GNU Affero General Public License v3.0. See [LICENSE](LICENSE) for details.

## Author

Ryan Patton
