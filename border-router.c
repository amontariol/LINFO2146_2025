#include "contiki.h"
#include "net/nullnet/nullnet.h"
#include "net/netstack.h"
#include "sys/log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "sys/node-id.h"
#include "dev/serial-line.h"

#define LOG_MODULE "BorderRouter"
#define LOG_LEVEL LOG_LEVEL_INFO

#define BORDER_ROUTER_ID       1
#define DISCOVERY_INTERVAL     (CLOCK_SECOND * 30)

// Routing table to keep track of next hops
#define MAX_ROUTES 10
typedef struct {
  uint8_t dest_id;
  uint8_t next_hop;
  uint8_t hop_count;
  uint32_t last_updated;
} route_entry_t;

static route_entry_t routing_table[MAX_ROUTES];
static uint8_t route_count = 0;
static uint16_t energy_level = 1000;

PROCESS(border_router_process, "Border Router Process");
PROCESS(discovery_process, "Discovery Process");
PROCESS(serial_process, "Serial Process");
AUTOSTART_PROCESSES(&border_router_process);

static void send_discovery(void);
static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest);
static void forward_command(uint8_t sensor_id, uint8_t command);
static void update_route(uint8_t dest_id, uint8_t next_hop, uint8_t hop_count);
static uint8_t find_next_hop(uint8_t dest_id);

PROCESS_THREAD(border_router_process, ev, data)
{
  PROCESS_BEGIN();

  nullnet_set_input_callback(receive_callback);
  process_start(&discovery_process, NULL);
  process_start(&serial_process, NULL);

  // Initialize routing table
  memset(routing_table, 0, sizeof(routing_table));

  LOG_INFO("Border router started (ID %u)\n", BORDER_ROUTER_ID);

  PROCESS_END();
}

PROCESS_THREAD(discovery_process, ev, data)
{
  static struct etimer discovery_timer;
  PROCESS_BEGIN();

  etimer_set(&discovery_timer, DISCOVERY_INTERVAL);

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&discovery_timer));
    send_discovery();
    etimer_set(&discovery_timer, DISCOVERY_INTERVAL);
  }

  PROCESS_END();
}

PROCESS_THREAD(serial_process, ev, data)
{
  PROCESS_BEGIN();
  
  serial_line_init();
  
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(ev == serial_line_event_message);
    
    char *message = (char *)data;
    LOG_INFO("Received serial message: %s\n", message);
    
    // Parse command messages from server
    if(strncmp(message, "COMMAND", 7) == 0) {
      char *token = strtok(message, " ");
      token = strtok(NULL, " "); // Get sensor_id
      if(token != NULL) {
        uint8_t sensor_id = (uint8_t)atoi(token);
        token = strtok(NULL, " "); // Get command
        if(token != NULL) {
          uint8_t command = (uint8_t)atoi(token);
          LOG_INFO("Parsed command: sensor_id=%u, command=%u\n", sensor_id, command);
          forward_command(sensor_id, command);
        }
      }
    }
  }
  
  PROCESS_END();
}

static void send_discovery(void)
{
  static uint8_t discovery_msg[4];
  discovery_msg[0] = 1; // Discovery
  discovery_msg[1] = BORDER_ROUTER_ID;
  discovery_msg[2] = 0; // Hop to root
  discovery_msg[3] = energy_level >> 8;

  nullnet_buf = discovery_msg;
  nullnet_len = sizeof(discovery_msg);
  NETSTACK_NETWORK.output(NULL);

  LOG_INFO("Sent discovery: id=%u, hop=0, energy=%u\n",
           discovery_msg[1], discovery_msg[3]);
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

static uint8_t find_next_hop(uint8_t dest_id)
{
  for(int i = 0; i < route_count; i++) {
    if(routing_table[i].dest_id == dest_id) {
      return routing_table[i].next_hop;
    }
  }
  
  // If no specific route, try to send directly
  return dest_id;
}

static void forward_command(uint8_t sensor_id, uint8_t command)
{
  static uint8_t cmd_msg[4];
  linkaddr_t dest_addr;
  
  cmd_msg[0] = 4; // Command message type
  cmd_msg[1] = sensor_id;
  cmd_msg[2] = command;
  cmd_msg[3] = 0;
  
  // Find next hop to destination
  uint8_t next_hop = find_next_hop(sensor_id);
  
  dest_addr.u8[0] = next_hop;
  dest_addr.u8[1] = 0;
  
  nullnet_buf = cmd_msg;
  nullnet_len = sizeof(cmd_msg);
  NETSTACK_NETWORK.output(&dest_addr);
  
  LOG_INFO("Forwarded command %u to sensor %u via %u\n", command, sensor_id, next_hop);
  
  // For sensors directly connected to border router, also broadcast the command
  // This helps with sensors that might not have established routes yet
  if(next_hop == sensor_id) {
    NETSTACK_NETWORK.output(NULL);
    LOG_INFO("Also broadcast command %u to sensor %u\n", command, sensor_id);
  }
}

static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest)
{
  if(len == 0) return;
  if(src->u8[0] == node_id) return;

  uint8_t *msg = (uint8_t *)data;
  uint8_t msg_type = msg[0];

  if(msg_type == 3 && len >= 6) { // Data message
    uint8_t source_id = msg[1];
    uint16_t value = (msg[4] << 8) | msg[5];
    printf("Received data from %u: %u\n", source_id, value);
    printf("DATA ");
    for(int i = 0; i < len; i++) {
      printf("%02x ", ((uint8_t *)data)[i]);
    }
    printf("\n");
    update_route(source_id, src->u8[0], 1);
    printf("light,%02x:%02x,%u\n", source_id, 0, value);
  }
  else if(msg_type == 1 && len >= 4) { // Discovery message
    uint8_t source_id = msg[1];
    uint8_t hop_count = msg[2];
    update_route(source_id, src->u8[0], hop_count + 1);
  }
  else if(msg_type == 4 && len >= 4) { // Command message
    uint8_t target_id = msg[1];
    uint8_t command = msg[2];
    forward_command(target_id, command);
  }
}

