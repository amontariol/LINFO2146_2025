#include "contiki.h"
#include "net/nullnet/nullnet.h"
#include "net/netstack.h"
#include "lib/random.h"
#include "sys/log.h"
#include "dev/leds.h"
#include <string.h>
#include <stdio.h>
#include "sys/node-id.h"
#include "sys/energest.h"

#define LOG_MODULE "SensorNode"
#define LOG_LEVEL LOG_LEVEL_INFO

#define SENSOR_NODE_ID         (node_id)
#define DISCOVERY_INTERVAL     (CLOCK_SECOND * 60)
#define SENSOR_READ_INTERVAL   (CLOCK_SECOND * 60)
#define VALVE_DURATION         60 // 1 minute

static uint16_t parent_id = 0xFFFF;
static uint8_t hop_to_root = 0xFF;
//static uint8_t valve_open = 0;

PROCESS(sensor_node_process, "Sensor Node Process");
PROCESS(discovery_process, "Discovery Process");
PROCESS(data_process, "Data Process");
PROCESS(energest_process, "Energest Process");

AUTOSTART_PROCESSES(&sensor_node_process);

static void send_discovery(void);
static void send_data(uint16_t value);
static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest);
static uint16_t generate_sensor_data(void);

PROCESS_THREAD(sensor_node_process, ev, data)
{
  PROCESS_BEGIN();

  nullnet_set_input_callback(receive_callback);
  process_start(&discovery_process, NULL);
  process_start(&data_process, NULL);
  process_start(&energest_process, NULL);
  leds_init();

  LOG_INFO("Sensor node %u started\n", SENSOR_NODE_ID);

  PROCESS_END();
}

PROCESS_THREAD(energest_process, ev, data)
{
  static struct etimer timer;
  PROCESS_BEGIN();
  etimer_set(&timer, CLOCK_SECOND * 60); // every minute
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));
    energest_flush();
    unsigned long cpu = energest_type_time(ENERGEST_TYPE_CPU);
    unsigned long lpm = energest_type_time(ENERGEST_TYPE_LPM);
    unsigned long tx = energest_type_time(ENERGEST_TYPE_TRANSMIT);
    unsigned long rx = energest_type_time(ENERGEST_TYPE_LISTEN);
    LOG_INFO("ENERGY: cpu %lu lpm %lu tx %lu rx %lu\n", cpu, lpm, tx, rx);
    etimer_reset(&timer);
  }
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

PROCESS_THREAD(data_process, ev, data)
{
  static struct etimer data_timer;
  static uint16_t sensor_value;
  PROCESS_BEGIN();

  // Start with a shorter interval to get data flowing quickly
  etimer_set(&data_timer, CLOCK_SECOND * 10 + 
            (random_rand() % (CLOCK_SECOND * 5)));

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&data_timer));
    if(parent_id != 0xFFFF) {
      sensor_value = generate_sensor_data();
      send_data(sensor_value);
      LOG_INFO("Sent sensor reading: %u\n", sensor_value);
    } else {
      LOG_INFO("No parent found, cannot send data\n");
    }
    etimer_set(&data_timer, SENSOR_READ_INTERVAL);
  }

  PROCESS_END();
}

static void send_discovery(void)
{
  energest_flush();
  unsigned long total = energest_type_time(ENERGEST_TYPE_CPU) +
                        energest_type_time(ENERGEST_TYPE_LPM) +
                        energest_type_time(ENERGEST_TYPE_TRANSMIT) +
                        energest_type_time(ENERGEST_TYPE_LISTEN);
  uint16_t energy_metric = (uint16_t)(0xFFFF - (total >> 8));
  static uint8_t discovery_msg[4];
  discovery_msg[0] = 1;
  discovery_msg[1] = SENSOR_NODE_ID;
  discovery_msg[2] = hop_to_root;
  discovery_msg[3] = energy_metric >> 8;

  LOG_INFO("Sending discovery: id=%u, hop=%u, energy=%u\n",
           discovery_msg[1], discovery_msg[2], discovery_msg[3]);

  nullnet_buf = discovery_msg;
  nullnet_len = sizeof(discovery_msg);
  NETSTACK_NETWORK.output(NULL);

  LOG_INFO("Sent discovery message\n");
}

static void send_data(uint16_t value)
{
  static uint8_t data_msg[6];
  linkaddr_t parent_addr;
  data_msg[0] = 3;
  data_msg[1] = SENSOR_NODE_ID;
  data_msg[2] = parent_id;
  data_msg[3] = 0;
  data_msg[4] = value >> 8;
  data_msg[5] = value & 0xFF;
  parent_addr.u8[0] = parent_id;
  parent_addr.u8[1] = 0;
  nullnet_buf = data_msg;
  nullnet_len = sizeof(data_msg);
  NETSTACK_NETWORK.output(&parent_addr);
}

static void receive_callback(const void *data, uint16_t len, 
                            const linkaddr_t *src, const linkaddr_t *dest)
{
  if(len == 0) return;
  if(src->u8[0] == node_id) return;

  uint8_t *msg = (uint8_t *)data;
  uint8_t msg_type = msg[0];

  switch(msg_type) {
    case 1: // Discovery
      if(len >= 4) {
        uint8_t source_id = msg[1];
        uint8_t hop_count = msg[2];
        uint8_t energy = msg[3];

        energest_flush();
        unsigned long my_total = energest_type_time(ENERGEST_TYPE_CPU) +
                                energest_type_time(ENERGEST_TYPE_LPM) +
                                energest_type_time(ENERGEST_TYPE_TRANSMIT) +
                                energest_type_time(ENERGEST_TYPE_LISTEN);
        uint16_t my_energy_metric = (uint16_t)(0xFFFF - (my_total >> 8));

        LOG_INFO("Received discovery from node %u (hop %u, energy %u)\n", 
                source_id, hop_count, energy);
        LOG_INFO("Parent selection: my_energy=%u, received_energy=%u, hop=%u, my_hop=%u\n",
         my_energy_metric >> 8, energy, hop_count, hop_to_root);
        if(hop_count < hop_to_root || 
          (hop_count == hop_to_root && energy > (my_energy_metric >> 8))) {
          LOG_INFO("Better parent found! Old: %u (hop %u), New: %u (hop %u)\n",
                  parent_id, hop_to_root, source_id, hop_count + 1);
          parent_id = source_id;
          hop_to_root = hop_count + 1;
          LOG_INFO("Selected new parent %u (hop count: %u)\n", 
                  parent_id, hop_to_root);
        } else {
          LOG_INFO("Kept current parent %u (hop %u, my_energy=%u >= received_energy=%u)\n",
           parent_id, hop_to_root, my_energy_metric >> 8, energy);
        }
      }
      break;
    case 4: // Command
      if(len >= 4) {
        uint8_t target_id = msg[1];
        uint8_t command = msg[2];
        if(target_id == SENSOR_NODE_ID || target_id == 0 || target_id == 255) {
          LOG_INFO("Received command %u for self\n", command);
          if(command == 1) {
            leds_on(LEDS_GREEN);
            LOG_INFO("Valve opened\n");
          } else {
            leds_off(LEDS_GREEN);
            LOG_INFO("Valve closed\n");
          }
        } else {
          LOG_INFO("Ignoring command for node %u\n", target_id);
        }
      }
      break;
    case 3: // Data message
      // *** Always forward data messages up the tree, unmodified ***
      if(parent_id != 0xFFFF) {
        linkaddr_t parent_addr;
        parent_addr.u8[0] = parent_id;
        parent_addr.u8[1] = 0;
        uint8_t fwd_buf[32];
        if(len > sizeof(fwd_buf)) len = sizeof(fwd_buf);
        memcpy(fwd_buf, data, len);
        nullnet_buf = fwd_buf;
        nullnet_len = len;
        NETSTACK_NETWORK.output(&parent_addr);
        LOG_INFO("Forwarded data message from sensor %u to parent %u\n", msg[1], parent_id);      }
      break;
    default:
      // Optionally forward other messages
      break;
  }
}

static uint16_t generate_sensor_data(void)
{
  // Generate data with more variation to trigger slope detection
  static uint16_t last_value = 500;
  static uint8_t trend_counter = 0;
  static int8_t trend_direction = 1;
  int16_t change;
  
  // Every few readings, create a significant trend
  if(trend_counter == 0) {
    trend_direction = (random_rand() % 2) ? 1 : -1;
    trend_counter = 3 + (random_rand() % 3); // Trend lasts 3-5 readings
  }
  
  if(trend_counter > 0) {
    // During a trend, make consistent changes in one direction
    change = trend_direction * (30 + (random_rand() % 50));
    trend_counter--;
  } else {
    // Random fluctuations between trends
    change = (random_rand() % 60) - 30;
  }
  
  last_value += change;
  
  // Keep values in reasonable range
  if(last_value < 400) last_value = 400;
  if(last_value > 1000) last_value = 1000;
  
  return last_value;
}
