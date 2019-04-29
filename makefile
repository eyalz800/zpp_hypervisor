.PHONY: all clean environment environment_clean environment_mkdir linux_loader_all linux_loader_clean hypervisor_all hypervisor_clean

mode ?= debug
CONFIGURATION_NAME ?= $(mode)
TARGET_ABI := native
MAKEFILE_DIRECTORY := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

include environment.config

all: linux_loader_all hypervisor_all environment

clean: linux_loader_clean hypervisor_clean environment_clean

linux_loader_all: | hypervisor_all
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

hypervisor_all:
	@$(MAKE) -s -C hypervisor

hypervisor_clean:
	@$(MAKE) -s -C hypervisor clean

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
	echo "Built $@."

environment_clean:
	@rm -r environment

