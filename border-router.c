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
  PROCESS_BEGIN();
  
  /* Initialize NullNet */
  nullnet_set_input_callback(receive_callback);
  
  /* Start discovery process */
  process_start(&discovery_process, NULL);
  
  LOG_INFO("Border router started\n");
  
  /* Main loop */
  while(1) {
    PROCESS_YIELD();
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
  
  LOG_INFO("Sent discovery message\n");
}

/* Handle received messages from the wireless network */
static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest)
{
  if(len == 0) return;
  
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
