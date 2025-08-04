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
    """Function to capture video and stream to Kinesis"""
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
        
        # Set up Kinesis client with provided credentials
        kvs_client = boto3.client(
            'kinesisvideo',
            region_name=AWS_REGION,
            aws_access_key_id=credentials['accessKey'],
            aws_secret_access_key=credentials['secretKey'],
            aws_session_token=credentials['sessionToken']
        )
        
        # Get an endpoint for the PutMedia API
        endpoint_response = kvs_client.get_data_endpoint(
            StreamName=KINESIS_STREAM_NAME,
            APIName='PUT_MEDIA'
        )
        
        endpoint = endpoint_response['DataEndpoint']
        
        # Create a client for PutMedia API
        kvs_media_client = boto3.client(
            'kinesis-video-media',
            region_name=AWS_REGION,
            aws_access_key_id=credentials['accessKey'],
            aws_secret_access_key=credentials['secretKey'],
            aws_session_token=credentials['sessionToken'],
            endpoint_url=endpoint
        )
        
        # Initialize video encoding with OpenCV and H.264
        import cv2
        fourcc = cv2.VideoWriter_fourcc(*'avc1')  # H.264 codec
        fps = 30
        width, height = 1280, 720
        
        # Create temporary file for the encoded video fragment
        import tempfile
        temp_file = tempfile.NamedTemporaryFile(delete=False, suffix='.mp4')
        
        # Create VideoWriter to encode frames
        video_writer = cv2.VideoWriter(
            temp_file.name,
            fourcc,
            fps,
            (width, height)
        )
        
        # Stream parameters
        fragment_duration = 2  # seconds per fragment
        frames_per_fragment = int(fps * fragment_duration)
        frame_count = 0
        
        start_timestamp = datetime.now().timestamp() * 1000  # Convert to milliseconds
        
        print(f"[{datetime.now()}] Starting to stream to Kinesis Video Stream: {KINESIS_STREAM_NAME}")
        
        while keep_running:
            # Capture frame from camera
            frame = picam2.capture_array()
            
            # OpenCV expects BGR format, but Picamera2 gives RGB
            frame_bgr = cv2.cvtColor(frame, cv2.COLOR_RGB2BGR)
            
            # Write the frame to the video file
            video_writer.write(frame_bgr)
            
            frame_count += 1
            
            # When we have enough frames for a fragment, send it to Kinesis
            if frame_count >= frames_per_fragment:
                # Release and close the video writer
                video_writer.release()
                temp_file.close()
                
                # Read the encoded video data
                with open(temp_file.name, 'rb') as f:
                    encoded_data = f.read()
                
                # Calculate timestamps
                current_time = datetime.now().timestamp() * 1000  # Convert to milliseconds
                
                try:
                    # Send the fragment to Kinesis Video Streams
                    kvs_media_client.put_media(
                        StreamName=KINESIS_STREAM_NAME,
                        Data=encoded_data,
                        ProducerTimestamp=int(current_time),
                        FragmentTimecode=str(int(current_time - start_timestamp))
                    )
                    print(f"[{datetime.now()}] Successfully sent fragment to Kinesis")
                except Exception as e:
                    print(f"[{datetime.now()}] Error sending fragment to Kinesis: {str(e)}")
                
                # Create a new temporary file for the next fragment
                temp_file = tempfile.NamedTemporaryFile(delete=False, suffix='.mp4')
                
                # Create a new VideoWriter for the next fragment
                video_writer = cv2.VideoWriter(
                    temp_file.name,
                    fourcc,
                    fps,
                    (width, height)
                )
                
                # Reset frame count
                frame_count = 0
            
            # Sleep to maintain frame rate
            time.sleep(1/fps)
        
        # Clean up
        if video_writer:
            video_writer.release()
        if temp_file:
            temp_file.close()
            os.unlink(temp_file.name)
            
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