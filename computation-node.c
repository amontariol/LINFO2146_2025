#include "contiki.h"
#include "net/nullnet/nullnet.h"
#include "net/netstack.h"
#include "lib/random.h"
#include "sys/log.h"
#include <string.h>
#include <stdio.h>
#include "sys/node-id.h"

#define LOG_MODULE "ComputationNode"
#define LOG_LEVEL LOG_LEVEL_INFO

#define COMPUTATION_NODE_ID (node_id)
#define DISCOVERY_INTERVAL (CLOCK_SECOND * 60)
#define MAX_SENSOR_NODES 5
#define MAX_SENSOR_READINGS 30
#define SLOPE_THRESHOLD 5.0
#define CLEANUP_INTERVAL (CLOCK_SECOND * 300)
#define VALVE_DURATION 600
#define MAX_CHILDREN 10

typedef struct {
  uint8_t sensor_id;
  uint16_t readings[MAX_SENSOR_READINGS];
  uint8_t count;
  uint32_t last_update;
  uint8_t active;
  uint8_t valve_open;
  uint32_t valve_open_time;
  uint8_t is_direct_child;
} sensor_data_t;

static uint8_t parent_id = 0xFF;
static uint8_t hop_to_root = 0xFF;
static uint16_t energy_level = 1000;
static sensor_data_t sensors[MAX_SENSOR_NODES];
static uint8_t sensor_count = 0;
static uint8_t children[MAX_CHILDREN];
static uint8_t child_count = 0;

PROCESS(computation_node_process, "Computation Node Process");
PROCESS(discovery_process, "Discovery Process");
PROCESS(valve_timer_process, "Valve Timer Process");
PROCESS(cleanup_process, "Cleanup Process");
AUTOSTART_PROCESSES(&computation_node_process);

static void send_discovery(void);
static void forward_data(const uint8_t *data, uint16_t len, uint8_t treated);
static void forward_command(const uint8_t *data, uint16_t len, uint8_t dest);
static void send_command(uint8_t sensor_id, uint8_t command);
static void receive_callback(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest);
static int8_t find_sensor(uint8_t sensor_id);
static int8_t add_sensor(uint8_t sensor_id, uint8_t is_direct_child);
static void add_reading(int8_t index, uint16_t value);
static float calculate_slope(int8_t sensor_index);
static uint8_t find_child(uint8_t target_id);
static void add_child(uint8_t child_id);


PROCESS_THREAD(computation_node_process, ev, data)
{
  PROCESS_BEGIN();
  nullnet_set_input_callback(receive_callback);
  memset(sensors, 0, sizeof(sensors));
  process_start(&discovery_process, NULL);
  process_start(&valve_timer_process, NULL);
  process_start(&cleanup_process, NULL);
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
    etimer_set(&discovery_timer, DISCOVERY_INTERVAL + (random_rand() % (DISCOVERY_INTERVAL/10)));
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
        send_command(sensors[i].sensor_id, 0);
        sensors[i].valve_open = 0;
        LOG_INFO("Valve closed for sensor %u (timer expired)\n", sensors[i].sensor_id);
      }
    }
    etimer_reset(&timer);
  }
  PROCESS_END();
}

PROCESS_THREAD(cleanup_process, ev, data)
{
  static struct etimer cleanup_timer;
  PROCESS_BEGIN();
  etimer_set(&cleanup_timer, CLEANUP_INTERVAL);
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&cleanup_timer));
    uint32_t now = clock_seconds();
    for(int i = 0; i < sensor_count; i++) {
      if(sensors[i].active && (now - sensors[i].last_update > CLEANUP_INTERVAL / CLOCK_SECOND)) {
        LOG_INFO("Removing inactive sensor %u\n", sensors[i].sensor_id);
        sensors[i].active = 0;
      }
    }
    etimer_reset(&cleanup_timer);
  }
  PROCESS_END();
}


static void send_message(uint8_t *data, uint16_t len, linkaddr_t *dest)
{
  nullnet_buf = data;
  nullnet_len = len;
  NETSTACK_NETWORK.output(dest);
}


static void send_discovery(void)
{
  static uint8_t discovery_msg[4];
  discovery_msg[0] = 1;
  discovery_msg[1] = COMPUTATION_NODE_ID;
  discovery_msg[2] = hop_to_root;
  discovery_msg[3] = energy_level >> 8;
  LOG_INFO("Sending discovery: type=%u, id=%u, hop=%u, energy=%u\n", discovery_msg[0], discovery_msg[1], discovery_msg[2], discovery_msg[3]);
  send_message(discovery_msg, sizeof(discovery_msg), NULL);
  LOG_INFO("Sent discovery message\n");
}


static void forward_data(const uint8_t *data, uint16_t len, uint8_t treated)
{
  linkaddr_t parent_addr;
  parent_addr.u8[0] = parent_id;
  parent_addr.u8[1] = 0;
  static uint8_t data_msg[6];
  memcpy(data_msg, data, len);
  data_msg[3] = treated;
  send_message(data_msg, len, &parent_addr);
  LOG_INFO("Forwarded data to %u, len: %u\n", parent_addr.u8[0], len);
}


static void forward_command(const uint8_t *data, uint16_t len, uint8_t dest)
{
  linkaddr_t child_addr;
  child_addr.u8[0] = dest;
  child_addr.u8[1] = 0;
  static uint8_t cmd_msg[4];
  memcpy(cmd_msg, data, len);
  send_message(cmd_msg, len, &child_addr);
  LOG_INFO("Forwarded command to %u, len: %u\n", child_addr.u8[0], len);
}

static void send_command(uint8_t sensor_id, uint8_t command)
{
  static uint8_t cmd_msg[4];
  cmd_msg[0] = 4;
  cmd_msg[1] = sensor_id;
  cmd_msg[2] = command;
  cmd_msg[3] = 0;

  uint8_t child = find_child(sensor_id);
  if(child != 0xFF) {
    linkaddr_t dest_addr;
    dest_addr.u8[0] = child;
    dest_addr.u8[1] = 0;
    send_message(cmd_msg, sizeof(cmd_msg), &dest_addr);
    LOG_INFO("Sent command %u to sensor %u via %u\n", command, sensor_id, child);
  } else {
    LOG_INFO("Cannot send command: sensor %u is not my child\n", sensor_id);
  }
}

static void add_child(uint8_t child_id)
{
  for(int i = 0; i < child_count; i++) {
    if(children[i] == child_id) return;
  }
  if(child_count < MAX_CHILDREN) {
    children[child_count++] = child_id;
    LOG_INFO("Added child %u\n", child_id);
  }
}

static uint8_t find_child(uint8_t target_id)
{
  for(int i = 0; i < child_count; i++) {
    if(children[i] == target_id) {
      return target_id;
    }
  }
  for(int i = 0; i < sensor_count; i++) {
    if(sensors[i].sensor_id == target_id && sensors[i].active && sensors[i].is_direct_child) {
      return target_id;
    }
  }
  return parent_id;
}

static void receive_callback(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest)
{
  if(len == 0) return;

  uint8_t *msg = (uint8_t *)data;
  uint8_t msg_type = msg[0];
  if (len >= 2) {
    uint8_t source_id = msg[1];
    LOG_INFO("RECEIVED MESSAGE from %u, length %u\n", source_id, len);
  } else {
    LOG_INFO("RECEIVED MESSAGE from %u, length %u\n", src->u8[0], len);
  }
  if(src->u8[0] == node_id) {
    LOG_INFO("Ignoring message from self\n");
    return;
  }
  LOG_INFO("Message type: %u\n", msg_type);
  switch(msg_type) {
    case 1:
      if(len >= 4) {
        uint8_t source_id = msg[1];
        uint8_t hop_count = msg[2];
        uint8_t energy = msg[3];
        LOG_INFO("Discovery details: source=%u, hop_count=%u, energy=%u, my_hop=%u\n", source_id, hop_count, energy, hop_to_root);
        if(hop_count < hop_to_root || 
           (hop_count == hop_to_root && energy > (energy_level >> 8))) {
          LOG_INFO("Better parent found! Old: %u (hop %u), New: %u (hop %u)\n", parent_id, hop_to_root, source_id, hop_count + 1);
          parent_id = source_id;
          hop_to_root = hop_count + 1;
          LOG_INFO("Selected new parent %u (hop count: %u)\n", parent_id, hop_to_root);
        } else {
          LOG_INFO("Not a better parent. Current: %u (hop %u)\n", parent_id, hop_to_root);
        }
        
       
        if(hop_count > hop_to_root) {
          add_child(source_id);
        }
      }
      break;
    case 3:
      if(len >= 6) {
        uint8_t source_id = msg[1];
        uint16_t value = (msg[4] << 8) | msg[5];
        if(sensor_count < MAX_SENSOR_NODES || find_sensor(source_id) >= 0) {
          int8_t sensor_index = find_sensor(source_id);
          if(sensor_index == -1) {
            sensor_index = add_sensor(source_id, src->u8[0] == source_id);
            LOG_INFO("Added new sensor %u (index %d)\n", source_id, sensor_index);
          }
          add_reading(sensor_index, value);
          LOG_INFO("Received reading %u from sensor %u\n", value, source_id);
          if(sensors[sensor_index].count >= 2) {
            float slope = calculate_slope(sensor_index);
            LOG_INFO("Calculated slope for sensor %u: %d.%02u\n", source_id, (int)slope, (unsigned int)((slope - (int)slope) * 100));
            if(slope > SLOPE_THRESHOLD && !sensors[sensor_index].valve_open) {
              send_command(source_id, 1);
              sensors[sensor_index].valve_open = 1;
              sensors[sensor_index].valve_open_time = clock_seconds();
              LOG_INFO("Slope exceeds threshold, opening valve for sensor %u\n", source_id);
            }
            forward_data(msg, len, 1);
          }
        } else {
          if(parent_id != 0xFF) {
            forward_data(msg, len, 0);
            LOG_INFO("At capacity, forwarded data from sensor %u\n", source_id);
          }
        }
      }
      break;
    case 4: {
      uint8_t target_id = msg[1];
      if(target_id == node_id) {
        LOG_INFO("Received command for self, ignoring\n");
      } else {
        uint8_t child = find_child(target_id);
        if(child != 0xFF) {
          forward_command(msg, len, child);
          LOG_INFO("Forwarded command for sensor %u to %u\n", target_id, child);
        } else {
          send_message(msg, len, NULL);
          LOG_INFO("Broadcast command for sensor %u\n", target_id, child);
        }
      }
      break;
    }
    default:
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

static int8_t add_sensor(uint8_t sensor_id, uint8_t is_direct_child)
{
  for(int i = 0; i < sensor_count; i++) {
    if(!sensors[i].active) {
      sensors[i].sensor_id = sensor_id;
      sensors[i].count = 0;
      sensors[i].last_update = clock_seconds();
      sensors[i].active = 1;
      sensors[i].valve_open = 0;
      sensors[i].valve_open_time = 0;
      sensors[i].is_direct_child = is_direct_child;
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
    sensors[sensor_count].is_direct_child = is_direct_child;
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
  if(denominator == 0) return 0;
  slope = (sum_xy - n * x_mean * y_mean) / denominator;
  return slope;
}
