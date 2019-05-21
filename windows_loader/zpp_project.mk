ifeq ($(ZPP_PROJECT_SETTINGS), true)

include ../environment.config

ZPP_TARGET_NAME := zpp_loader.sys
ZPP_TARGET_TYPES := $(SUPPORTED_ARCHITECTURES)
ZPP_LINK_TYPE := default
ZPP_OUTPUT_DIRECTORY_ROOT := ../out
ZPP_SOURCE_DIRECTORIES := \
	./src \
	../loader/src
ZPP_SOURCE_FILES :=

endif

ifeq ($(ZPP_PROJECT_FLAGS), true)
ZPP_FLAGS := \
	$(patsubst %, -I%, $(shell find . -type d -name "include")) \
	$(patsubst %, -I%, $(shell find ../hypervisor -type d -name "include")) \
	-pedantic \
	-Wall \
	-Wextra \
	-Werror \
	-DZPP_ELF_BINARY_NO_HIDDEN \
	-DZPP_ELF_BINARY_PATH="\"$(ZPP_OUTPUT_DIRECTORY)/zpp_hypervisor\"" \
	-nostdlib \
	-mno-red-zone \
	-ffreestanding
ZPP_FLAGS_DEBUG :=
ZPP_FLAGS_RELEASE := \
	-O2 \
	-ffunction-sections \
	-fdata-sections \
	-fvisibility=hidden
ZPP_CFLAGS := \
	$(ZPP_FLAGS) \
	-std=c11
ZPP_CFLAGS_DEBUG := \
	$(ZPP_FLAGS_DEBUG)
ZPP_CFLAGS_RELEASE := \
	$(ZPP_FLAGS_RELEASE)
ZPP_CXXFLAGS := \
	$(ZPP_FLAGS) \
	-fno-exceptions \
	-fno-rtti \
	-std=c++17
ZPP_CXXFLAGS_DEBUG := \
	$(ZPP_FLAGS_DEBUG)
ZPP_CXXFLAGS_RELEASE := \
	$(ZPP_FLAGS_RELEASE)
ZPP_ASFLAGS := \
	$(ZPP_FLAGS) \
	-x assembler-with-cpp
ASFLAGS_DEBUG := \
	$(ZPP_FLAGS_DEBUG)
ASFLAGS_RELEASE := \
	$(ZPP_FLAGS_RELEASE)
ZPP_LFLAGS := \
	$(ZPP_FLAGS) \
	-Wl,-subsystem:native \
	-Wl,/driver:wdm \
	-Wl,/dynamicbase \
	-Wl,/nodefaultlib \
	-Wl,/dll \
	-Wl,ntoskrnl.lib \
	-Wl,-entry:driver_entry
ZPP_LFLAGS_DEBUG := \
	$(ZPP_FLAGS_DEBUG)
ZPP_LFLAGS_RELEASE := \
	$(ZPP_FLAGS_RELEASE) \
	-Wl,--strip-all \
	-Wl,--gc-sections
endif

ifeq ($(ZPP_PROJECT_RULES), true)

$(ZPP_INTERMEDIATE_DIRECTORY)/../loader/src/elf_binary.o: \
	$(ZPP_OUTPUT_DIRECTORY)/zpp_hypervisor

endif

ifeq ($(ZPP_TOOLCHAIN_SETTINGS), true)
	include zpp_toolchain.mk
endif
