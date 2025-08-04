import requests
import base64
import json
import time
import io
import os
import subprocess
import boto3
import threading
from datetime import datetime
import signal
import sys
from picamera2 import Picamera2

# Configuration
API_ENDPOINT = "https://treblesurf.com/api"
API_KEY = os.environ.get("API_KEY", "asd87f6ds7f6sdfsda5da4876fsdvg8atd7f5a9sd76f5sad6fgpgfbqw38g")
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
stream_thread = None  # Add this line

picam2 = None

def init_picamera2():
    global picam2
    if picam2 is None:
        picam2 = Picamera2()
        picam2.configure(picam2.create_still_configuration())
        picam2.start()

def take_picture():
    """Take a picture and send it to the backend using Picamera2"""
    global picam2
    try:
        init_picamera2()
        picam2.capture_file(IMAGE_PATH)
        
        # Read the image bytes
        with open(IMAGE_PATH, "rb") as f:
            image_bytes = f.read()
            
        # Upload to backend
        files = {'file': ('snapshot.jpg', image_bytes, 'image/jpeg')}
        headers = {'Authorization': f'ApiKey {API_KEY}'}
        data = {'timestamp': datetime.now().isoformat(), 'spot_id': "Ireland_Donegal_Ballymastocker"}

        response = requests.post(f"{API_ENDPOINT}/upload-snapshot", 
                               files=files, 
                               data=data,
                               headers=headers)
        
        if response.status_code == 200:    
            print(f"[{datetime.now()}] Snapshot uploaded successfully!")
        else:
            print(f"[{datetime.now()}] Failed to upload snapshot. Status code: {response.status_code}")
            print(response.json())
    except Exception as e:
        print(f"[{datetime.now()}] Error capturing or uploading snapshot: {str(e)}")

def check_streaming_requested():
    """Check if streaming is requested from the backend"""
    global last_stream_request_time
    
    headers = {'Authorization': f'ApiKey {API_KEY}'}
    
    try:
        response = requests.get(f"{API_ENDPOINT}/check-streaming-requested?spot_id=Ireland_Donegal_Ballymastocker", headers=headers)
        
        if response.status_code == 200:
            data = response.json()
            if data.get('stream_requested', False):
                print(f"[{datetime.now()}] Streaming requested!")
                last_stream_request_time = time.time()
                return True
    except Exception as e:
        print(f"[{datetime.now()}] Error checking streaming status: {str(e)}")
    
    # Check if streaming should continue based on timeout
    if time.time() - last_stream_request_time < stream_timeout:
        return True
        
    return False

def stream_to_kinesis(credentials):
    """Function to capture video and stream to Kinesis using the Producer SDK"""
    global picam2, keep_running
    try:
        # Reconfigure global picam2 for video streaming
        if picam2 is None:
            picam2 = Picamera2()
        picam2.stop()  # Stop if running in still mode
        video_config = picam2.create_video_configuration(main={"size": (1280, 720)})
        picam2.configure(video_config)
        picam2.start()
        
        print(f"[{datetime.now()}] Stream started with Picamera2")
        
        # Import the KVS Producer SDK
        from amazon_kinesis_video_streams_producer_sdk.producer import KvsProducer
        
        # Create a KVS Producer
        kvs_producer = KvsProducer(
            stream_name=KINESIS_STREAM_NAME,
            access_key=credentials['accessKey'],
            secret_key=credentials['secretKey'],
            session_token=credentials['sessionToken'],
            region=AWS_REGION
        )
        
        # Start the producer
        kvs_producer.start()
        
        # Stream parameters
        fps = 30
        frame_duration = int(1_000_000 / fps)  # Duration in microseconds
        
        print(f"[{datetime.now()}] Starting to stream to Kinesis Video Stream: {KINESIS_STREAM_NAME}")
        
        while keep_running:
            # Capture frame from camera
            frame = picam2.capture_array()
            
            # Convert to BGR format for OpenCV
            frame_bgr = cv2.cvtColor(frame, cv2.COLOR_RGB2BGR)
            
            # Encode frame to JPEG
            _, encoded_frame = cv2.imencode('.jpg', frame_bgr)
            frame_data = encoded_frame.tobytes()
            
            # Get current timestamp in microseconds
            timestamp = int(datetime.now().timestamp() * 1_000_000)
            
            # Send frame to KVS
            kvs_producer.put_frame(
                frame_data=frame_data,
                timestamp=timestamp,
                flags=0  # No flags
            )
            
            # Sleep to maintain frame rate
            time.sleep(1/fps)
        
        # Stop the producer
        kvs_producer.stop()
        
        picam2.stop()
        print(f"[{datetime.now()}] Stream stopped")
        
        # Reconfigure for stills after streaming ends
        picam2.configure(picam2.create_still_configuration())
        picam2.start()
        
    except Exception as e:
        print(f"[{datetime.now()}] Error in streaming thread: {str(e)}")
        import traceback
        traceback.print_exc()

def start_kinesis_stream():
    """Start streaming to AWS Kinesis using Picamera2"""
    global streaming_process, stream_thread
    
    if stream_thread is not None and stream_thread.is_alive():
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
        
        print(f"[{datetime.now()}] Starting stream with Picamera2...")
        
        # Start the streaming in a separate thread
        stream_thread = threading.Thread(target=stream_to_kinesis, args=(credentials,))
        stream_thread.daemon = True
        stream_thread.start()
        
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
    global keep_running
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