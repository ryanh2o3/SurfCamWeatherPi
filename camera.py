import requests
import base64
import json
import time
import io
from PIL import Image
import os
import subprocess
import boto3
import threading
from datetime import datetime
import signal
import sys

# Configuration
API_ENDPOINT = "https://treblesurf.com/api"
API_KEY = os.environ.get("API_KEY", "REQUIRES_API_KEY_ENV_VAR")
SNAPSHOT_INTERVAL = 30  # seconds
STREAM_CHECK_INTERVAL = 5  # seconds
AWS_REGION = "eu-west-1"
KINESIS_STREAM_NAME = "treblesurf-webcam"
IMAGE_PATH = "/home/ryanpatton/image.jpg"

# Global variables
streaming_process = None
keep_running = True
last_stream_request_time = 0
stream_timeout = 30  # seconds to keep streaming without new requests

def take_picture():
    """Take a picture and send it to the backend"""
    try:
        # Use libcamera-jpeg to capture an image
        subprocess.run(['libcamera-jpeg', '-o', IMAGE_PATH], check=True)
    except FileNotFoundError:
        print("Failed to run libcamera-jpeg command. Is it installed and in the system's PATH?")
        return

    try:
        image = Image.open(IMAGE_PATH)
    except FileNotFoundError:
        print(f"Failed to open {IMAGE_PATH}. Did the libcamera-jpeg command succeed?")
        return

    image_file_object = io.BytesIO()
    image.save(image_file_object, format='JPEG')
    image_file_object.seek(0)
    image_bytes = image_file_object.getvalue()

    # Upload to backend
    files = {'file': ('snapshot.jpg', image_bytes, 'image/jpeg')}
    headers = {'Authorization': f'ApiKey {API_KEY}'}
    data = {'timestamp': datetime.now().isoformat(), 'spot_id': "Ireland_Donegal_Ballymastocker"}

    try:
        response = requests.post(f"{API_ENDPOINT}/upload-snapshot", 
                               files=files, 
                               data=data,
                               headers=headers)
        
        if response.status_code == 200:    
            print(f"[{datetime.now()}] Snapshot uploaded successfully!")
        else:
            print(f"[{datetime.now()}] Failed to upload snapshot. Status code: {response.status_code}")
    except Exception as e:
        print(f"[{datetime.now()}] Error uploading snapshot: {str(e)}")

def check_streaming_requested():
    """Check if streaming is requested from the backend"""
    global last_stream_request_time
    
    headers = {'Authorization': f'ApiKey {API_KEY}'}
    
    try:
        response = requests.get(f"{API_ENDPOINT}/check-streaming-requested?spot_id=Ireland_Donegal_Ballymastocker", headers=headers)
        
        if response.status_code == 200:
            data = response.json()
            if data.get('streaming_requested', False):
                print(f"[{datetime.now()}] Streaming requested!")
                last_stream_request_time = time.time()
                return True
    except Exception as e:
        print(f"[{datetime.now()}] Error checking streaming status: {str(e)}")
    
    # Check if streaming should continue based on timeout
    if time.time() - last_stream_request_time < stream_timeout:
        return True
        
    return False

def start_kinesis_stream():
    """Start streaming to AWS Kinesis"""
    global streaming_process
    
    if streaming_process is not None:
        # Stream already running
        return
    
    try:
        # Get streaming credentials
        headers = {"Authorization": f"ApiKey {API_KEY}"}
        response = requests.get(f"{API_ENDPOINT}/streaming-credentials", headers=headers)
        
        if response.status_code != 200:
            print(f"[{datetime.now()}] Failed to get streaming credentials")
            return
            
        credentials = response.json()
        
        # Configure GStreamer pipeline
        pipeline = (
            f"raspivid -n -t 0 -w 1280 -h 720 -fps 30 -b 2000000 -o - | "
            f"gst-launch-1.0 fdsrc ! h264parse ! "
            f"kvssink stream-name={KINESIS_STREAM_NAME} "
            f"aws-region={AWS_REGION} "
            f"access-key={credentials['accessKey']} "
            f"secret-key={credentials['secretKey']} "
            f"session-token={credentials['sessionToken']}"
        )
        
        print(f"[{datetime.now()}] Starting stream...")
        streaming_process = subprocess.Popen(pipeline, shell=True)
        
    except Exception as e:
        print(f"[{datetime.now()}] Error starting stream: {str(e)}")

def stop_kinesis_stream():
    """Stop the Kinesis stream"""
    global streaming_process
    
    if streaming_process is None:
        return
        
    try:
        print(f"[{datetime.now()}] Stopping stream...")
        streaming_process.terminate()
        streaming_process = None
    except Exception as e:
        print(f"[{datetime.now()}] Error stopping stream: {str(e)}")

def snapshot_worker():
    """Worker thread for taking periodic snapshots"""
    while keep_running:
        take_picture()
        time.sleep(SNAPSHOT_INTERVAL)

def streaming_worker():
    """Worker thread for managing streaming"""
    global streaming_process
    
    while keep_running:
        streaming_requested = check_streaming_requested()
        
        if streaming_requested and streaming_process is None:
            start_kinesis_stream()
        elif not streaming_requested and streaming_process is not None:
            stop_kinesis_stream()
            
        time.sleep(STREAM_CHECK_INTERVAL)

def signal_handler(sig, frame):
    """Handle Ctrl+C and other termination signals"""
    global keep_running
    print("\nShutting down gracefully...")
    keep_running = False
    if streaming_process is not None:
        stop_kinesis_stream()
    sys.exit(0)

def main():
    """Main function to start the threads"""
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    
    print(f"[{datetime.now()}] Starting SurfCam Weather Pi")
    print(f"[{datetime.now()}] Taking snapshots every {SNAPSHOT_INTERVAL} seconds")
    print(f"[{datetime.now()}] Checking for streaming requests every {STREAM_CHECK_INTERVAL} seconds")
    
    # Start the threads
    snapshot_thread = threading.Thread(target=snapshot_worker)
    streaming_thread = threading.Thread(target=streaming_worker)
    
    snapshot_thread.daemon = True
    streaming_thread.daemon = True
    
    snapshot_thread.start()
    streaming_thread.start()
    
    # Keep the main thread alive
    try:
        while keep_running:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nExiting...")
        keep_running = False

if __name__ == "__main__":
    main()