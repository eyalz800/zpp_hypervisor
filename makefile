.PHONY: \
	all \
	build \
	clean \
	environment \
	environment_clean \
	environment_mkdir \
	linux_loader \
	linux_loader_clean \
	windows_loader \
	windows_loader_clean \
	uefi_loader \
	uefi_loader_clean \
	hypervisor \
	hypervisor_clean \
	detect_visual_studio_root \
	detect_windows_kits_root \
	detect_windows_kits_version

all: build

mode ?= debug
CONFIGURATION ?= $(mode)
MAKEFILE_DIRECTORY := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

include environment.config

TARGET_TYPE := $(SELECTED_ARCHITECTURE)

ifeq ($(OS), Windows_NT)
	C_DIRECTORY := C:
else
	C_DIRECTORY := /mnt/c
endif

DETECT_ENVIRONMENTS :=
DETECTED_VISUAL_STUDIO_ROOT :=
DETECTED_WINDOWS_KITS_ROOT :=
DETECTED_WINDOWS_KITS_VERSION :=

ifneq ($(filter $(BUILD_DRIVERS), windows uefi), )
	ifeq ($(VISUAL_STUDIO_ROOT), )
		DETECT_ENVIRONMENTS += detect_visual_studio_root
		DETECTED_VISUAL_STUDIO_ROOT := $(shell \
			find "$(C_DIRECTORY)/Program Files (x86)/Microsoft Visual Studio" \
			-name cstddef | sort | tail -n 1 | \
			xargs -d '\n' dirname | xargs -d '\n' dirname \
		)
	endif

	ifeq ($(WINDOWS_KITS_ROOT), )
		DETECT_ENVIRONMENTS += detect_windows_kits_root
		MAJOR_WINDOWS_KITS_VERSION := $(shell \
			find "$(C_DIRECTORY)/Program Files (x86)/Windows Kits" \
			-maxdepth 1 | awk -F / '{print $$NF}' | grep -o '[0-9\.]*' | \
			sort -n | tail -n 1 \
		)
		DETECTED_WINDOWS_KITS_ROOT := \
			$(C_DIRECTORY)/Program Files (x86)/Windows Kits/$(MAJOR_WINDOWS_KITS_VERSION)
	endif

	ifeq ($(WINDOWS_KITS_VERSION), )
		DETECT_ENVIRONMENTS += detect_windows_kits_version
		DETECTED_WINDOWS_KITS_VERSION := $(shell \
			find "$(C_DIRECTORY)/Program Files (x86)/Windows Kits" \
			-name wdm.h | xargs -d '\n' dirname | xargs -d '\n' dirname | \
			awk -F / '{print $$NF}' | grep -o '[0-9\.]*' | sort -n | tail -n 1 \
		)
	endif
endif

build: $(patsubst %, %_loader, $(BUILD_DRIVERS)) hypervisor environment

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

hypervisor: | environment.config environment
	@$(MAKE) -s -C hypervisor

hypervisor_clean:
	@$(MAKE) -s -C hypervisor clean

environment_mkdir:
	@mkdir -p environment

environment: $(DETECT_ENVIRONMENTS) \
	$(patsubst environment_templates/%, environment/%, $(shell find "environment_templates" -type f))

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

ifneq ($(DETECTED_VISUAL_STUDIO_ROOT), )
detect_visual_studio_root:
	@echo "Detected Visual Studio Root: '$(DETECTED_VISUAL_STUDIO_ROOT)'." && \
	echo VISUAL_STUDIO_ROOT := "\"$(DETECTED_VISUAL_STUDIO_ROOT)\"" >> environment.config
else
detect_visual_studio_root:
endif

ifneq ($(DETECTED_WINDOWS_KITS_ROOT), )
detect_windows_kits_root:
	@echo "Detected Windows Kits Root: '$(DETECTED_WINDOWS_KITS_ROOT)'." && \
	echo WINDOWS_KITS_ROOT := "\"$(DETECTED_WINDOWS_KITS_ROOT)\"" >> environment.config
else
detect_windows_kits_root:
endif

ifneq ($(DETECTED_WINDOWS_KITS_VERSION), )
detect_windows_kits_version:
	@echo "Detected Windows Kits version: '$(DETECTED_WINDOWS_KITS_VERSION)'." && \
	echo WINDOWS_KITS_VERSION := "$(DETECTED_WINDOWS_KITS_VERSION)" >> environment.config
endif

