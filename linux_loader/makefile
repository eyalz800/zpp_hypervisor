mode ?= debug
CONFIGURATION_NAME ?= $(mode)
TARGET_ABI ?= native

.PHONY: all clean
MAKEFILE_DIRECTORY := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
include $(MAKEFILE_DIRECTORY)/../environment.config

PWD := $(shell pwd)
obj-m += zpp_loader.o 
zpp_loader-objs := ./linux_src/main.o ./out/$(CONFIGURATION_NAME)/$(TARGET_ABI)/zpp_loader.o

all: 
	@$(MAKE) -s -f makefile_loader ; \
	echo cmd_$(MAKEFILE_DIRECTORY)/./out/$(CONFIGURATION_NAME)/$(TARGET_ABI)/zpp_loader.o: \
	$(MAKE) -s -C $(MAKEFILE_DIRECTORY) -f $(MAKEFILE_DIRECTORY)/makefile_loader \
	> $(MAKEFILE_DIRECTORY)/out/$(CONFIGURATION_NAME)/$(TARGET_ABI)/.zpp_loader.o.cmd ; \
	echo "Building linux kernel driver..." ; \
	$(MAKE) -s -C /lib/modules/$(LINUX_KERNEL)/build M=$(PWD) modules ; \
	echo "Built linux kernel driver." ; \
	cp zpp_loader.ko ../out/$(CONFIGURATION_NAME)/$(TARGET_ABI)

clean:
	@$(MAKE) -s -f makefile_loader clean ; \
	echo "Cleaning linux kernel driver..." ; \
	$(MAKE) -s -C /lib/modules/$(LINUX_KERNEL)/build M=$(PWD) clean ; \
	echo "Cleaned linux kernel driver." ; \
	rm -rf $(MAKEFILE_DIRECTORY)/../out/debug/$(TARGET_ABI)/zpp_loader.ko ; \
	rm -rf $(MAKEFILE_DIRECTORY)/../out/release/$(TARGET_ABI)/zpp_loader.ko ; \
	rm -rf $(MAKEFILE_DIRECTORY)/out/debug/$(TARGET_ABI)/.zpp_loader.o.cmd ; \
	rm -rf $(MAKEFILE_DIRECTORY)/out/release/$(TARGET_ABI)/.zpp_loader.o.cmd
