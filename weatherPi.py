import serial
from time import sleep
import requests
import time
import sys
import board
import adafruit_dht

class SurfWeatherCam:
    def run(self):
        halfData = ''
        completeData = ''

        DATA_INTEGRATOR_URL = 'https://dataintegrator-667754725634.europe-west1.run.app/localWeather'
        ser = serial.Serial ("/dev/ttyS0", 9600)    #Open port with baud rate
        dhtDevice = adafruit_dht.DHT11(board.D23)
        
        outsideTemp = 0
        outsideHumidity = 0

        while True:
            # Initial the dht device, with data pin connected to:


# you can pass DHT22 use_pulseio=False if you wouldn't like to use pulseio.
# This may be necessary on a Linux single board computer like the Raspberry Pi,
# but it will not work in CircuitPython.
# dhtDevice = adafruit_dht.DHT22(board.D18, use_pulseio=False)

            try:
                # Print the values to the serial port
                temperature_c = dhtDevice.temperature
                temperature_f = temperature_c * (9 / 5) + 32
                humidity = dhtDevice.humidity
                if temperature_f is not None:
                    outsideTemp = temperature_c
                if humidity is not None:
                    outsideHumidity = humidity

            except RuntimeError as error:
                # Errors happen fairly often, DHT's are hard to read, just keep going
                print(error.args[0])
                time.sleep(2.0)
                continue
            except Exception as error:
                dhtDevice.exit()
                raise error

            time.sleep(2.0)

            try:
                received_data = ser.read()              #read serial port
                sleep(0.03)
                data_left = ser.inWaiting()             #check for remaining byte
                received_data += ser.read(data_left)
                halfData += received_data.decode('utf-8')
                if halfData.find("\r\n") != -1:
                    completeData = halfData.split('\r\n')
                    halfData =''
                    completeData = completeData[0]
                    try:
                        a = completeData.split('A')
                        a = a[1].split('B')
                        a = a[0]
                        #print(a)
                        direction = completeData.split('B')
                        direction = direction[1].split('C')
                        direction = direction[0]
                        direction = int(direction)
                        direction = str(direction)
                        #print("Direction:" + direction)
        
                        speed = completeData.split('D')
                        speed = speed[1].split('E')
                        speed = speed[0]
                        speed = int(speed)
                        speed = speed * 0.36
                        speed = str(speed)

                        highSpeed5 = completeData.split('F')
                        highSpeed5 = highSpeed5[1].split('G')
                        highSpeed5 = highSpeed5[0]
                        highSpeed5 = int(highSpeed5)
                        highSpeed5 = highSpeed5 * 0.36
                        highSpeed5 = str(highSpeed5)
                        #print("Highest Speed in 5 mins: " + highSpeed5)
                        g = completeData.split('G')
                        g = g[1].split('H')
                        g = g[0]
                        #print(g)
                        h = completeData.split('H')
                        h = h[1].split('I')
                        h = h[0]
                        #print(h)
                        rainfallMin = completeData.split('I')
                        rainfallMin = rainfallMin[1].split('J')
                        rainfallMin = rainfallMin[0]
                        #print(i)
                        rainfallHour = completeData.split('J')
                        rainfallHour = rainfallHour[1].split('K')
                        rainfallHour = rainfallHour[0]
                        #print(j)
                        rainfall24 = completeData.split('K')
                        rainfall24 = rainfall24[1].split('L')
                        rainfall24 = rainfall24[0]
                        #print(k)
                        temperature = completeData.split('L')
                        temperature = temperature[1].split('M')
                        temperature = temperature[0]
                        temperature = int(temperature)
                        temperature = temperature/10
                        temperature = str(temperature)

                        humidity = completeData.split('M')
                        humidity = humidity[1].split('N')
                        humidity = humidity[0]
                        humidity = int(humidity)
                        humidity = humidity/10
                        humidity = str(humidity)
                        #print("Humidity:" + humidity)
                        pressure = completeData.split('N')
                        pressure = pressure[1].split('O')
                        pressure = pressure[0]
                        pressure = int(pressure)
                        pressure = pressure/10
                        pressure = str(pressure)
                        #print("Pressure:" + pressure)

                        data = { 'weather': {'direction': direction,  'speed': speed, 'highSpeed5': highSpeed5, 'temperature': temperature, 'humidity': humidity, 'pressure': pressure, 'rainfallMin': rainfallMin, 'rainfallHour': rainfallHour, 'rainfall24': rainfall24, 'outsideTemp': outsideTemp, 'outsideHumidity': outsideHumidity}, 'api_key': 'asd87f6ds7f6sdfsda5da4876fsdvg8atd7f5a9sd76f5sad6fgpgfbqw38g'}
                        
                        wait_time = 180
                        while True:
                                try:
                                    #print("Sending data to Home Assistant!")
                                    # Make an HTTP POST request to the Home Assistant webhook URL
                                    if data is not None:
                                        request = requests.Request('POST', DATA_INTEGRATOR_URL, json=data)
                                        prepared_request = request.prepare()

                                        # Calculate the size of the request
                                        body_size = len(prepared_request.body or b'')
                                        headers_size = sum(len(k) + len(v) for k, v in prepared_request.headers.items())
                                        total_size = body_size + headers_size

                                        # print(f"Total HTTP POST request size: {total_size} bytes")

                                        # Send the request
                                        response = requests.Session().send(prepared_request)

                                    if response.status_code == 200:
                                        #print("Data sent successfully!")
                                        wait_time = 180
                                        break
                                    else:
                                        print(f"Failed to send data. Status code: {response.status_code}")
                                except:
                                    print('Unknown exception. Request failed and script terminates.')                  
                                    time.sleep(wait_time)
                                    wait_time = wait_time + 180
                    except IndexError:
                        print("Error")
                        continue
                    
                sleep(1)
            except KeyboardInterrupt:
                print("error occured")
                break

if __name__ == "__main__":        
    surf_weather_cam = SurfWeatherCam()
    surf_weather_cam.run()