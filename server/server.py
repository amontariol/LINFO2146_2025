#!/usr/bin/env python3
# server.py

import socket
import argparse
import sys
import time
import threading
import numpy as np
from datetime import datetime

# Configuration
MAX_SENSOR_READINGS = 30
SLOPE_THRESHOLD = 5.0
VALVE_DURATION = 600  # 10 minutes in seconds

class SensorData:
    def __init__(self, sensor_id):
        self.sensor_id = sensor_id
        self.readings = []
        self.timestamps = []
        self.last_update = time.time()
        self.valve_open = False
        self.valve_open_time = 0

    def add_reading(self, value, timestamp):
        # Add new reading
        self.readings.append(value)
        self.timestamps.append(timestamp)
        self.last_update = time.time()
        
        # Keep only the last MAX_SENSOR_READINGS
        if len(self.readings) > MAX_SENSOR_READINGS:
            self.readings.pop(0)
            self.timestamps.pop(0)
    
    def calculate_slope(self):
        # Need at least 2 points for a line
        if len(self.readings) < 2:
            return 0
        
        # Convert to numpy arrays for calculation
        x = np.array(self.timestamps)
        y = np.array(self.readings)
        
        # Calculate slope using least squares
        n = len(x)
        slope = (n * np.sum(x * y) - np.sum(x) * np.sum(y)) / (n * np.sum(x**2) - np.sum(x)**2)
        return slope

class Server:
    def __init__(self, ip, port):
        self.ip = ip
        self.port = port
        self.socket = None
        self.client = None
        self.sensors = {}
        self.running = False
    
    def start(self):
        # Create socket
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket.bind((self.ip, self.port))
        self.socket.listen(1)
        
        self.running = True
        print(f"Server started on {self.ip}:{self.port}")
        
        # Start valve check thread
        valve_thread = threading.Thread(target=self.check_valves)
        valve_thread.daemon = True
        valve_thread.start()
        
        # Accept connections
        while self.running:
            try:
                self.client, addr = self.socket.accept()
                print(f"Connected to border router at {addr}")
                self.handle_client()
            except KeyboardInterrupt:
                self.running = False
                break
            except Exception as e:
                print(f"Error: {e}")
                time.sleep(1)
    
    def handle_client(self):
        # Handle messages from the border router
        while self.running:
            try:
                data = self.receive_line()
                if not data:
                    break
                
                self.process_message(data)
            except Exception as e:
                print(f"Error handling client: {e}")
                break
        
        # Close connection
        if self.client:
            self.client.close()
            self.client = None
    
    def receive_line(self):
        # Read a line from the socket
        buffer = b""
        while self.running:
            char = self.client.recv(1)
            if not char:
                return None
            
            if char == b'\n':
                return buffer.decode('utf-8')
            
            buffer += char
    
    def process_message(self, message):
        # Parse and process messages from the border router
        parts = message.strip().split()
        
        if not parts:
            return
        
        msg_type = parts[0]
        
        if msg_type == "DATA" and len(parts) >= 4:
            # Format: DATA sensor_id value timestamp
            sensor_id = int(parts[1])
            value = int(parts[2])
            timestamp = int(parts[3])
            
            # Get or create sensor data
            if sensor_id not in self.sensors:
                self.sensors[sensor_id] = SensorData(sensor_id)
            
            # Add reading
            self.sensors[sensor_id].add_reading(value, timestamp)
            print(f"Received data from sensor {sensor_id}: {value}")
            
            # Calculate slope and check if valve should be opened
            if len(self.sensors[sensor_id].readings) >= 2:
                slope = self.sensors[sensor_id].calculate_slope()
                print(f"Calculated slope for sensor {sensor_id}: {slope:.2f}")
                
                if slope > SLOPE_THRESHOLD and not self.sensors[sensor_id].valve_open:
                    # Open valve
                    self.send_command(sensor_id, 1, VALVE_DURATION)
                    self.sensors[sensor_id].valve_open = True
                    self.sensors[sensor_id].valve_open_time = time.time()
                    print(f"Opening valve for sensor {sensor_id} for {VALVE_DURATION} seconds")
        
        elif msg_type == "ENERGY" and len(parts) >= 3:
            # Format: ENERGY node_id energy_level
            node_id = int(parts[1])
            energy = int(parts[2])
            print(f"Energy level for node {node_id}: {energy}")
    
    def send_command(self, sensor_id, command, duration):
        # Send command to open/close valve
        if self.client:
            message = f"COMMAND {sensor_id} {command} {duration}\n"
            self.client.send(message.encode('utf-8'))
            print(f"Sent command: {message.strip()}")
    
    def check_valves(self):
        # Periodically check if valves should be closed
        while self.running:
            current_time = time.time()
            
            for sensor_id, sensor in self.sensors.items():
                if sensor.valve_open and current_time - sensor.valve_open_time >= VALVE_DURATION:
                    # Close valve
                    self.send_command(sensor_id, 0, 0)
                    sensor.valve_open = False
                    print(f"Closing valve for sensor {sensor_id} (timer expired)")
            
            time.sleep(1)
    
    def stop(self):
        self.running = False
        if self.client:
            self.client.close()
        if self.socket:
            self.socket.close()

def main():
    parser = argparse.ArgumentParser(description="Building Management Server")
    parser.add_argument("--ip", dest="ip", type=str, default="localhost",
                        help="IP address to bind to")
    parser.add_argument("--port", dest="port", type=int, default=60001,
                        help="Port to bind to")
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
