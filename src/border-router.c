/* border-router.c */
#include "contiki.h"
#include "net/nullnet/nullnet.h"
#include "net/netstack.h"
#include "sys/log.h"
#include "dev/serial-line.h"
#include "dev/uart0.h"
#include "message-format.h"
#include <string.h>
#include <stdio.h>

#define LOG_MODULE "BorderRouter"
#define LOG_LEVEL LOG_LEVEL_INFO

/* Configuration */
#define BORDER_ROUTER_ID       1
#define BROADCAST_CHANNEL      129
#define DATA_CHANNEL           130
#define DISCOVERY_INTERVAL     (CLOCK_SECOND * 30)

/* Global variables */
static uint16_t energy_level = 1000;

/* Process definitions */
PROCESS(border_router_process, "Border Router Process");
PROCESS(discovery_process, "Discovery Process");
AUTOSTART_PROCESSES(&border_router_process);

/* Function prototypes */
static void send_discovery(void);
static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest);
static void send_to_server(const void *data, uint16_t len);
static void process_server_command(const char *data);

/* Main process */
PROCESS_THREAD(border_router_process, ev, data)
{
  PROCESS_BEGIN();
  
  /* Initialize NullNet */
  nullnet_set_input_callback(receive_callback);
  
  /* Initialize serial communication */
  serial_line_init();
  uart0_set_input(serial_line_input_byte);
  
  /* Start discovery process */
  process_start(&discovery_process, NULL);
  
  LOG_INFO("Border router started\n");
  
  /* Main loop */
  while(1) {
    PROCESS_YIELD();
    
    /* Handle serial line events (from server) */
    if(ev == serial_line_event_message) {
      process_server_command((const char *)data);
    }
  }
  
  PROCESS_END();
}

/* Discovery process - broadcasts router presence */
PROCESS_THREAD(discovery_process, ev, data)
{
  static struct etimer discovery_timer;
  
  PROCESS_BEGIN();
  
  /* Set up discovery timer */
  etimer_set(&discovery_timer, DISCOVERY_INTERVAL);
  
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&discovery_timer));
    
    /* Send discovery message */
    send_discovery();
    
    /* Reset timer */
    etimer_set(&discovery_timer, DISCOVERY_INTERVAL);
  }
  
  PROCESS_END();
}

/* Send discovery message to announce presence */
static void send_discovery(void)
{
  discovery_msg_t msg;
  
  /* Fill message header */
  msg.header.type = MSG_TYPE_DISCOVERY;
  msg.header.source = BORDER_ROUTER_ID;
  msg.header.dest = 0xFFFF; /* Broadcast */
  msg.header.hop_count = 0;
  
  /* Fill discovery-specific fields */
  msg.node_type = NODE_TYPE_BORDER;
  msg.parent = 0; /* No parent for border router */
  msg.hop_to_root = 0; /* Border router is the root */
  msg.energy = energy_level;
  
  /* Send message */
  nullnet_buf = (uint8_t *)&msg;
  nullnet_len = sizeof(msg);
  NETSTACK_NETWORK.output(NULL);
  
  LOG_INFO("Sent discovery message\n");
}

/* Handle received messages from the wireless network */
static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest)
{
  if(len == 0) return;
  
  message_header_t *header = (message_header_t *)data;
  
  /* Process based on message type */
  switch(header->type) {
    case MSG_TYPE_DATA: {
      data_msg_t *msg = (data_msg_t *)data;
      
      LOG_INFO("Received data from sensor %u: %u\n", msg->sensor_id, msg->value);
      
      /* Forward to server */
      send_to_server(data, len);
      break;
    }
    
    case MSG_TYPE_ENERGY_STATUS: {
      energy_msg_t *msg = (energy_msg_t *)data;
      
      LOG_INFO("Received energy status from node %u: %u\n", 
               msg->header.source, msg->energy);
      
      /* Forward to server */
      send_to_server(data, len);
      break;
    }
    
    case MSG_TYPE_JOIN: {
      join_msg_t *msg = (join_msg_t *)data;
      
      LOG_INFO("Node %u joined as child\n", msg->header.source);
      break;
    }
    
    default:
      /* Ignore other message types */
      break;
  }
}

/* Send data to the server over serial */
static void send_to_server(const void *data, uint16_t len)
{
  message_header_t *header = (message_header_t *)data;
  
  /* Format depends on message type */
  switch(header->type) {
    case MSG_TYPE_DATA: {
      data_msg_t *msg = (data_msg_t *)data;
      printf("DATA %u %u %lu\n", msg->sensor_id, msg->value, (unsigned long)msg->timestamp);
      break;
    }
    
    case MSG_TYPE_ENERGY_STATUS: {
      energy_msg_t *msg = (energy_msg_t *)data;
      printf("ENERGY %u %u\n", msg->header.source, msg->energy);
      break;
    }
    
    default:
      /* Don't forward other message types */
      break;
  }
}

/* Process commands from the server */
static void process_server_command(const char *cmd)
{
  char cmd_type[16];
  uint16_t sensor_id, duration;
  uint8_t command;
  command_msg_t msg;
  linkaddr_t dest_addr;
  
  /* Parse command */
  if(sscanf(cmd, "%15s %hu %hhu %hu", cmd_type, &sensor_id, &command, &duration) >= 3) {
    if(strcmp(cmd_type, "COMMAND") == 0) {
      /* Fill message header */
      msg.header.type = MSG_TYPE_COMMAND;
      msg.header.source = BORDER_ROUTER_ID;
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
  }
}
