ifeq ($(ZPP_PROJECT_SETTINGS), true)

include ../environment.config

ZPP_TARGET_NAME := zpp_loader.o
ZPP_TARGET_TYPES := $(SUPPORTED_ARCHITECTURES)
ZPP_LINK_TYPE := default
ZPP_SOURCE_DIRECTORIES := \
	../loader/src
ZPP_SOURCE_FILES :=
ZPP_INCLUDE_PROJECTS :=
ZPP_COMPILE_COMMANDS_JSON := ../loader/compile_commands.json

endif

ifeq ($(ZPP_PROJECT_FLAGS), true)
ZPP_FLAGS := \
	$(patsubst %, -I%, $(shell find . -type d -name "include")) \
	$(patsubst %, -I%, $(shell find ../hypervisor -type d -name "include")) \
	-pedantic \
	-Wall \
	-Wextra \
	-Werror \
	-nostdlib \
	-ffreestanding \
	-mno-red-zone \
	-DZPP_ELF_BINARY_PATH="\"../$(ZPP_OUTPUT_DIRECTORY)/zpp_hypervisor\""
ZPP_FLAGS_DEBUG := \
	-g
ZPP_FLAGS_RELEASE := \
	-O0 \
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
	-std=c++17 \
	-stdlib=libc++ \
	-fno-rtti \
	-fno-threadsafe-statics \
	-fno-exceptions
ZPP_CXXFLAGS_DEBUG := \
	$(ZPP_FLAGS_DEBUG)
ZPP_CXXFLAGS_RELEASE := \
	$(ZPP_FLAGS_RELEASE)
ZPP_ASFLAGS := \
	$(ZPP_FLAGS) \
	-x assembler-with-cpp
ZPP_ASFLAGS_DEBUG := \
	$(ZPP_FLAGS_DEBUG)
ZPP_ASFLAGS_RELEASE := \
	$(ZPP_FLAGS_RELEASE)
ZPP_LFLAGS := \
	$(ZPP_FLAGS) \
	-r \
	-Wl,--no-undefined
ZPP_LFLAGS_DEBUG := \
	$(ZPP_FLAGS_DEBUG)
ZPP_LFLAGS_RELEASE := \
	$(ZPP_FLAGS_RELEASE)
endif

ifeq ($(ZPP_PROJECT_RULES), true)

$(ZPP_INTERMEDIATE_DIRECTORY)/../loader/src/elf_binary.o: \
	../$(ZPP_OUTPUT_DIRECTORY)/zpp_hypervisor

endif

ifeq ($(ZPP_TOOLCHAIN_SETTINGS), true)

ZPP_CC := clang
ZPP_CXX := clang++
ZPP_AS := $(ZPP_CC)
ZPP_LINK := $(ZPP_CXX)
ZPP_AR := ar
ZPP_PYTHON := python

-include zpp_toolchain.mk

endif

