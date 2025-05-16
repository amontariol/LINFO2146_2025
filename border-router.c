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
#define MAX_CHILDREN           10

static uint16_t energy_level = 1000;
static uint8_t children[MAX_CHILDREN];
static uint8_t child_count = 0;

PROCESS(border_router_process, "Border Router Process");
PROCESS(discovery_process, "Discovery Process");
PROCESS(serial_process, "Serial Process");
AUTOSTART_PROCESSES(&border_router_process);

static void send_discovery(void);
static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest);
static void process_serial_input(char *line);
static void add_child(uint8_t child_id);
static uint8_t find_child(uint8_t target_id);

PROCESS_THREAD(border_router_process, ev, data)
{
  PROCESS_BEGIN();

  nullnet_set_input_callback(receive_callback);
  process_start(&discovery_process, NULL);
  process_start(&serial_process, NULL);

  LOG_INFO("Border router started (ID %u)\n", BORDER_ROUTER_ID);

  while(1) {
    PROCESS_YIELD();
  }

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
    PROCESS_YIELD();
    if(ev == serial_line_event_message) {
      process_serial_input((char *)data);
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

static void add_child(uint8_t child_id)
{
  for(int i = 0; i < child_count; i++) {
    if(children[i] == child_id) return; // Already a child
  }
  
  if(child_count < MAX_CHILDREN) {
    children[child_count++] = child_id;
    LOG_INFO("Added child %u\n", child_id);
  }
}

static uint8_t find_child(uint8_t target_id)
{
  // Check if target is a direct child
  for (int i = 0; i < child_count; i++)
  {
    if (children[i] == target_id)
    {
      return target_id;
    }
  }

  return 0xFF;
}

static void send_message(uint8_t *data, uint16_t len, linkaddr_t *dest)
{
  nullnet_buf = data;
  nullnet_len = len;
  NETSTACK_NETWORK.output(dest);
}

static void process_serial_input(char *line)
{
  char *token;
  token = strtok(line, " ");
  
  if(token && strcmp(token, "COMMAND") == 0) {
    token = strtok(NULL, " ");
    if(!token) return;
    uint8_t sensor_id = atoi(token);
    
    token = strtok(NULL, " ");
    if(!token) return;
    uint8_t command = atoi(token);
    
    // Create command message
    static uint8_t cmd_msg[4];
    cmd_msg[0] = 4;
    cmd_msg[1] = sensor_id;
    cmd_msg[2] = command;
    cmd_msg[3] = 0;
    
    // Find next hop to target
    uint8_t child = find_child(sensor_id);
    if(child == 0xFF) {
      // If we don't know the route, broadcast
      LOG_INFO("Unknown route to sensor %u, broadcasting command\n", sensor_id);
      // Broadcast to all children
      send_message(cmd_msg, sizeof(cmd_msg), NULL);
    } else {
      // Send to next hop
      linkaddr_t child_addr;
      child_addr.u8[0] = child;
      child_addr.u8[1] = 0;
      // Send command to child
      send_message(cmd_msg, sizeof(cmd_msg), &child_addr);
      LOG_INFO("Sent command %u to sensor %u via %u\n", command, sensor_id, child);
    }
  }
}

static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest)
{
  /*
  // Print raw data for debugging
  printf("DATA ");
  for(int i = 0; i < len; i++) {
    printf("%02x ", ((uint8_t *)data)[i]);
  }
  printf("\n");
  */
  if(len == 0) return;
  if(src->u8[0] == node_id) return;

  uint8_t *msg = (uint8_t *)data;
  uint8_t msg_type = msg[0];
  if (len >= 2) {
    uint8_t source_id = msg[1];
    LOG_INFO("RECEIVED MESSAGE from %u, length %u\n", source_id, len);
  } else {
    LOG_INFO("RECEIVED MESSAGE from %u, length %u\n", src->u8[0], len);
  }
  
  switch(msg_type) {
    case 1: // Discovery response
      // Add sender as child
      add_child(src->u8[0]);
      break;
      
    case 3: // Data message
      if(len >= 6) {
        uint8_t source_id = msg[1];
        uint16_t value = (msg[4] << 8) | msg[5];
        // Forward to server
        printf("DATA %u %u %lu\n", source_id, value, (unsigned long)clock_seconds());
      }
      break;
      
    case 4: // Command message
      if(len >= 4) {
        uint8_t target_id = msg[1];
        uint8_t command = msg[2];
        
        if(target_id != BORDER_ROUTER_ID) {
          // Forward command to target
          uint8_t child = find_child(target_id);
          if(child != 0xFF) {
            linkaddr_t child_addr;
            child_addr.u8[0] = child;
            child_addr.u8[1] = 0;
            send_message(msg, len, &child_addr);
            LOG_INFO("Forwarded command %u to sensor %u via %u\n", command, target_id, child);
          } else {
            // If we don't know the route, broadcast
            send_message(msg, len, NULL);
            LOG_INFO("Unknown route to sensor %u, broadcasting command\n", target_id);
          }
        }
      }
      break;
  }
}
