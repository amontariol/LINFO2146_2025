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
        x = np.array(range(len(self.readings)))  # Use indices as x values
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
        self.sensors = {}
        self.running = False
    
    def start(self):
        # Connect to Cooja's Serial Socket as a client
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        
        try:
            print(f"Connecting to border router at {self.ip}:{self.port}...")
            self.socket.connect((self.ip, self.port))
            print("Connected successfully!")
            
            self.running = True
            
            # Start valve check thread
            valve_thread = threading.Thread(target=self.check_valves)
            valve_thread.daemon = True
            valve_thread.start()
            
            # Handle messages from the border router
            self.handle_messages()
            
        except Exception as e:
            print(f"Error connecting to border router: {e}")
            if self.socket:
                self.socket.close()
    
    def handle_messages(self):
        # Handle messages from the border router
        while self.running:
            try:
                data = self.receive_line()
                if not data:
                    print("Connection closed by border router")
                    break
                
                self.process_message(data)
            except Exception as e:
                print(f"Error handling messages: {e}")
                break
        
        # Close connection
        if self.socket:
            self.socket.close()
            self.socket = None
    
    def receive_line(self):
        # Read a line from the socket
        buffer = b""
        try:
            while self.running:
                char = self.socket.recv(1)
                if not char:
                    return None
                
                if char == b'\n':
                    return buffer.decode('utf-8')
                
                buffer += char
        except socket.timeout:
            return None
        except ConnectionResetError:
            print("Connection reset by border router")
            return None
    
    def process_message(self, message):
        # Parse and process messages from the border router
        print(f"Received: {message}")
        parts = message.strip().split()
        
        if not parts:
            return
        
        if parts[0] == "DATA" and len(parts) >= 4:
            # Format: DATA sensor_id value timestamp
            try:
                sensor_id = int(parts[1])
                value = int(parts[2])
                timestamp = int(time.time())  # Use current time as timestamp
                
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
                        self.send_command(sensor_id, 1)
                        self.sensors[sensor_id].valve_open = True
                        self.sensors[sensor_id].valve_open_time = time.time()
                        print(f"Opening valve for sensor {sensor_id} for {VALVE_DURATION} seconds")
            except Exception as e:
                print(f"Error processing data message: {e}")
        
        elif parts[0] == "ENERGY" and len(parts) >= 3:
            # Format: ENERGY node_id energy_level
            try:
                node_id = int(parts[1])
                energy = int(parts[2])
                print(f"Energy level for node {node_id}: {energy}")
            except Exception as e:
                print(f"Error processing energy message: {e}")
    
    def send_command(self, sensor_id, command):
        # Send command to open/close valve
        if self.socket:
            message = f"COMMAND {sensor_id} {command}\n"
            self.socket.send(message.encode('utf-8'))
            print(f"Sent command: {message.strip()}")
    
    def check_valves(self):
        # Periodically check if valves should be closed
        while self.running:
            current_time = time.time()
            
            for sensor_id, sensor in self.sensors.items():
                if sensor.valve_open and current_time - sensor.valve_open_time >= VALVE_DURATION:
                    # Close valve
                    self.send_command(sensor_id, 0)
                    sensor.valve_open = False
                    print(f"Closing valve for sensor {sensor_id} (timer expired)")
            
            time.sleep(1)
    
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
    parser.add_argument("--ip", dest="ip", type=str, default="localhost",
                        help="IP address of the border router")
    parser.add_argument("--port", dest="port", type=int, default=60001,
                        help="Port of the border router")
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
