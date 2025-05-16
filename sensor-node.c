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

#define SENSOR_NODE_ID (node_id)
#define DISCOVERY_INTERVAL (CLOCK_SECOND * 60)
#define SENSOR_READ_INTERVAL (CLOCK_SECOND * 60)
#define VALVE_DURATION 60 // 1 minute
#define MAX_CHILDREN 10

static uint8_t parent_id = 0xFF;
static uint8_t hop_to_root = 0xFF;
static uint8_t children[MAX_CHILDREN];
static uint8_t child_count = 0;
static uint8_t valve_open = 0;

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
static void add_child(uint8_t child_id);
static uint8_t find_child(uint8_t target_id);

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
  while (1)
  {
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

  while (1)
  {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&discovery_timer));
    send_discovery();
    etimer_set(&discovery_timer, DISCOVERY_INTERVAL +
                                     (random_rand() % (DISCOVERY_INTERVAL / 10)));
  }

  PROCESS_END();
}

PROCESS_THREAD(data_process, ev, data)
{
  static struct etimer data_timer;
  static uint16_t sensor_value;
  PROCESS_BEGIN();

  etimer_set(&data_timer, SENSOR_READ_INTERVAL +
                              (random_rand() % (SENSOR_READ_INTERVAL / 10)));

  while (1)
  {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&data_timer));
    if (parent_id != 0xFFFF)
    {
      sensor_value = generate_sensor_data();
      send_data(sensor_value);
      LOG_INFO("Sent sensor reading: %u\n", sensor_value);
    }
    else
    {
      LOG_INFO("No parent found, cannot send data\n");
    }
    etimer_set(&data_timer, SENSOR_READ_INTERVAL);
  }

  PROCESS_END();
}

static void add_child(uint8_t child_id)
{
  for (int i = 0; i < child_count; i++)
  {
    if (children[i] == child_id)
      return; // Already a child
  }

  if (child_count < MAX_CHILDREN)
  {
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

  send_message(discovery_msg, sizeof(discovery_msg), NULL);

  LOG_INFO("Sent discovery message\n");
}




static void send_data(uint16_t value)
{
  static uint8_t data_msg[6];
  linkaddr_t parent_addr;
  parent_addr.u8[0] = parent_id;
  parent_addr.u8[1] = 0;
  data_msg[0] = 3;
  data_msg[1] = SENSOR_NODE_ID;
  data_msg[2] = parent_id;
  data_msg[3] = 0;
  data_msg[4] = value >> 8;
  data_msg[5] = value & 0xFF;
  send_message(data_msg, sizeof(data_msg), &parent_addr);
}

static void forward_data(const uint8_t *data, uint16_t len)
{
  linkaddr_t parent_addr;
  parent_addr.u8[0] = parent_id;
  parent_addr.u8[1] = 0;
  static uint8_t data_msg[6];
  memcpy(data_msg, data, len);
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

static void receive_callback(const void *data, uint16_t len,
                             const linkaddr_t *src, const linkaddr_t *dest)
{
  if (len == 0)
    return;
  if (src->u8[0] == node_id)
    return;

  uint8_t *msg = (uint8_t *)data;
  uint8_t msg_type = msg[0];
  if (len >= 2)
  {
    uint8_t source_id = msg[1];
    LOG_INFO("RECEIVED MESSAGE from %u, length %u\n", source_id, len);
  }
  else
  {
    LOG_INFO("RECEIVED MESSAGE from %u, length %u\n", src->u8[0], len);
  }

  // Print raw data for debugging
  printf("Message: ");
  for (int i = 0; i < len; i++)
  {
    printf("%02x ", ((uint8_t *)data)[i]);
  }
  printf("\n");

  switch (msg_type)
  {
  case 1: // Discovery
    if (len >= 4)
    {
      uint8_t source_id = msg[1];
      uint8_t hop_count = msg[2];
      uint8_t energy = msg[3];

      // Compute my own energy metric
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
      if (hop_count < hop_to_root ||
          (hop_count == hop_to_root && energy > (my_energy_metric >> 8)))
      {
        LOG_INFO("Better parent found! Old: %u (hop %u), New: %u (hop %u)\n",
                 parent_id, hop_to_root, source_id, hop_count + 1);
        parent_id = source_id;
        hop_to_root = hop_count + 1;
        LOG_INFO("Selected new parent %u (hop count: %u)\n",
                 parent_id, hop_to_root);
      }
      else
      {
        LOG_INFO("Kept current parent %u (hop %u, my_energy=%u >= received_energy=%u)\n",
                 parent_id, hop_to_root, my_energy_metric >> 8, energy);
      }

      // If this is from a child, add to child list
      if (hop_count > hop_to_root)
      {
        add_child(source_id);
      }
    }
    break;
  case 3:
    // forward data to parent
    if (len >= 6)
    {
      uint8_t source_id = msg[1];
      uint16_t value = (msg[4] << 8) | msg[5];
      LOG_INFO("Received data from sensor %u: %u\n", source_id, value);
      if (parent_id != 0xFF)
      {
        forward_data(msg, len);
        // send_data(value);
      }
      else
      {
        LOG_INFO("No parent found, cannot forward data\n");
      }
    }
    break;
  case 4: // Command
    printf("command received from %u\n", src->u8[0]);
    printf("command received for %u\n", msg[1]);
    if (len >= 4)
    {
      uint8_t target_id = msg[1];
      if (target_id == SENSOR_NODE_ID)
      {
        printf("command for me\n");
        // Command is for me
        uint8_t command = msg[2];
        printf("command %u\n", command);
        if (command == 1)
        {
          leds_on(LEDS_GREEN);
          valve_open = 1;
          LOG_INFO("Valve opened\n");
        }
        else
        {
          leds_off(LEDS_GREEN);
          valve_open = 0;
          LOG_INFO("Valve closed\n");
        }
      }
      else
      {
        // Forward the command to the appropriate child
        uint8_t child = find_child(target_id);
        if (child != 0xFF)
        {
          forward_command(msg, len, child);
          printf("command forwarded for %u to %u\n", target_id, child);
          LOG_INFO("Forwarded command for sensor %u to %u\n", target_id, child);
        } else {
          printf("command not forwarded for %u, no child found\n", target_id);
        }
      }
    }
    break;
  default:
    break;
  }
}

static uint16_t generate_sensor_data(void)
{
  return 400 + (random_rand() % 600);
}
