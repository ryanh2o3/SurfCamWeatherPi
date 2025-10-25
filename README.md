# SurfCam Weather Pi

A Raspberry Pi application that captures surf conditions at Ballymastocker Bay, Ireland, and streams live video to AWS Kinesis Video Streams while periodically uploading still snapshots.

## Overview

SurfCam Weather Pi is designed to run on a Raspberry Pi Zero W (or compatible) with a camera module. It continuously captures surf conditions and provides two modes of operation:

- **Snapshot Mode**: Takes still images every 30 seconds and uploads them to a remote API
- **Live Streaming**: On-demand live video streaming to AWS Kinesis Video Streams when requested via the API

The application is optimized for the resource-constrained Raspberry Pi Zero, with features like:

- Memory management and monitoring
- CPU temperature monitoring
- Automatic stream timeout handling
- GStreamer H.264 encoding pipeline
- Thread-safe camera management

## Architecture

The application is built in C++17 and consists of several key components:

### Components

1. **CameraManager** (`src/CameraManager.cpp`)

   - Manages camera initialization and control using libcamera
   - Handles both still capture and video mode
   - Manages GStreamer pipeline for H.264 encoding
   - Thread-safe buffer management

2. **ApiClient** (`src/ApiClient.cpp`)

   - Handles HTTP communication with the remote API
   - Manages snapshot uploads
   - Retrieves streaming credentials
   - Checks for streaming requests

3. **KinesisStreamer** (`src/KinesisStreamer.cpp`)

   - Manages AWS Kinesis Video Streams connection
   - Handles video frame encoding and upload
   - Manages temporal fragmentation of video streams

4. **Main Application** (`src/main.cpp`)
   - Coordinates all components
   - Manages worker threads for snapshots and streaming
   - Monitors system resources (memory and temperature)
   - Handles graceful shutdown

## Dependencies

### System Requirements

- Raspberry Pi Zero W (or compatible)
- Raspberry Pi Camera Module V2 or V3
- Raspbian OS or similar Linux distribution
- Network connectivity

### Build Dependencies

- CMake 3.10 or higher
- C++17 compatible compiler (gcc or clang)
- OpenCV (for image processing)
- libcamera (camera interface)
- GStreamer 1.0 (H.264 encoding)
- AWS C++ SDK (Kinesis Video Streams)
- libcurl (HTTP client)
- pthread

### Installation on Raspberry Pi

```bash
# Update system
sudo apt update && sudo apt upgrade -y

# Install build tools
sudo apt install -y build-essential cmake git pkg-config

# Install camera libraries
sudo apt install -y libcamera-dev libcamera-apps

# Install OpenCV
sudo apt install -y libopencv-dev

# Install GStreamer
sudo apt install -y libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev

# Install curl
sudo apt install -y libcurl4-openssl-dev

# Install AWS SDK dependencies
# (Follow AWS C++ SDK installation instructions)
# See: https://github.com/aws/aws-sdk-cpp/blob/master/Docs/CMake_Parameters.md

# Install nlohmann/json
sudo apt install -y nlohmann-json3-dev
```

## Building

```bash
# Clone the repository (if not already done)
cd /path/to/SurfCamWeatherPi

# Create build directory
mkdir build && cd build

# Configure with CMake
cmake ..

# Build
make -j4

# Install (optional)
sudo make install
```

## Configuration

Configuration is managed in `include/Config.h`. Key settings include:

```cpp
- SNAPSHOT_INTERVAL: How often to take snapshots (default: 30 seconds)
- STREAM_CHECK_INTERVAL: How often to check for streaming requests (default: 5 seconds)
- CAMERA_WIDTH/HEIGHT: Video resolution (default: 1280x720)
- STREAM_FPS: Frame rate for streaming (default: 15 fps)
- API_ENDPOINT: Base URL for the API
- KINESIS_STREAM_NAME: AWS Kinesis stream name
- AWS_REGION: AWS region
- SPOT_ID: Surf spot identifier
```

### Environment Variables

- `API_KEY`: API authentication key (set in `script.sh` or as environment variable)

## Deployment

### Systemd Service

1. Create a service file at `/etc/systemd/system/surfcam.service`:

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

2. Enable and start the service:

```bash
sudo systemctl daemon-reload
sudo systemctl enable surfcam.service
sudo systemctl start surfcam.service
```

3. Check status:

```bash
sudo systemctl status surfcam.service
```

### Manual Startup Script

The included `script.sh` handles:

- Setting environment variables
- Waiting for network connectivity
- Waiting for camera availability
- Killing any existing instances
- Redirecting output to log file

To use it manually:

```bash
chmod +x script.sh
./script.sh
```

## Operation

### Normal Operation

The application runs continuously with three main threads:

1. **Snapshot Thread**: Periodically captures and uploads still images
2. **Stream Check Thread**: Monitors for streaming requests and manages stream lifecycle
3. **Monitor Thread**: Watches system resources and temperature

### Streaming

When a streaming request is received from the API:

1. Application fetches AWS credentials from the API
2. Initializes Kinesis Video Streams connection
3. Starts video capture mode with H.264 encoding
4. Streams video in fragments to Kinesis
5. Automatically stops after 30 seconds of inactivity

### Resource Management

The application includes several resource management features:

- **Memory Monitoring**: Checks available memory every 30 seconds and can shut down streaming if memory is critically low
- **Temperature Monitoring**: Monitors CPU temperature and can shut down streaming if overheating is detected
- **Automatic Recovery**: Attempts to recover from camera failures and network issues
- **Fragment Size Limits**: Limits video fragment size to prevent memory exhaustion

## Troubleshooting

### Camera Not Detected

```bash
# Check if camera is detected
libcamera-hello --list-cameras

# Test camera capture
libcamera-still -o test.jpg
```

### Build Issues

```bash
# Clean build directory
rm -rf build/*
cmake ..
make clean
make
```

### Runtime Issues

Check logs:

```bash
tail -f /home/ryanpatton/surfcam.log
```

Or systemd logs:

```bash
journalctl -u surfcam.service -f
```

### Network Issues

```bash
# Test API connectivity
curl -X POST https://treblesurf.com/api/upload-snapshot
```

### Streaming Issues

- Verify AWS credentials are valid
- Check Kinesis stream exists in the correct region
- Ensure network bandwidth is sufficient
- Check Pi Zero isn't overheating

## Performance Optimization

The application is optimized specifically for Raspberry Pi Zero:

- Reduced fragment sizes (256KB vs default)
- Reduced storage size (32MB vs 128MB)
- Lower bitrate encoding (400Kbps)
- Increased timeouts for slow network
- Reduced frame buffering
- CPU throttling prevention

## API Integration

### Endpoints Used

- `POST /api/upload-snapshot`: Upload still image

  - Body: multipart/form-data with file, timestamp, spot_id
  - Headers: `Authorization: ApiKey <key>`

- `GET /api/is-streaming-requested?spot_id=<id>`: Check if streaming requested

  - Returns JSON with streaming status

- `GET /api/get-streaming-credentials`: Get AWS credentials for streaming
  - Returns JSON with AWS credentials

## License

[Add your license here]

## Author

Ryan Patton

## Acknowledgments

- Raspberry Pi Foundation for the camera module and libcamera
- AWS for Kinesis Video Streams
- OpenCV community
- GStreamer developers
