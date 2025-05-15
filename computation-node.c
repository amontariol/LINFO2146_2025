#include "contiki.h"
#include "net/nullnet/nullnet.h"
#include "net/netstack.h"
#include "lib/random.h"
#include "sys/log.h"
#include <string.h>
#include <stdio.h>
#include "sys/node-id.h"
#include <math.h>  // Added for fabs()

#define LOG_MODULE "ComputationNode"
#define LOG_LEVEL LOG_LEVEL_INFO

#define COMPUTATION_NODE_ID    (node_id)
#define DISCOVERY_INTERVAL     (CLOCK_SECOND * 60)
#define MAX_SENSOR_NODES       5
#define MAX_SENSOR_READINGS    30
#define SLOPE_THRESHOLD        3.0  // Reduced threshold to trigger more often
#define CLEANUP_INTERVAL       (CLOCK_SECOND * 300)
#define VALVE_DURATION         60 // 1 minute for testing

// Routing table to keep track of next hops
#define MAX_ROUTES 10
typedef struct {
  uint8_t dest_id;
  uint8_t next_hop;
  uint8_t hop_count;
  uint32_t last_updated;
} route_entry_t;

typedef struct {
  uint8_t sensor_id;
  uint16_t readings[MAX_SENSOR_READINGS];
  uint8_t count;
  uint32_t last_update;
  uint8_t active;
  uint8_t valve_open;
  uint32_t valve_open_time;
} sensor_data_t;

static uint16_t parent_id = 0xFFFF;
static uint8_t hop_to_root = 0xFF;
static uint16_t energy_level = 1000;
static sensor_data_t sensors[MAX_SENSOR_NODES];
static uint8_t sensor_count = 0;
static route_entry_t routing_table[MAX_ROUTES];
static uint8_t route_count = 0;

PROCESS(computation_node_process, "Computation Node Process");
PROCESS(discovery_process, "Discovery Process");
PROCESS(valve_timer_process, "Valve Timer Process");
AUTOSTART_PROCESSES(&computation_node_process);

static void send_discovery(void);
static void forward_message(const uint8_t *data, uint16_t len, uint8_t dest);
static void send_command(uint8_t sensor_id, uint8_t command);
static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest);
static int8_t find_sensor(uint8_t sensor_id);
static int8_t add_sensor(uint8_t sensor_id);
static void add_reading(int8_t index, uint16_t value);
static float calculate_slope(int8_t sensor_index);
static uint8_t find_next_hop(uint8_t target_id);
static void update_route(uint8_t dest_id, uint8_t next_hop, uint8_t hop_count);

PROCESS_THREAD(computation_node_process, ev, data)
{
  PROCESS_BEGIN();
  nullnet_set_input_callback(receive_callback);
  memset(sensors, 0, sizeof(sensors));
  memset(routing_table, 0, sizeof(routing_table));
  process_start(&discovery_process, NULL);
  process_start(&valve_timer_process, NULL);
  LOG_INFO("Computation node %u started\n", COMPUTATION_NODE_ID);
  PROCESS_END();
}

PROCESS_THREAD(discovery_process, ev, data)
{
  static struct etimer discovery_timer;
  PROCESS_BEGIN();
  etimer_set(&discovery_timer, random_rand() % DISCOVERY_INTERVAL);
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&discovery_timer));
    send_discovery();
    etimer_set(&discovery_timer, DISCOVERY_INTERVAL + 
              (random_rand() % (DISCOVERY_INTERVAL/10)));
  }
  PROCESS_END();
}

PROCESS_THREAD(valve_timer_process, ev, data)
{
  static struct etimer timer;
  PROCESS_BEGIN();
  etimer_set(&timer, CLOCK_SECOND);
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));
    uint32_t now = clock_seconds();
    for(int i = 0; i < sensor_count; i++) {
      if(sensors[i].valve_open && (now - sensors[i].valve_open_time >= VALVE_DURATION)) {
        send_command(sensors[i].sensor_id, 0); // Close valve
        sensors[i].valve_open = 0;
        LOG_INFO("Valve closed for sensor %u (timer expired)\n", sensors[i].sensor_id);
      }
    }
    etimer_reset(&timer);
  }
  PROCESS_END();
}

static void update_route(uint8_t dest_id, uint8_t next_hop, uint8_t hop_count)
{
  // Check if route already exists
  for(int i = 0; i < route_count; i++) {
    if(routing_table[i].dest_id == dest_id) {
      // Update existing route if new one is better
      if(hop_count < routing_table[i].hop_count) {
        routing_table[i].next_hop = next_hop;
        routing_table[i].hop_count = hop_count;
        routing_table[i].last_updated = clock_seconds();
        LOG_INFO("Updated route to %u via %u (hop count: %u)\n", 
                dest_id, next_hop, hop_count);
      }
      return;
    }
  }
  
  // Add new route if table isn't full
  if(route_count < MAX_ROUTES) {
    routing_table[route_count].dest_id = dest_id;
    routing_table[route_count].next_hop = next_hop;
    routing_table[route_count].hop_count = hop_count;
    routing_table[route_count].last_updated = clock_seconds();
    route_count++;
    LOG_INFO("Added new route to %u via %u (hop count: %u)\n", 
            dest_id, next_hop, hop_count);
  }
}

static void send_discovery(void)
{
  static uint8_t discovery_msg[4];
  discovery_msg[0] = 1;
  discovery_msg[1] = COMPUTATION_NODE_ID;
  discovery_msg[2] = hop_to_root;
  discovery_msg[3] = energy_level >> 8;
  LOG_INFO("Sending discovery: type=%u, id=%u, hop=%u, energy=%u\n",
         discovery_msg[0], discovery_msg[1], discovery_msg[2], discovery_msg[3]);
  nullnet_buf = discovery_msg;
  nullnet_len = sizeof(discovery_msg);
  NETSTACK_NETWORK.output(NULL);
  LOG_INFO("Sent discovery message\n");
}

static void forward_message(const uint8_t *data, uint16_t len, uint8_t dest)
{
  linkaddr_t dest_addr;
  dest_addr.u8[0] = dest;
  dest_addr.u8[1] = 0;
  nullnet_buf = (uint8_t *)data;
  nullnet_len = len;
  NETSTACK_NETWORK.output(&dest_addr);
  LOG_INFO("Forwarded message to %u\n", dest_addr.u8[0]);
}

static void send_command(uint8_t sensor_id, uint8_t command)
{
  static uint8_t cmd_msg[4];
  linkaddr_t dest_addr;
  cmd_msg[0] = 4;
  cmd_msg[1] = sensor_id;
  cmd_msg[2] = command;
  cmd_msg[3] = 0;

  uint8_t next_hop = find_next_hop(sensor_id);
  dest_addr.u8[0] = next_hop;
  dest_addr.u8[1] = 0;
  nullnet_buf = cmd_msg;
  nullnet_len = sizeof(cmd_msg);
  NETSTACK_NETWORK.output(&dest_addr);

  LOG_INFO("Sent command %u to sensor %u via %u\n", command, sensor_id, next_hop);
  
  // Also broadcast the command to reach sensors that might not have established routes
  NETSTACK_NETWORK.output(NULL);
  LOG_INFO("Also broadcast command %u to sensor %u\n", command, sensor_id);
}

static uint8_t find_next_hop(uint8_t target_id)
{
  // Check routing table first
  for(int i = 0; i < route_count; i++) {
    if(routing_table[i].dest_id == target_id) {
      return routing_table[i].next_hop;
    }
  }
  
  // If no route found, try direct communication or forward to parent
  if(parent_id != 0xFFFF) {
    return parent_id;
  }
  
  // Last resort: try direct communication
  return target_id;
}

static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest)
{
  if(len == 0) return;
  LOG_INFO("RECEIVED MESSAGE from %u, length %u\n", src->u8[0], len);
  if(src->u8[0] == node_id) {
    LOG_INFO("Ignoring message from self\n");
    return;
  }
  uint8_t *msg = (uint8_t *)data;
  uint8_t msg_type = msg[0];
  LOG_INFO("Message type: %u\n", msg_type);
  
  // Update routing information based on the source
  update_route(src->u8[0], src->u8[0], 1);
  
  switch(msg_type) {
    case 1: /* Discovery message */
      if(len >= 4) {
        uint8_t source_id = msg[1];
        uint8_t hop_count = msg[2];
        uint8_t energy = msg[3];
        LOG_INFO("Discovery details: source=%u, hop_count=%u, energy=%u, my_hop=%u\n", 
                 source_id, hop_count, energy, hop_to_root);
        
        // Update route to the source
        update_route(source_id, src->u8[0], hop_count + 1);
        
        if(hop_count < hop_to_root || 
           (hop_count == hop_to_root && energy > (energy_level >> 8))) {
          LOG_INFO("Better parent found! Old: %u (hop %u), New: %u (hop %u)\n",
                   parent_id, hop_to_root, source_id, hop_count + 1);
          parent_id = source_id;
          hop_to_root = hop_count + 1;
          LOG_INFO("Selected new parent %u (hop count: %u)\n", 
                   parent_id, hop_to_root);
        }else {
          LOG_INFO("Not a better parent. Current: %u (hop %u)\n", 
                   parent_id, hop_to_root);
        }
      }
      break;
    case 3: /* Data message */
      if(len >= 6) {
        uint8_t source_id = msg[1];
        uint16_t value = (msg[4] << 8) | msg[5];
        
        // Update route to the source
        update_route(source_id, src->u8[0], 1);
        
        if(sensor_count < MAX_SENSOR_NODES || find_sensor(source_id) >= 0) {
          int8_t sensor_index = find_sensor(source_id);
          if(sensor_index == -1) {
            sensor_index = add_sensor(source_id);
            LOG_INFO("Added new sensor %u (index %d)\n", source_id, sensor_index);
          }
          add_reading(sensor_index, value);
          LOG_INFO("Received reading %u from sensor %u\n", value, source_id);
          if(sensors[sensor_index].count >= 2) {
            float slope = calculate_slope(sensor_index);
            LOG_INFO("Calculated slope for sensor %u: %d.%02u (readings: %u)\n", 
                    source_id, (int)slope, (unsigned int)((slope - (int)slope) * 100),
                    sensors[sensor_index].count);
            
            // Use absolute value of slope for threshold comparison
            if(fabs(slope) > SLOPE_THRESHOLD && !sensors[sensor_index].valve_open) {
              send_command(source_id, 1); /* Open valve */
              sensors[sensor_index].valve_open = 1;
              sensors[sensor_index].valve_open_time = clock_seconds();
              LOG_INFO("Slope exceeds threshold, opening valve for sensor %u\n", source_id);
            }
          }
        } else {
          forward_message(msg, len, parent_id);
          LOG_INFO("At capacity, forwarded data from sensor %u\n", source_id);
        }
      }
      break;
    case 4: { // Command message
      uint8_t target_id = msg[1];
      uint8_t command = msg[2];
      
      if(target_id == node_id) {
        // Should not happen for computation node
        LOG_INFO("Received command %u for self, ignoring\n", command);
      } else {
        // Forward the command toward the target
        uint8_t next_hop = find_next_hop(target_id);
        if(next_hop != 0xFF) {
          forward_message(msg, len, next_hop);
          LOG_INFO("Forwarded command %u for sensor %u to %u\n", command, target_id, next_hop);
        } else {
          // If no specific route, broadcast the command
          nullnet_buf = (uint8_t *)msg;
          nullnet_len = len;
          NETSTACK_NETWORK.output(NULL);
          LOG_INFO("Broadcast command %u for sensor %u\n", command, target_id);
        }
      }
      break;
    }
    default:
      if(parent_id != 0xFFFF) {
        forward_message(msg, len, parent_id);
      }
      break;
  }
}

static int8_t find_sensor(uint8_t sensor_id)
{
  for(int i = 0; i < sensor_count; i++) {
    if(sensors[i].sensor_id == sensor_id && sensors[i].active) {
      return i;
    }
  }
  return -1;
}

static int8_t add_sensor(uint8_t sensor_id)
{
  for(int i = 0; i < sensor_count; i++) {
    if(!sensors[i].active) {
      sensors[i].sensor_id = sensor_id;
      sensors[i].count = 0;
      sensors[i].last_update = clock_seconds();
      sensors[i].active = 1;
      sensors[i].valve_open = 0;
      sensors[i].valve_open_time = 0;
      return i;
    }
  }
  if(sensor_count < MAX_SENSOR_NODES) {
    sensors[sensor_count].sensor_id = sensor_id;
    sensors[sensor_count].count = 0;
    sensors[sensor_count].last_update = clock_seconds();
    sensors[sensor_count].active = 1;
    sensors[sensor_count].valve_open = 0;
    sensors[sensor_count].valve_open_time = 0;
    return sensor_count++;
  }
  return -1;
}

static void add_reading(int8_t index, uint16_t value)
{
  if(sensors[index].count == MAX_SENSOR_READINGS) {
    for(int i = 0; i < MAX_SENSOR_READINGS - 1; i++) {
      sensors[index].readings[i] = sensors[index].readings[i + 1];
    }
    sensors[index].count--;
  }
  sensors[index].readings[sensors[index].count] = value;
  sensors[index].count++;
  sensors[index].last_update = clock_seconds();
  
  // Debug: print all readings for this sensor
  LOG_INFO("Sensor %u readings: ", sensors[index].sensor_id);
  for(int i = 0; i < sensors[index].count; i++) {
    LOG_INFO_("%u ", sensors[index].readings[i]);
  }
  LOG_INFO_("\n");
}

static float calculate_slope(int8_t sensor_index)
{
  float sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
  float x_mean, y_mean, slope;
  int n = sensors[sensor_index].count;
  
  if(n < 2) return 0;
  
  for(int i = 0; i < n; i++) {
    float x = (float)i;
    float y = (float)sensors[sensor_index].readings[i];
    sum_x += x;
    sum_y += y;
    sum_xy += x * y;
    sum_xx += x * x;
  }
  
  x_mean = sum_x / n;
  y_mean = sum_y / n;
  
  float denominator = (sum_xx - n * x_mean * x_mean);
  if(fabs(denominator) < 0.0001) {  // Avoid division by zero with a small threshold
    LOG_INFO("Warning: Denominator near zero in slope calculation for sensor %u\n", 
             sensors[sensor_index].sensor_id);
    return 0;
  }
  
  slope = (sum_xy - n * x_mean * y_mean) / denominator;
  
  // Debug output for slope calculation
  LOG_INFO("Slope calculation for sensor %u: n=%d, sum_x=%.2f, sum_y=%.2f, sum_xy=%.2f, sum_xx=%.2f\n",
           sensors[sensor_index].sensor_id, n, sum_x, sum_y, sum_xy, sum_xx);
  LOG_INFO("x_mean=%.2f, y_mean=%.2f, denominator=%.2f, slope=%.2f\n",
           x_mean, y_mean, denominator, slope);
  
  return slope;
}
