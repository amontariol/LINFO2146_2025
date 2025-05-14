/* computation-node.c */
#include "contiki.h"
#include "net/nullnet/nullnet.h"
#include "net/netstack.h"
#include "lib/random.h"
#include "sys/log.h"
#include <string.h>
#include <stdio.h>

#define LOG_MODULE "ComputationNode"
#define LOG_LEVEL LOG_LEVEL_INFO

/* Configuration */
#define COMPUTATION_NODE_ID    (linkaddr_node_addr.u8[0])
#define DISCOVERY_INTERVAL     (CLOCK_SECOND * 60)
#define MAX_SENSOR_NODES       5
#define MAX_SENSOR_READINGS    30
#define SLOPE_THRESHOLD        5.0  /* Adjust based on your needs */
#define CLEANUP_INTERVAL       (CLOCK_SECOND * 300)  /* 5 minutes */

/* Sensor data structure */
typedef struct {
  uint8_t sensor_id;
  uint16_t readings[MAX_SENSOR_READINGS];
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
AUTOSTART_PROCESSES(&computation_node_process);

/* Function prototypes */
static void send_discovery(void);
static void forward_message(const uint8_t *data, uint16_t len, uint8_t dest);
static void send_command(uint8_t sensor_id, uint8_t command);
static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest);
static int8_t find_sensor(uint8_t sensor_id);
static int8_t add_sensor(uint8_t sensor_id);
static void add_reading(int8_t index, uint16_t value);
static float calculate_slope(int8_t sensor_index);

/* Main process */
PROCESS_THREAD(computation_node_process, ev, data)
{
  PROCESS_BEGIN();
  
  /* Initialize NullNet */
  nullnet_set_input_callback(receive_callback);
  
  /* Initialize sensor data */
  memset(sensors, 0, sizeof(sensors));
  
  /* Start discovery process */
  process_start(&discovery_process, NULL);
  
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
    
    /* Reset timer */
    etimer_set(&discovery_timer, DISCOVERY_INTERVAL + 
              (random_rand() % (DISCOVERY_INTERVAL/10)));
  }
  
  PROCESS_END();
}

/* Send discovery message to find potential parents */
static void send_discovery(void)
{
  /* Simple discovery message */
  static uint8_t discovery_msg[4];
  
  /* Fill message */
  discovery_msg[0] = 1; /* Message type: discovery */
  discovery_msg[1] = COMPUTATION_NODE_ID; /* Source ID */
  discovery_msg[2] = hop_to_root; /* Hop count to root */
  discovery_msg[3] = energy_level >> 8; /* Energy level (high byte) */
  
  /* Send message */
  nullnet_buf = discovery_msg;
  nullnet_len = sizeof(discovery_msg);
  NETSTACK_NETWORK.output(NULL);
  
  LOG_INFO("Sent discovery message\n");
}

/* Forward a message to its destination */
static void forward_message(const uint8_t *data, uint16_t len, uint8_t dest)
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
static void send_command(uint8_t sensor_id, uint8_t command)
{
  /* Simple command message */
  static uint8_t cmd_msg[4];
  linkaddr_t dest_addr;
  
  /* Fill message */
  cmd_msg[0] = 4; /* Message type: command */
  cmd_msg[1] = sensor_id; /* Target sensor ID */
  cmd_msg[2] = command; /* Command: 1=open, 0=close */
  cmd_msg[3] = 0; /* Reserved */
  
  /* Set destination address */
  dest_addr.u8[0] = sensor_id;
  dest_addr.u8[1] = 0;
  
  /* Send message */
  nullnet_buf = cmd_msg;
  nullnet_len = sizeof(cmd_msg);
  NETSTACK_NETWORK.output(&dest_addr);
  
  LOG_INFO("Sent command %u to sensor %u\n", command, sensor_id);
}

/* Handle received messages */
static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest)
{
  if(len == 0) return;
  
  uint8_t *msg = (uint8_t *)data;
  uint8_t msg_type = msg[0];
  
  /* Process based on message type */
  switch(msg_type) {
    case 1: /* Discovery message */
      if(len >= 4) {
        uint8_t source_id = msg[1];
        uint8_t hop_count = msg[2];
        uint8_t energy = msg[3];
        
        /* If this is a better parent */
        if(hop_count < hop_to_root || 
           (hop_count == hop_to_root && energy > (energy_level >> 8))) {
          /* Update parent */
          parent_id = source_id;
          hop_to_root = hop_count + 1;
          
          LOG_INFO("Selected new parent %u (hop count: %u)\n", 
                   parent_id, hop_to_root);
        }
      }
      break;
    
    case 3: /* Data message */
      if(len >= 6) {
        uint8_t source_id = msg[1];
        /* Remove the unused variable dest_id */
        uint16_t value = (msg[4] << 8) | msg[5];
        
        /* Check if we should handle this sensor */
        if(sensor_count < MAX_SENSOR_NODES || find_sensor(source_id) >= 0) {
          int8_t sensor_index = find_sensor(source_id);
          
          /* Add sensor if not found */
          if(sensor_index == -1) {
            sensor_index = add_sensor(source_id);
            LOG_INFO("Added new sensor %u (index %d)\n", source_id, sensor_index);
          }
          
          /* Add reading */
          add_reading(sensor_index, value);
          LOG_INFO("Received reading %u from sensor %u\n", value, source_id);
          
          /* Calculate slope if we have enough readings */
          if(sensors[sensor_index].count >= 2) {
            float slope = calculate_slope(sensor_index);
            LOG_INFO("Calculated slope for sensor %u: %d.%02u\n", 
                    source_id, (int)slope, (unsigned int)((slope - (int)slope) * 100));
            
            /* If slope exceeds threshold, send command to open valve */
            if(slope > SLOPE_THRESHOLD) {
              send_command(source_id, 1); /* Open valve */
              LOG_INFO("Slope exceeds threshold, opening valve for sensor %u\n", source_id);
            }
          }
        } else {
          /* Forward to parent if we're at capacity */
          forward_message(msg, len, parent_id);
          LOG_INFO("At capacity, forwarded data from sensor %u\n", source_id);
        }
      }
      break;
    
    default:
      /* Forward other message types */
      if(parent_id != 0xFFFF) {
        forward_message(msg, len, parent_id);
      }
      break;
  }
}

/* Find a sensor by ID, returns index or -1 if not found */
static int8_t find_sensor(uint8_t sensor_id)
{
  for(int i = 0; i < sensor_count; i++) {
    if(sensors[i].sensor_id == sensor_id && sensors[i].active) {
      return i;
    }
  }
  return -1;
}

/* Add a new sensor, returns index */
static int8_t add_sensor(uint8_t sensor_id)
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
static void add_reading(int8_t index, uint16_t value)
{
  /* If buffer is full, shift everything */
  if(sensors[index].count == MAX_SENSOR_READINGS) {
    for(int i = 0; i < MAX_SENSOR_READINGS - 1; i++) {
      sensors[index].readings[i] = sensors[index].readings[i + 1];
    }
    sensors[index].count--;
  }
  
  /* Add new reading */
  sensors[index].readings[sensors[index].count] = value;
  sensors[index].count++;
  sensors[index].last_update = clock_seconds();
}

/* Calculate slope using simple linear regression */
static float calculate_slope(int8_t sensor_index)
{
  float sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
  float x_mean, y_mean, slope;
  int n = sensors[sensor_index].count;
  
  /* If we don't have enough data, return 0 */
  if(n < 2) return 0;
  
  /* Calculate sums */
  for(int i = 0; i < n; i++) {
    float x = (float)i;
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
  float denominator = (sum_xx - n * x_mean * x_mean);
  if(denominator == 0) return 0;
  
  slope = (sum_xy - n * x_mean * y_mean) / denominator;
  
  return slope;
}
