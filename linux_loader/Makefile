mode ?= debug
CONFIGURATION ?= $(mode)

.PHONY: all clean
MAKEFILE_DIRECTORY := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
include $(MAKEFILE_DIRECTORY)/../environment.config

TARGET_TYPE ?= $(SELECTED_ARCHITECTURE)

PWD := $(shell pwd)
obj-m += zpp_loader.o 
zpp_loader-objs := ./src/main.o ./out/$(CONFIGURATION)/$(TARGET_TYPE)/zpp_loader.o

all: 
	@$(MAKE) -s -f ../zpp.mk && \
	echo cmd_$(MAKEFILE_DIRECTORY)/./out/$(CONFIGURATION)/$(TARGET_TYPE)/zpp_loader.o: \
	$(MAKE) -s -C $(MAKEFILE_DIRECTORY) -f $(MAKEFILE_DIRECTORY)/../zpp.mk \
	> $(MAKEFILE_DIRECTORY)/out/$(CONFIGURATION)/$(TARGET_TYPE)/.zpp_loader.o.cmd && \
	echo "Building linux kernel driver..." && \
	$(MAKE) -s -C /lib/modules/$(LINUX_KERNEL)/build M=$(PWD) modules && \
	echo "Built linux kernel driver." && \
	mkdir -p ../out/$(CONFIGURATION)/$(TARGET_TYPE) && \
	cp zpp_loader.ko ../out/$(CONFIGURATION)/$(TARGET_TYPE)

clean:
	@$(MAKE) -s -f ../zpp.mk clean && \
	echo "Cleaning linux kernel driver..." && \
	$(MAKE) -s -C /lib/modules/$(LINUX_KERNEL)/build M=$(PWD) clean && \
	echo "Cleaned linux kernel driver." && \
	rm -rf $(MAKEFILE_DIRECTORY)/../out/debug/$(TARGET_TYPE)/zpp_loader.ko && \
	rm -rf $(MAKEFILE_DIRECTORY)/../out/release/$(TARGET_TYPE)/zpp_loader.ko && \
	rm -rf $(MAKEFILE_DIRECTORY)/out/debug/$(TARGET_TYPE)/.zpp_loader.o.cmd && \
	rm -rf $(MAKEFILE_DIRECTORY)/out/release/$(TARGET_TYPE)/.zpp_loader.o.cmd && \
	find ../out -type d -empty -delete 2> /dev/null; \
	ignore_failure=
