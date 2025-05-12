/* computation-node.c */
#include "contiki.h"
#include "net/nullnet/nullnet.h"
#include "net/netstack.h"
#include "lib/random.h"
#include "sys/log.h"
#include "message-format.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define LOG_MODULE "ComputationNode"
#define LOG_LEVEL LOG_LEVEL_INFO

/* Configuration */
#define COMPUTATION_NODE_ID    (linkaddr_node_addr.u8[0])
#define BROADCAST_CHANNEL      129
#define DATA_CHANNEL           130
#define DISCOVERY_INTERVAL     (CLOCK_SECOND * 60)
#define MAX_SENSOR_NODES       5
#define MAX_SENSOR_READINGS    30
#define SLOPE_THRESHOLD        5.0  /* Adjust based on your needs */
#define CLEANUP_INTERVAL       (CLOCK_SECOND * 300)  /* 5 minutes */
#define INACTIVE_THRESHOLD     (CLOCK_SECOND * 600)  /* 10 minutes */

/* Sensor data structure */
typedef struct {
  uint16_t sensor_id;
  uint16_t readings[MAX_SENSOR_READINGS];
  uint32_t timestamps[MAX_SENSOR_READINGS];
  uint8_t count;
  uint32_t last_update;
  uint8_t active;
} sensor_data_t;

/* Global variables */
static uint16_t parent_id = 0xFFFF;
static uint8_t hop_to_root = 0xFF;
static uint16_t energy_level = 1000;
static sensor_data_t sensors[MAX_SENSOR_NODES];
static uint8_t sensor_count = 0;

/* Process definitions */
PROCESS(computation_node_process, "Computation Node Process");
PROCESS(discovery_process, "Discovery Process");
PROCESS(cleanup_process, "Cleanup Process");
AUTOSTART_PROCESSES(&computation_node_process);

/* Function prototypes */
static void send_discovery(void);
static void send_join(uint16_t parent);
static void forward_message(const void *data, uint16_t len, uint16_t dest);
static void send_command(uint16_t sensor_id, uint8_t command, uint16_t duration);
static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest);
static int8_t find_sensor(uint16_t sensor_id);
static int8_t add_sensor(uint16_t sensor_id);
static void add_reading(int8_t index, uint16_t value, uint32_t timestamp);
static float calculate_slope(int8_t sensor_index);
static void cleanup_inactive_sensors(void);

/* Main process */
PROCESS_THREAD(computation_node_process, ev, data)
{
  PROCESS_BEGIN();
  
  /* Initialize NullNet */
  nullnet_set_input_callback(receive_callback);
  
  /* Initialize sensor data */
  memset(sensors, 0, sizeof(sensors));
  
  /* Start other processes */
  process_start(&discovery_process, NULL);
  process_start(&cleanup_process, NULL);
  
  LOG_INFO("Computation node %u started\n", COMPUTATION_NODE_ID);
  
  PROCESS_END();
}

/* Discovery process - finds and maintains parent connection */
PROCESS_THREAD(discovery_process, ev, data)
{
  static struct etimer discovery_timer;
  
  PROCESS_BEGIN();
  
  /* Set up discovery timer */
  etimer_set(&discovery_timer, random_rand() % DISCOVERY_INTERVAL);
  
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&discovery_timer));
    
    /* Send discovery message */
    send_discovery();
    
    /* Reset timer with some randomization to avoid collisions */
    etimer_set(&discovery_timer, DISCOVERY_INTERVAL + 
              (random_rand() % (DISCOVERY_INTERVAL/10)));
  }
  
  PROCESS_END();
}

/* Cleanup process - removes inactive sensors */
PROCESS_THREAD(cleanup_process, ev, data)
{
  static struct etimer cleanup_timer;
  
  PROCESS_BEGIN();
  
  /* Set up cleanup timer */
  etimer_set(&cleanup_timer, CLEANUP_INTERVAL);
  
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&cleanup_timer));
    
    /* Clean up inactive sensors */
    cleanup_inactive_sensors();
    
    /* Reset timer */
    etimer_set(&cleanup_timer, CLEANUP_INTERVAL);
  }
  
  PROCESS_END();
}

/* Send discovery message to find potential parents */
static void send_discovery(void)
{
  discovery_msg_t msg;
  
  /* Fill message header */
  msg.header.type = MSG_TYPE_DISCOVERY;
  msg.header.source = COMPUTATION_NODE_ID;
  msg.header.dest = 0xFFFF; /* Broadcast */
  msg.header.hop_count = 0;
  
  /* Fill discovery-specific fields */
  msg.node_type = NODE_TYPE_COMPUTATION;
  msg.parent = parent_id;
  msg.hop_to_root = hop_to_root;
  msg.energy = energy_level;
  
  /* Send message */
  nullnet_buf = (uint8_t *)&msg;
  nullnet_len = sizeof(msg);
  NETSTACK_NETWORK.output(NULL);
  
  LOG_INFO("Sent discovery message\n");
}

/* Send join message to selected parent */
static void send_join(uint16_t new_parent)
{
  join_msg_t msg;
  linkaddr_t parent_addr;
  
  /* Fill message header */
  msg.header.type = MSG_TYPE_JOIN;
  msg.header.source = COMPUTATION_NODE_ID;
  msg.header.dest = new_parent;
  msg.header.hop_count = 0;
  
  /* Fill join-specific fields */
  msg.parent = new_parent;
  
  /* Set parent address */
  parent_addr.u8[0] = new_parent;
  parent_addr.u8[1] = 0;
  
  /* Send message */
  nullnet_buf = (uint8_t *)&msg;
  nullnet_len = sizeof(msg);
  NETSTACK_NETWORK.output(&parent_addr);
  
  /* Update parent */
  parent_id = new_parent;
  
  LOG_INFO("Joined parent %u\n", parent_id);
}

/* Forward a message to its destination */
static void forward_message(const void *data, uint16_t len, uint16_t dest)
{
  linkaddr_t dest_addr;
  
  /* Set destination address */
  if(dest == parent_id) {
    dest_addr.u8[0] = parent_id;
    dest_addr.u8[1] = 0;
  } else {
    /* If not parent, forward to parent */
    dest_addr.u8[0] = parent_id;
    dest_addr.u8[1] = 0;
  }
  
  /* Forward message */
  nullnet_buf = (uint8_t *)data;
  nullnet_len = len;
  NETSTACK_NETWORK.output(&dest_addr);
  
  LOG_INFO("Forwarded message to %u\n", dest_addr.u8[0]);
}

/* Send command to a sensor node */
static void send_command(uint16_t sensor_id, uint8_t command, uint16_t duration)
{
  command_msg_t msg;
  linkaddr_t dest_addr;
  
  /* Fill message header */
  msg.header.type = MSG_TYPE_COMMAND;
  msg.header.source = COMPUTATION_NODE_ID;
  msg.header.dest = sensor_id;
  msg.header.hop_count = 0;
  
  /* Fill command-specific fields */
  msg.sensor_id = sensor_id;
  msg.command = command;
  msg.duration = duration;
  
  /* Set destination address */
  dest_addr.u8[0] = sensor_id;
  dest_addr.u8[1] = 0;
  
  /* Send message */
  nullnet_buf = (uint8_t *)&msg;
  nullnet_len = sizeof(msg);
  NETSTACK_NETWORK.output(&dest_addr);
  
  LOG_INFO("Sent command %u to sensor %u for %u seconds\n", 
           command, sensor_id, duration);
}

/* Handle received messages */
static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest)
{
  if(len == 0) return;
  
  message_header_t *header = (message_header_t *)data;
  
  /* Process based on message type */
  switch(header->type) {
    case MSG_TYPE_DISCOVERY: {
      discovery_msg_t *msg = (discovery_msg_t *)data;
      
      /* If this is a better parent (closer to root or higher energy) */
      if(msg->hop_to_root < hop_to_root || 
         (msg->hop_to_root == hop_to_root && msg->energy > energy_level)) {
        /* Join this parent */
        send_join(msg->header.source);
        hop_to_root = msg->hop_to_root + 1;
      }
      break;
    }
    
    case MSG_TYPE_DATA: {
      data_msg_t *msg = (data_msg_t *)data;
      int8_t sensor_index;
      float slope;
      
      /* Check if we're already tracking this sensor */
      sensor_index = find_sensor(msg->sensor_id);
      
      if(sensor_index == -1) {
        /* If we're not at capacity, add this sensor */
        if(sensor_count < MAX_SENSOR_NODES) {
          sensor_index = add_sensor(msg->sensor_id);
          LOG_INFO("Added new sensor %u (index %d)\n", msg->sensor_id, sensor_index);
        } else {
          /* Forward to parent if we're at capacity */
          forward_message(data, len, parent_id);
          LOG_INFO("At capacity, forwarded data from sensor %u\n", msg->sensor_id);
          break;
        }
      }
      
      /* Add the reading */
      add_reading(sensor_index, msg->value, msg->timestamp);
      LOG_INFO("Received reading %u from sensor %u\n", msg->value, msg->sensor_id);
      
      /* If we have enough readings, calculate slope */
      if(sensors[sensor_index].count >= 2) {
        slope = calculate_slope(sensor_index);
        LOG_INFO("Calculated slope for sensor %u: %d.%02u\n", 
                msg->sensor_id, (int)slope, (unsigned int)((slope - (int)slope) * 100));
        
        /* If slope exceeds threshold, send command to open valve */
        if(slope > SLOPE_THRESHOLD) {
          send_command(msg->sensor_id, 1, 600); /* Open for 10 minutes */
          LOG_INFO("Slope exceeds threshold, opening valve for sensor %u\n", msg->sensor_id);
        }
      }
      break;
    }
    
    default:
      /* Forward other message types */
      forward_message(data, len, header->dest);
      break;
  }
}

/* Find a sensor by ID, returns index or -1 if not found */
static int8_t find_sensor(uint16_t sensor_id)
{
  for(int i = 0; i < sensor_count; i++) {
    if(sensors[i].sensor_id == sensor_id && sensors[i].active) {
      return i;
    }
  }
  return -1;
}

/* Add a new sensor, returns index */
static int8_t add_sensor(uint16_t sensor_id)
{
  /* First check for inactive slots */
  for(int i = 0; i < sensor_count; i++) {
    if(!sensors[i].active) {
      sensors[i].sensor_id = sensor_id;
      sensors[i].count = 0;
      sensors[i].last_update = clock_seconds();
      sensors[i].active = 1;
      return i;
    }
  }
  
  /* If no inactive slots, add to end */
  if(sensor_count < MAX_SENSOR_NODES) {
    sensors[sensor_count].sensor_id = sensor_id;
    sensors[sensor_count].count = 0;
    sensors[sensor_count].last_update = clock_seconds();
    sensors[sensor_count].active = 1;
    return sensor_count++;
  }
  
  /* Should not reach here */
  return -1;
}

/* Add a reading to a sensor's data */
static void add_reading(int8_t index, uint16_t value, uint32_t timestamp)
{
  /* If buffer is full, shift everything */
  if(sensors[index].count == MAX_SENSOR_READINGS) {
    for(int i = 0; i < MAX_SENSOR_READINGS - 1; i++) {
      sensors[index].readings[i] = sensors[index].readings[i + 1];
      sensors[index].timestamps[i] = sensors[index].timestamps[i + 1];
    }
    sensors[index].count--;
  }
  
  /* Add new reading */
  sensors[index].readings[sensors[index].count] = value;
  sensors[index].timestamps[sensors[index].count] = timestamp;
  sensors[index].count++;
  sensors[index].last_update = clock_seconds();
}

/* Calculate slope using least-squares method */
static float calculate_slope(int8_t sensor_index)
{
  float sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
  float x_mean, y_mean, slope;
  int n = sensors[sensor_index].count;
  
  /* If we don't have enough data, return 0 */
  if(n < 2) return 0;
  
  /* Calculate sums */
  for(int i = 0; i < n; i++) {
    float x = (float)sensors[sensor_index].timestamps[i];
    float y = (float)sensors[sensor_index].readings[i];
    
    sum_x += x;
    sum_y += y;
    sum_xy += x * y;
    sum_xx += x * x;
  }
  
  /* Calculate means */
  x_mean = sum_x / n;
  y_mean = sum_y / n;
  
  /* Calculate slope */
  if(sum_xx - n * x_mean * x_mean == 0) return 0;
  slope = (sum_xy - n * x_mean * y_mean) / (sum_xx - n * x_mean * x_mean);
  
  return slope;
}

/* Clean up inactive sensors */
static void cleanup_inactive_sensors(void)
{
  uint32_t current_time = clock_seconds();
  
  for(int i = 0; i < sensor_count; i++) {
    if(sensors[i].active && 
       current_time - sensors[i].last_update > INACTIVE_THRESHOLD) {
      /* Mark as inactive */
      sensors[i].active = 0;
      LOG_INFO("Sensor %u marked inactive (no data for %lu seconds)\n", 
               sensors[i].sensor_id, 
               (unsigned long)(current_time - sensors[i].last_update));
    }
  }
}
