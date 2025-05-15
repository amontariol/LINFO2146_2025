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
SLOPE_THRESHOLD = 3.0  # Reduced threshold to trigger more often
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
        denominator = (n * np.sum(x**2) - np.sum(x)**2)
        if abs(denominator) < 0.0001:  # Avoid division by zero
            return 0
        slope = (n * np.sum(x * y) - np.sum(x) * np.sum(y)) / denominator
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
            
            # Start status thread
            status_thread = threading.Thread(target=self.print_status_periodically)
            status_thread.daemon = True
            status_thread.start()
            
            # Start test commands thread - send commands to all possible sensors
            test_thread = threading.Thread(target=self.send_test_commands)
            test_thread.daemon = True
            test_thread.start()
            
            # Handle messages from the border router
            self.handle_messages()
            
        except Exception as e:
            print(f"Error connecting to border router: {e}")
            if self.socket:
                self.socket.close()
    
    def handle_messages(self):
        # Handle messages from the border router
        buffer = b""
        while self.running:
            try:
                data = self.socket.recv(1)
                if not data:
                    print("Connection closed by border router")
                    break
                
                if data == b'\n':
                    self.process_message(buffer.decode('utf-8'))
                    buffer = b""
                else:
                    buffer += data
                    
            except Exception as e:
                print(f"Error handling messages: {e}")
                break
        
        # Close connection
        if self.socket:
            self.socket.close()
            self.socket = None
    
    def process_message(self, message):
        # Parse and process messages from the border router
        print(f"Received: {message}")
        
        # Check if it's a data message
        if message.startswith("DATA"):
            parts = message.strip().split()
            if len(parts) >= 6:
                try:
                    # Format: DATA 03 04 01 00 02 f5
                    # This is a data message with sensor reading
                    sensor_id = int(parts[2], 16)
                    value_high = int(parts[5], 16)
                    value_low = int(parts[6], 16) if len(parts) > 6 else 0
                    value = (value_high << 8) | value_low
                    
                    print(f"Parsed sensor data: sensor_id={sensor_id}, value={value}")
                    
                    # Get or create sensor data
                    if sensor_id not in self.sensors:
                        self.sensors[sensor_id] = SensorData(sensor_id)
                    
                    # Add reading
                    timestamp = int(time.time())
                    self.sensors[sensor_id].add_reading(value, timestamp)
                    print(f"Added reading {value} for sensor {sensor_id}")
                    
                    # Calculate slope and check if valve should be opened
                    if len(self.sensors[sensor_id].readings) >= 2:
                        slope = self.sensors[sensor_id].calculate_slope()
                        print(f"Calculated slope for sensor {sensor_id}: {slope:.2f}")
                        
                        if abs(slope) > SLOPE_THRESHOLD and not self.sensors[sensor_id].valve_open:
                            # Open valve
                            self.send_command(sensor_id, 1)
                            self.sensors[sensor_id].valve_open = True
                            self.sensors[sensor_id].valve_open_time = time.time()
                            print(f"Opening valve for sensor {sensor_id} (slope={slope:.2f})")
                except Exception as e:
                    print(f"Error processing data message: {e}")
        
        # Check if it's a light level message
        elif message.startswith("light"):
            parts = message.strip().split(',')
            if len(parts) == 3:
                try:
                    sensor_type, greenhouse, value = parts
                    value = int(value)
                    
                    # Extract sensor ID from greenhouse address
                    addr_parts = greenhouse.split(':')
                    if len(addr_parts) == 2:
                        sensor_id = int(addr_parts[0], 16)
                        
                        # Get or create sensor data
                        if sensor_id not in self.sensors:
                            self.sensors[sensor_id] = SensorData(sensor_id)
                        
                        # Add reading
                        timestamp = int(time.time())
                        self.sensors[sensor_id].add_reading(value, timestamp)
                        print(f"Added light reading {value} for sensor {sensor_id}")
                        
                        # Calculate slope and check if valve should be opened
                        if len(self.sensors[sensor_id].readings) >= 2:
                            slope = self.sensors[sensor_id].calculate_slope()
                            print(f"Calculated slope for sensor {sensor_id}: {slope:.2f}")
                            
                            if abs(slope) > SLOPE_THRESHOLD and not self.sensors[sensor_id].valve_open:
                                # Open valve
                                self.send_command(sensor_id, 1)
                                self.sensors[sensor_id].valve_open = True
                                self.sensors[sensor_id].valve_open_time = time.time()
                                print(f"Opening valve for sensor {sensor_id} (slope={slope:.2f})")
                except Exception as e:
                    print(f"Error processing light message: {e}")
    
    def send_command(self, sensor_id, command):
        if self.socket:
            message = f"COMMAND {sensor_id} {command}\n"
            self.socket.send(message.encode('utf-8'))
            print(f"Sent command: {message.strip()}")
            # Always track valve state
            if sensor_id not in self.sensors:
                self.sensors[sensor_id] = SensorData(sensor_id)
            if command == 1:
                self.sensors[sensor_id].valve_open = True
                self.sensors[sensor_id].valve_open_time = time.time()
            else:
                self.sensors[sensor_id].valve_open = False


    
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
    
    def print_status_periodically(self):
        # Print status every minute
        while self.running:
            self.print_network_status()
            time.sleep(60)
    
    def send_test_commands(self):
        # Send test commands to all possible sensors
        while self.running:
            if len(self.sensors) >= 10:
                break
            print("Waiting for sensors to connect...")
            self.print_network_status()
            time.sleep(1)
        
        # Send commands to all possible sensor IDs
        for sensor_id in range(4, 10):
            print(f"Sending test command to sensor {sensor_id}")
            self.send_command(sensor_id, 1)
            time.sleep(2)
        
        time.sleep(30)  # Wait before sending close commands
        
        for sensor_id in range(4, 10):
            self.send_command(sensor_id, 0)
            time.sleep(2)
        
        # Repeat periodically
        while self.running:
            time.sleep(300)  # Wait 5 minutes
            
            for sensor_id in range(4, 10):
                self.send_command(sensor_id, 1)
                time.sleep(2)
            
            time.sleep(30)
            
            for sensor_id in range(4, 10):
                self.send_command(sensor_id, 0)
                time.sleep(2)
    
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
            if len(sensor.readings) >= 2:
                slope = sensor.calculate_slope()
                print(f"  Current slope: {slope:.2f}")
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
