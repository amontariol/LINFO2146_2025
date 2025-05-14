/* border-router.c */
#include "contiki.h"
#include "net/nullnet/nullnet.h"
#include "net/netstack.h"
#include "sys/log.h"
#include <string.h>
#include <stdio.h>
#include "sys/node-id.h"


#define LOG_MODULE "BorderRouter"
#define LOG_LEVEL LOG_LEVEL_INFO

/* Configuration */
#define BORDER_ROUTER_ID       1
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

/* Main process */
PROCESS_THREAD(border_router_process, ev, data)
{
  static struct etimer test_timer;
  
  PROCESS_BEGIN();
  
  /* Initialize NullNet */
  nullnet_set_input_callback(receive_callback);
  
  /* Start discovery process */
  process_start(&discovery_process, NULL);
  
  LOG_INFO("Border router started\n");
  
  /* Set up test timer */
  etimer_set(&test_timer, CLOCK_SECOND * 5);
  
  /* Main loop */
  while(1) {
    PROCESS_WAIT_EVENT();
    
    if(etimer_expired(&test_timer)) {
      /* Send a simple test message */
      static uint8_t test_msg[2] = {99, BORDER_ROUTER_ID};
      nullnet_buf = test_msg;
      nullnet_len = sizeof(test_msg);
      NETSTACK_NETWORK.output(NULL);
      
      LOG_INFO("Sent test message\n");
      
      etimer_reset(&test_timer);
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
  /* Simple discovery message */
  static uint8_t discovery_msg[4];
  
  /* Fill message */
  discovery_msg[0] = 1; /* Message type: discovery */
  discovery_msg[1] = BORDER_ROUTER_ID; /* Source ID */
  discovery_msg[2] = 0; /* Hop count to root: 0 */
  discovery_msg[3] = energy_level >> 8; /* Energy level (high byte) */
  
  /* Send message */
  nullnet_buf = discovery_msg;
  nullnet_len = sizeof(discovery_msg);
  NETSTACK_NETWORK.output(NULL);

  LOG_INFO("Sending discovery: type=%u, id=%u, hop=%u, energy=%u\n",
         discovery_msg[0], discovery_msg[1], discovery_msg[2], discovery_msg[3]);
  
  LOG_INFO("Sent discovery message\n");
}

/* Handle received messages from the wireless network */
static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest)
{
  if(len == 0) return;

  /* Ignore messages from self */
  if(src->u8[0] == node_id) {
    LOG_INFO("Ignoring message from self\n");
    return;
  }
  
  /* Just log the received message */
  LOG_INFO("Received message from node %u, length %u\n", 
           src->u8[0], len);
  
  /* Print message content */
  printf("DATA ");
  for(int i = 0; i < len; i++) {
    printf("%02x ", ((uint8_t *)data)[i]);
  }
  printf("\n");
}
