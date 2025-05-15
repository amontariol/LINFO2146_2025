CONTIKI = /home/yonyc/Documents/ucl/contiki-ng
PROJECT_CONF_PATH = ./project-conf.h

# Define the project name
CONTIKI_PROJECT = sensor-node computation-node border-router

# Include nullnet module
MODULES += os/net/nullnet

CSMA_CONF_MAX_NEIGHBOR_QUEUES = 4

# Include Contiki-NG main Makefile
include $(CONTIKI)/Makefile.include