/* sensor-node.c */
#include "contiki.h"
#include "net/nullnet/nullnet.h"
#include "net/netstack.h"
#include "lib/random.h"
#include "sys/log.h"
#include "dev/leds.h"
#include "dev/button-sensor.h"
#include "sys/energest.h"
#include "message-format.h"
#include <string.h>
#include <stdio.h>

#define LOG_MODULE "SensorNode"
#define LOG_LEVEL LOG_LEVEL_INFO

/* Configuration */
#define SENSOR_NODE_ID         (linkaddr_node_addr.u8[0])
#define BROADCAST_CHANNEL      129
#define DATA_CHANNEL           130
#define DISCOVERY_INTERVAL     (CLOCK_SECOND * 60)
#define SENSOR_READ_INTERVAL   (CLOCK_SECOND * 60)
#define ENERGY_REPORT_INTERVAL (CLOCK_SECOND * 300)
#define VALVE_DURATION         (CLOCK_SECOND * 600) /* 10 minutes */

/* Global variables */
static uint16_t parent_id = 0xFFFF;
static uint8_t hop_to_root = 0xFF;
static uint16_t energy_level = 1000; /* Start with full energy */
static uint8_t valve_status = 0;     /* 0 = closed, 1 = open */
static struct ctimer valve_timer;

/* Process definitions */
PROCESS(sensor_node_process, "Sensor Node Process");
PROCESS(discovery_process, "Discovery Process");
PROCESS(data_process, "Data Process");
PROCESS(energy_process, "Energy Process");
AUTOSTART_PROCESSES(&sensor_node_process);

/* Function prototypes */
static void send_discovery(void);
static void send_join(uint16_t parent);
static void send_data(uint16_t value);
static void send_energy_status(void);
static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest);
static void close_valve(void *ptr);
static uint16_t generate_sensor_data(void);
static void update_energy_level(void);

/* Main process */
PROCESS_THREAD(sensor_node_process, ev, data)
{
  PROCESS_BEGIN();
  
  /* Initialize NullNet */
  nullnet_set_input_callback(receive_callback);
  
  /* Initialize energy tracking */
  energest_init();
  
  /* Start other processes */
  process_start(&discovery_process, NULL);
  process_start(&data_process, NULL);
  process_start(&energy_process, NULL);
  
  /* Initialize LED for valve status */
  leds_init();
  
  LOG_INFO("Sensor node %u started\n", SENSOR_NODE_ID);
  
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

/* Data process - reads sensor and sends data */
PROCESS_THREAD(data_process, ev, data)
{
  static struct etimer data_timer;
  static uint16_t sensor_value;
  
  PROCESS_BEGIN();
  
  /* Set up data timer */
  etimer_set(&data_timer, SENSOR_READ_INTERVAL + 
            (random_rand() % (SENSOR_READ_INTERVAL/10)));
  
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&data_timer));
    
    /* Only send data if we have a parent */
    if(parent_id != 0xFFFF) {
      /* Generate fake sensor data */
      sensor_value = generate_sensor_data();
      
      /* Send data */
      send_data(sensor_value);
      
      LOG_INFO("Sent sensor reading: %u\n", sensor_value);
    } else {
      LOG_INFO("No parent found, cannot send data\n");
    }
    
    /* Reset timer */
    etimer_set(&data_timer, SENSOR_READ_INTERVAL);
  }
  
  PROCESS_END();
}

/* Energy process - monitors and reports energy status */
PROCESS_THREAD(energy_process, ev, data)
{
  static struct etimer energy_timer;
  
  PROCESS_BEGIN();
  
  /* Set up energy timer */
  etimer_set(&energy_timer, ENERGY_REPORT_INTERVAL);
  
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&energy_timer));
    
    /* Update energy level based on usage */
    update_energy_level();
    
    /* Send energy status if we have a parent */
    if(parent_id != 0xFFFF) {
      send_energy_status();
    }
    
    /* Reset timer */
    etimer_set(&energy_timer, ENERGY_REPORT_INTERVAL);
  }
  
  PROCESS_END();
}

/* Send discovery message to find potential parents */
static void send_discovery(void)
{
  discovery_msg_t msg;
  
  /* Fill message header */
  msg.header.type = MSG_TYPE_DISCOVERY;
  msg.header.source = SENSOR_NODE_ID;
  msg.header.dest = 0xFFFF; /* Broadcast */
  msg.header.hop_count = 0;
  
  /* Fill discovery-specific fields */
  msg.node_type = NODE_TYPE_SENSOR;
  msg.parent = parent_id;
  msg.hop_to_root = hop_to_root;
  msg.energy = energy_level;
  
  /* Set NullNet channel for discovery */
  nullnet_set_input_callback(receive_callback);
  
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
  msg.header.source = SENSOR_NODE_ID;
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

/* Send sensor data to parent */
static void send_data(uint16_t value)
{
  data_msg_t msg;
  linkaddr_t parent_addr;
  
  /* Fill message header */
  msg.header.type = MSG_TYPE_DATA;
  msg.header.source = SENSOR_NODE_ID;
  msg.header.dest = parent_id;
  msg.header.hop_count = 0;
  
  /* Fill data-specific fields */
  msg.sensor_id = SENSOR_NODE_ID;
  msg.value = value;
  msg.timestamp = clock_seconds();
  
  /* Set parent address */
  parent_addr.u8[0] = parent_id;
  parent_addr.u8[1] = 0;
  
  /* Send message */
  nullnet_buf = (uint8_t *)&msg;
  nullnet_len = sizeof(msg);
  NETSTACK_NETWORK.output(&parent_addr);
}

/* Send energy status to parent */
static void send_energy_status(void)
{
  energy_msg_t msg;
  linkaddr_t parent_addr;
  
  /* Fill message header */
  msg.header.type = MSG_TYPE_ENERGY_STATUS;
  msg.header.source = SENSOR_NODE_ID;
  msg.header.dest = parent_id;
  msg.header.hop_count = 0;
  
  /* Fill energy-specific fields */
  msg.energy = energy_level;
  
  /* Set parent address */
  parent_addr.u8[0] = parent_id;
  parent_addr.u8[1] = 0;
  
  /* Send message */
  nullnet_buf = (uint8_t *)&msg;
  nullnet_len = sizeof(msg);
  NETSTACK_NETWORK.output(&parent_addr);
  
  LOG_INFO("Sent energy status: %u\n", energy_level);
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
    
    case MSG_TYPE_COMMAND: {
      command_msg_t *msg = (command_msg_t *)data;
      
      /* Check if command is for this node */
      if(msg->sensor_id == SENSOR_NODE_ID) {
        if(msg->command == 1) {
          /* Open valve */
          valve_status = 1;
          leds_on(LEDS_GREEN);
          LOG_INFO("Valve opened for %u seconds\n", msg->duration);
          
          /* Set timer to close valve */
          if(msg->duration > 0) {
            ctimer_set(&valve_timer, msg->duration * CLOCK_SECOND, close_valve, NULL);
          }
        } else {
          /* Close valve */
          valve_status = 0;
          leds_off(LEDS_GREEN);
          LOG_INFO("Valve closed\n");
          
          /* Cancel any pending timer */
          ctimer_stop(&valve_timer);
        }
      }
      break;
    }
    
    default:
      /* Ignore other message types */
      break;
  }
}

/* Close valve after timer expires */
static void close_valve(void *ptr)
{
  valve_status = 0;
  leds_off(LEDS_GREEN);
  LOG_INFO("Valve closed (timer expired)\n");
}

/* Generate fake sensor data */
static uint16_t generate_sensor_data(void)
{
  /* Generate a value between 400-1000 (representing CO2 ppm) */
  return 400 + (random_rand() % 600);
}

/* Update energy level based on usage */
static void update_energy_level(void)
{
  unsigned long cpu_time, lpm_time, transmit_time, listen_time;
  
  /* Get energy usage statistics */
  energest_flush();
  
  cpu_time = energest_type_time(ENERGEST_TYPE_CPU);
  lpm_time = energest_type_time(ENERGEST_TYPE_LPM);
  transmit_time = energest_type_time(ENERGEST_TYPE_TRANSMIT);
  listen_time = energest_type_time(ENERGEST_TYPE_LISTEN);
  
  /* Calculate energy consumption (simplified model) */
  /* CPU: 1 unit per second, LPM: 0.1 unit per second */
  /* TX: 10 units per second, RX: 5 units per second */
  unsigned long total_energy = 
    (cpu_time / CLOCK_SECOND) * 1 +
    (lpm_time / CLOCK_SECOND) * 0.1 +
    (transmit_time / CLOCK_SECOND) * 10 +
    (listen_time / CLOCK_SECOND) * 5;
  
  /* Decrease energy level (ensure it doesn't go below 0) */
  if(total_energy > energy_level) {
    energy_level = 0;
  } else {
    energy_level -= total_energy;
  }
  
  LOG_INFO("Energy level: %u\n", energy_level);
}
