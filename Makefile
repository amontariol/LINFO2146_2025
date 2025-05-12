# Main Makefile for LINFO2146 project

# Define the project directory
PROJECT_DIR = /home/lingi2146/project/LINFO2146_2025

# Define the Contiki directory
CONTIKI = /home/lingi2146/contiki-ng

# Define the targets
SENSOR_NODE = sensor-node
COMPUTATION_NODE = computation-node
BORDER_ROUTER = border-router

# Source directories
SOURCEDIRS = src
SOURCES_SENSOR = $(SOURCEDIRS)/$(SENSOR_NODE).c
SOURCES_COMPUTATION = $(SOURCEDIRS)/$(COMPUTATION_NODE).c
SOURCES_BORDER = $(SOURCEDIRS)/$(BORDER_ROUTER).c

# Default target
all: $(SENSOR_NODE) $(COMPUTATION_NODE) $(BORDER_ROUTER)

# Target-specific variables
sensor-node: TARGET = cooja
computation-node: TARGET = cooja
border-router: TARGET = cooja

# Build rules
$(SENSOR_NODE):
	$(MAKE) -f $(CONTIKI)/Makefile.include TARGET=$(TARGET) CONTIKI=$(CONTIKI) \
		CONTIKI_PROJECT=$(SENSOR_NODE) PROJECTDIRS=$(SOURCEDIRS) \
		CONTIKI_WITH_RIME=1

$(COMPUTATION_NODE):
	$(MAKE) -f $(CONTIKI)/Makefile.include TARGET=$(TARGET) CONTIKI=$(CONTIKI) \
		CONTIKI_PROJECT=$(COMPUTATION_NODE) PROJECTDIRS=$(SOURCEDIRS) \
		CONTIKI_WITH_RIME=1

$(BORDER_ROUTER):
	$(MAKE) -f $(CONTIKI)/Makefile.include TARGET=$(TARGET) CONTIKI=$(CONTIKI) \
		CONTIKI_PROJECT=$(BORDER_ROUTER) PROJECTDIRS=$(SOURCEDIRS) \
		CONTIKI_WITH_RIME=1

# Clean rule
clean:
	rm -rf obj_*
	rm -f *~ *.bin *.hex *.elf *.map *.o

# Upload rules (if needed)
upload-sensor: $(SENSOR_NODE)
	$(MAKE) -f $(CONTIKI)/Makefile.include TARGET=z1 CONTIKI=$(CONTIKI) \
		CONTIKI_PROJECT=$(SENSOR_NODE) PROJECTDIRS=$(SOURCEDIRS) z1-upload

# Help rule
help:
	@echo "Available targets:"
	@echo "  all                - Build all node types"
	@echo "  $(SENSOR_NODE)     - Build only sensor nodes"
	@echo "  $(COMPUTATION_NODE) - Build only computation nodes"
	@echo "  $(BORDER_ROUTER)   - Build only border router"
	@echo "  clean              - Clean build files"
	@echo "  upload-sensor      - Upload sensor node to Z1 mote"
	@echo "  help               - Show this help message"

.PHONY: all clean help upload-sensor $(SENSOR_NODE) $(COMPUTATION_NODE) $(BORDER_ROUTER)
