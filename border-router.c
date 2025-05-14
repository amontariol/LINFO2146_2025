#include "contiki.h"
#include "net/nullnet/nullnet.h"
#include "net/netstack.h"
#include "sys/log.h"
#include <string.h>
#include <stdio.h>
#include "sys/node-id.h"

#define LOG_MODULE "BorderRouter"
#define LOG_LEVEL LOG_LEVEL_INFO

#define BORDER_ROUTER_ID       1
#define DISCOVERY_INTERVAL     (CLOCK_SECOND * 30)

static uint16_t energy_level = 1000;

PROCESS(border_router_process, "Border Router Process");
PROCESS(discovery_process, "Discovery Process");
AUTOSTART_PROCESSES(&border_router_process);

static void send_discovery(void);
static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest);

PROCESS_THREAD(border_router_process, ev, data)
{
  PROCESS_BEGIN();

  nullnet_set_input_callback(receive_callback);
  process_start(&discovery_process, NULL);

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

static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest)
{
  if(len == 0) return;
  if(src->u8[0] == node_id) return;

  LOG_INFO("Received message from node %u, length %u\n", src->u8[0], len);

  printf("DATA ");
  for(int i = 0; i < len; i++) {
    printf("%02x ", ((uint8_t *)data)[i]);
  }
  printf("\n");
}
