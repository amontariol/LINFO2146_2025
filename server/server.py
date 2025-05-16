import socket
import argparse
import sys
import time
import threading
import numpy as np
from datetime import datetime

MAX_SENSOR_READINGS = 30
SLOPE_THRESHOLD = 5.0
VALVE_DURATION = 600

class SensorData:
    def __init__(self, sensor_id):
        self.sensor_id = sensor_id
        self.readings = []
        self.timestamps = []
        self.last_update = time.time()
        self.valve_open = False
        self.valve_open_time = 0

    def add_reading(self, value, timestamp):
        self.readings.append(value)
        self.timestamps.append(timestamp)
        self.last_update = time.time()
        if len(self.readings) > MAX_SENSOR_READINGS:
            self.readings.pop(0)
            self.timestamps.pop(0)
    
    def calculate_slope(self):
        if len(self.readings) < 2:
            return 0
        x = np.array(range(len(self.readings)))
        y = np.array(self.readings)
        n = len(x)
        slope = (n * np.sum(x * y) - np.sum(x) * np.sum(y)) / (n * np.sum(x**2) - np.sum(x)**2)
        return slope


class Server:
    def __init__(self, ip, port):
        self.ip = ip
        self.port = port
        self.socket = None
        self.sensors = {}
        self.running = False
    
    def start(self):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        
        try:
            print(f"Connecting to border router at {self.ip}:{self.port}...")
            self.socket.connect((self.ip, self.port))
            print("Connected successfully!")
    
            self.running = True
            valve_thread = threading.Thread(target=self.check_valves)
            valve_thread.daemon = True
            valve_thread.start()
            
            status_thread = threading.Thread(target=self.print_status_periodically)
            status_thread.daemon = True
            status_thread.start()

            self.handle_messages()
            
        except Exception as e:
            print(f"Error connecting to border router: {e}")
            if self.socket:
                self.socket.close()
    
    def handle_messages(self):
        buffer = ""
        while self.running:
            try:
                data = self.socket.recv(1024).decode('utf-8')
                if not data:
                    print("Connection closed by border router")
                    break
                
                buffer += data
                lines = buffer.split('\n')
                buffer = lines.pop()
                
                for line in lines:
                    if line.strip():
                        self.process_message(line.strip())
            except Exception as e:
                print(f"Error handling messages: {e}")
                break
        
        if self.socket:
            self.socket.close()
            self.socket = None
    
    def process_message(self, message):
        print(f"Received: {message}")
        
        if message.startswith("DATA "):
            parts = message.split()
            if len(parts) >= 3:
                try:
                    if len(parts) >= 4:
                        sensor_id = int(parts[1])
                        value = int(parts[2])
                        timestamp = int(parts[3])
                    else:
                        sensor_id = int(parts[1])
                        value = int(parts[2])
                        timestamp = int(time.time())

                    if sensor_id not in self.sensors:
                        self.sensors[sensor_id] = SensorData(sensor_id)
                    
                    self.sensors[sensor_id].add_reading(value, timestamp)
                    print(f"Received data from sensor {sensor_id}: {value}")
                    
                    if len(self.sensors[sensor_id].readings) >= 2:
                        slope = self.sensors[sensor_id].calculate_slope()
                        print(f"Calculated slope for sensor {sensor_id}: {slope:.2f}")

                        if slope > SLOPE_THRESHOLD and not self.sensors[sensor_id].valve_open:
                            self.send_command(sensor_id, 1)
                            self.sensors[sensor_id].valve_open = True
                            self.sensors[sensor_id].valve_open_time = time.time()
                            print(f"Opening valve for sensor {sensor_id} for {VALVE_DURATION} seconds")
                except Exception as e:
                    print(f"Error processing data message: {e}")
    
    def send_command(self, sensor_id, command):
        if self.socket:
            message = f"COMMAND {sensor_id} {command}\n"
            self.socket.sendall(message.encode('utf-8'))
            print(f"Sent command: {message.strip()}")
    
    def check_valves(self):
        while self.running:
            current_time = time.time()
            
            for sensor_id, sensor in list(self.sensors.items()):
                if sensor.valve_open and current_time - sensor.valve_open_time >= VALVE_DURATION:
                    self.send_command(sensor_id, 0)
                    sensor.valve_open = False
                    print(f"Closing valve for sensor {sensor_id} (timer expired)")
            
            time.sleep(1)
    
    def print_status_periodically(self):
        while self.running:
            time.sleep(60)
            self.print_network_status()
    
    def stop(self):
        self.running = False
        if self.socket:
            self.socket.close()

    def print_network_status(self):
        """Print current network status"""
        print("\n=== Network Status ===")
        print(f"Connected sensors: {len(self.sensors)}")
        for sensor_id, sensor in self.sensors.items():
            valve_status = "OPEN" if sensor.valve_open else "CLOSED"
            readings = sensor.readings[-5:] if sensor.readings else []
            print(f"Sensor {sensor_id}: Valve {valve_status}, Last 5 readings: {readings}")
        print("=====================\n")


def main():
    parser = argparse.ArgumentParser(description="Building Management Server")
    parser.add_argument("--ip", dest="ip", type=str, default="localhost", help="IP address of the border router")
    parser.add_argument("--port", dest="port", type=int, default=60001, help="Port of the border router")
    args = parser.parse_args()
    
    server = Server(args.ip, args.port)
    
    try:
        server.start()
    except KeyboardInterrupt:
        print("Server shutting down...")
    finally:
        server.stop()

if __name__ == "__main__":
    main()
