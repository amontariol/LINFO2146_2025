/* sensor-node.c */
#include "contiki.h"
#include "net/nullnet/nullnet.h"
#include "net/netstack.h"
#include "lib/random.h"
#include "sys/log.h"
#include "dev/leds.h"
#include <string.h>
#include <stdio.h>
#include "sys/node-id.h"


#define LOG_MODULE "SensorNode"
#define LOG_LEVEL LOG_LEVEL_INFO

/* Configuration */
#define SENSOR_NODE_ID         (node_id)
#define DISCOVERY_INTERVAL     (CLOCK_SECOND * 60)
#define SENSOR_READ_INTERVAL   (CLOCK_SECOND * 60)
#define VALVE_DURATION 60 // 1 minute


/* Global variables */
static uint16_t parent_id = 0xFFFF;
static uint8_t hop_to_root = 0xFF;
static uint16_t energy_level = 1000;

/* Process definitions */
PROCESS(sensor_node_process, "Sensor Node Process");
PROCESS(discovery_process, "Discovery Process");
PROCESS(data_process, "Data Process");
AUTOSTART_PROCESSES(&sensor_node_process);

/* Function prototypes */
static void send_discovery(void);
static void send_data(uint16_t value);
static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest);
static uint16_t generate_sensor_data(void);

/* Main process */
PROCESS_THREAD(sensor_node_process, ev, data)
{
  PROCESS_BEGIN();
  
  /* Initialize NullNet */
  nullnet_set_input_callback(receive_callback);
  
  /* Start other processes */
  process_start(&discovery_process, NULL);
  process_start(&data_process, NULL);
  
  /* Initialize LED */
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
    
    /* Reset timer */
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

/* Send discovery message to find potential parents */
static void send_discovery(void)
{
  /* Simple discovery message */
  static uint8_t discovery_msg[4];
  
  /* Fill message */
  discovery_msg[0] = 1; /* Message type: discovery */
  discovery_msg[1] = SENSOR_NODE_ID; /* Source ID */
  discovery_msg[2] = hop_to_root; /* Hop count to root */
  discovery_msg[3] = energy_level >> 8; /* Energy level (high byte) */

  LOG_INFO("Sending discovery: type=%u, id=%u, hop=%u, energy=%u\n",
         discovery_msg[0], discovery_msg[1], discovery_msg[2], discovery_msg[3]);
  
  /* Send message */
  nullnet_buf = discovery_msg;
  nullnet_len = sizeof(discovery_msg);
  NETSTACK_NETWORK.output(NULL);

  
  
  LOG_INFO("Sent discovery message\n");
}

/* Send sensor data to parent */
static void send_data(uint16_t value)
{
  /* Simple data message */
  static uint8_t data_msg[6];
  linkaddr_t parent_addr;
  
  /* Fill message */
  data_msg[0] = 3; /* Message type: data */
  data_msg[1] = SENSOR_NODE_ID; /* Source ID */
  data_msg[2] = parent_id; /* Destination ID */
  data_msg[3] = 0; /* Hop count */
  data_msg[4] = value >> 8; /* Value high byte */
  data_msg[5] = value & 0xFF; /* Value low byte */
  
  /* Set parent address */
  parent_addr.u8[0] = parent_id;
  parent_addr.u8[1] = 0;
  
  /* Send message */
  nullnet_buf = data_msg;
  nullnet_len = sizeof(data_msg);
  NETSTACK_NETWORK.output(&parent_addr);
}

/* Handle received messages */
static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest)
{
  if(len == 0) return;
  LOG_INFO("RECEIVED MESSAGE from %u, length %u\n", src->u8[0], len);

  /* Ignore messages from self */
  if(src->u8[0] == node_id) {
    LOG_INFO("Ignoring message from self\n");
    return;
  }
  
  uint8_t *msg = (uint8_t *)data;
  uint8_t msg_type = msg[0];
  LOG_INFO("Message type: %u\n", msg_type);
  
  /* Process based on message type */
  switch(msg_type) {
    case 1: /* Discovery message */
      if(len >= 4) {
        uint8_t source_id = msg[1];
        uint8_t hop_count = msg[2];
        uint8_t energy = msg[3];

        LOG_INFO("Received discovery from node %u (hop count: %u, energy: %u)\n", 
                 source_id, hop_count, energy);
        
        /* If this is a better parent */
        if(hop_count < hop_to_root || 
           (hop_count == hop_to_root && energy > (energy_level >> 8))) {
          /* Update parent */
          LOG_INFO("Better parent found! Old: %u (hop %u), New: %u (hop %u)\n",
                   parent_id, hop_to_root, source_id, hop_count + 1);
          parent_id = source_id;
          hop_to_root = hop_count + 1;
          
          LOG_INFO("Selected new parent %u (hop count: %u)\n", 
                   parent_id, hop_to_root);
        }else {
          LOG_INFO("Not a better parent, ignoring\n");
        }

      }
      break;
    
    case 4: /* Command message */
      if(len >= 4 && msg[1] == SENSOR_NODE_ID) {
        uint8_t command = msg[2];
        
        if(command == 1) {
          /* Open valve (turn on LED) */
          leds_on(LEDS_GREEN);
          LOG_INFO("Valve opened\n");
        } else {
          /* Close valve (turn off LED) */
          leds_off(LEDS_GREEN);
          LOG_INFO("Valve closed\n");
        }
      }
      break;
    
    default:
      /* Ignore other message types */
      break;
  }
}

/* Generate fake sensor data */
static uint16_t generate_sensor_data(void)
{
  /* Generate a value between 400-1000 (representing CO2 ppm) */
  return 400 + (random_rand() % 600);
}
