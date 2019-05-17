.PHONY: all clean environment environment_clean environment_mkdir linux_loader linux_loader_clean windows_loader windows_loader_clean uefi_loader uefi_loader_clean hypervisor hypervisor_clean

mode ?= debug
CONFIGURATION ?= $(mode)
MAKEFILE_DIRECTORY := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

include environment.config

TARGET_TYPE := $(SELECTED_ARCHITECTURE)

all: $(patsubst %, %_loader, $(BUILD_DRIVERS)) hypervisor environment

clean: linux_loader_clean windows_loader_clean uefi_loader_clean hypervisor_clean environment_clean

linux_loader: | hypervisor
	@echo "Building linux loader..." && \
	$(MAKE) LINUX_KERNEL=$(LINUX_KERNEL) CONFIGURATION=$(CONFIGURATION) TARGET_TYPE=$(TARGET_TYPE) -s -C linux_loader && \
	echo "Built linux loader."

linux_loader_clean:
	@echo "Cleaning linux loader..." && \
	$(MAKE) LINUX_KERNEL=$(LINUX_KERNEL) CONFIGURATION=$(CONFIGURATION) TARGET_TYPE=$(TARGET_TYPE) -s -C linux_loader clean && \
	rm -rf out && \
	echo "Cleaned linux loader."

windows_loader: | hypervisor
	@$(MAKE) -s -C windows_loader

windows_loader_clean:
	@$(MAKE) -s -C windows_loader clean

uefi_loader: | hypervisor
	@$(MAKE) -s -C uefi_loader

uefi_loader_clean:
	@$(MAKE) -s -C uefi_loader clean

hypervisor:
	@$(MAKE) -s -C hypervisor

hypervisor_clean:
	@$(MAKE) -s -C hypervisor clean

environment_mkdir:
	@mkdir -p environment

environment: $(patsubst environment_templates/%, environment/%, $(shell find environment_templates -type f)) 

environment/%: environment_templates/% environment.config | environment_mkdir
	@rm -f $@ && \
	echo "Building $@..." && \
	cp $< environment && \
	sed -i 's@{{project_root}}@$(MAKEFILE_DIRECTORY)@g' $@ && \
	sed -i 's/{{ssh_target}}/$(SSH_TARGET)/g' $@ && \
	sed -i 's/{{ssh_port}}/$(SSH_PORT)/g' $@ && \
	sed -i 's/{{ssh_password}}/$(SSH_PASSWORD)/g' $@ && \
	sed -i 's/{{gdb_server_address}}/$(GDB_SERVER_ADDRESS)/g' $@ && \
	sed -i 's/{{selected_architecture}}/$(SELECTED_ARCHITECTURE)/g' $@ && \
	echo "Built $@."

environment_clean:
	@rm -rf environment

