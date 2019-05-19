ifeq ($(ZPP_PROJECT_SETTINGS), true)

include ../environment.config

ZPP_TARGET_NAME := zpp_hypervisor
ZPP_TARGET_TYPES := $(SUPPORTED_ARCHITECTURES)
ZPP_LINK_TYPE := default
ZPP_OUTPUT_DIRECTORY_ROOT := ../out
ZPP_SOURCE_DIRECTORIES := \
	./src
ZPP_SOURCE_FILES :=

endif

ifeq ($(ZPP_PROJECT_FLAGS), true)
ZPP_FLAGS := \
	$(patsubst %, -I%, $(shell find . -type d -name "include")) \
	-pedantic \
	-Wall \
	-Wextra \
	-Werror \
	-fPIE \
	-nostdlib \
	-ffreestanding \
	-mno-red-zone
ZPP_FLAGS_DEBUG := \
	-g \
	-DZPP_HYPERVISOR_WAIT_FOR_DEBUGGER=$(HYPERVISOR_WAIT_FOR_DEBUGGER)
ZPP_FLAGS_RELEASE := \
	-O2 \
	-flto \
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
	-pie \
	-Wl,--no-undefined
ZPP_LFLAGS_DEBUG := \
	$(ZPP_FLAGS_DEBUG)
ZPP_LFLAGS_RELEASE := \
	$(ZPP_FLAGS_RELEASE) \
	-Wl,--strip-all \
	-Wl,-flto \
	-Wl,--gc-sections
endif

ifeq ($(ZPP_PROJECT_RULES), true)
endif

ifeq ($(ZPP_TOOLCHAIN_SETTINGS), true)

ZPP_CC := clang
ZPP_CXX := clang++
ZPP_AS := $(ZPP_CC)
ZPP_LINK := $(ZPP_CXX)
ZPP_AR := ar

-include zpp_toolchain.mk

endif

