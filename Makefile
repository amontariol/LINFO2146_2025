CONTIKI = /home/lingi2146/contiki-ng
PROJECT_CONF_PATH = ./project-conf.h

# Define the project name
CONTIKI_PROJECT = sensor-node computation-node border-router

# Include nullnet module
MODULES += os/net/nullnet

# Include Contiki-NG main Makefile
include $(CONTIKI)/Makefile.include
