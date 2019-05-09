.PHONY: all clean environment environment_clean environment_mkdir linux_loader linux_loader_clean windows_loader windows_loader_clean uefi_loader uefi_loader_clean hypervisor hypervisor_clean

mode ?= debug
CONFIGURATION_NAME ?= $(mode)
MAKEFILE_DIRECTORY := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

include environment.config

TARGET_ABI := $(SELECTED_ARCHITECTURE)

all: $(patsubst %, %_loader, $(BUILD_DRIVERS)) hypervisor environment

clean: linux_loader_clean windows_loader_clean uefi_loader_clean hypervisor_clean environment_clean

linux_loader: | hypervisor
	@echo "Building linux loader..." ; \
	mkdir -p out ; \
	mkdir -p out/$(CONFIGURATION_NAME); \
	mkdir -p out/$(CONFIGURATION_NAME)/$(TARGET_ABI) ; \
	$(MAKE) LINUX_KERNEL=$(LINUX_KERNEL) CONFIGURATION_NAME=$(CONFIGURATION_NAME) TARGET_ABI=$(TARGET_ABI) LINUX_KERNEL=$(LINUX_KERNEL) -s -C linux_loader ; \
	echo "Done."

linux_loader_clean:
	@echo "Cleaning linux loader..." ; \
	$(MAKE) LINUX_KERNEL=$(LINUX_KERNEL) CONFIGURATION_NAME=$(CONFIGURATION_NAME) TARGET_ABI=$(TARGET_ABI) LINUX_KERNEL=$(LINUX_KERNEL) -s -C linux_loader clean ; \
	rm -rf out ; \
	echo "Cleaned."

windows_loader: | hypervisor
	@echo "Building windows loader..." ; \
	$(MAKE) -s -C windows_loader ; \
	echo "Done."

windows_loader_clean:
	@echo "Cleaning windows loader..." ; \
	$(MAKE) -s -C windows_loader clean ; \
	echo "Cleaned."

uefi_loader: | hypervisor
	@echo "Building uefi loader..." ; \
	$(MAKE) -s -C uefi_loader ; \
	echo "Done."

uefi_loader_clean:
	@echo "Cleaning uefi loader..." ; \
	$(MAKE) -s -C uefi_loader clean ; \
	echo "Cleaned."

hypervisor:
	@echo "Building hypervisor..." ; \
	$(MAKE) -s -C hypervisor ; \
	echo "Done."

hypervisor_clean:
	@echo "Cleaning windows loader..." ; \
	$(MAKE) -s -C hypervisor clean ; \
	echo "Cleaned."

environment_mkdir:
	@mkdir -p environment

environment: $(patsubst environment_templates/%, environment/%, $(shell find environment_templates -type f)) 

environment/%: environment_templates/% environment.config | environment_mkdir
	@rm -f $@ ; \
	echo "Building $@..." ; \
	cp $< environment ; \
	sed -i 's@{{project_root}}@$(MAKEFILE_DIRECTORY)@g' $@ ; \
	sed -i 's/{{ssh_target}}/$(SSH_TARGET)/g' $@ ; \
	sed -i 's/{{ssh_port}}/$(SSH_PORT)/g' $@ ; \
	sed -i 's/{{ssh_password}}/$(SSH_PASSWORD)/g' $@ ; \
	sed -i 's/{{gdb_server_address}}/$(GDB_SERVER_ADDRESS)/g' $@ ; \
	sed -i 's/{{selected_architecture}}/$(SELECTED_ARCHITECTURE)/g' $@ ; \
	echo "Built $@."

environment_clean:
	@rm -rf environment

