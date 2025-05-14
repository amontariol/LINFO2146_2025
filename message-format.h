/* message-format.h */
#ifndef MESSAGE_FORMAT_H
#define MESSAGE_FORMAT_H

#include "contiki.h"
#include <stdint.h>

/* Message types */
#define MSG_TYPE_DISCOVERY     1
#define MSG_TYPE_JOIN          2
#define MSG_TYPE_DATA          3
#define MSG_TYPE_COMMAND       4
#define MSG_TYPE_ENERGY_STATUS 5

/* Node types */
#define NODE_TYPE_SENSOR       1
#define NODE_TYPE_COMPUTATION  2
#define NODE_TYPE_BORDER       3

/* Common header for all messages */
typedef struct {
  uint8_t type;       /* Message type */
  uint16_t source;    /* Source node ID */
  uint16_t dest;      /* Destination node ID (0xFFFF for broadcast) */
  uint8_t hop_count;  /* Number of hops traveled */
} message_header_t;

/* Discovery message */
typedef struct {
  message_header_t header;
  uint8_t node_type;  /* Type of the broadcasting node */
  uint16_t parent;    /* Parent node ID (0xFFFF if no parent) */
  uint8_t hop_to_root; /* Hops to the root node */
  uint16_t energy;    /* Energy level (0-1000) */
} discovery_msg_t;

/* Join message */
typedef struct {
  message_header_t header;
  uint16_t parent;    /* Selected parent node ID */
} join_msg_t;

/* Data message */
typedef struct {
  message_header_t header;
  uint16_t sensor_id; /* ID of the sensor node */
  uint16_t value;     /* Sensor reading */
  uint32_t timestamp; /* Timestamp of the reading */
} data_msg_t;

/* Command message */
typedef struct {
  message_header_t header;
  uint16_t sensor_id; /* Target sensor node ID */
  uint8_t command;    /* 1 = open valve, 0 = close valve */
  uint16_t duration;  /* Duration in seconds (0 = indefinite) */
} command_msg_t;

/* Energy status message */
typedef struct {
  message_header_t header;
  uint16_t energy;    /* Energy level (0-1000) */
} energy_msg_t;

#endif /* MESSAGE_FORMAT_H */
