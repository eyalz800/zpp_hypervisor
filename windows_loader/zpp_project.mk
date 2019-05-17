include ../environment.config
OUTPUT_DIRECTORY_ROOT := ../out
TARGET_NAME := zpp_loader.sys
TARGET_ABIS := $(SUPPORTED_ARCHITECTURES)
OUTPUT_TYPE := executable
INCLUDE_DIRECTORIES := \
	$(shell find . -type d -name "include") \
	$(shell find ../hypervisor -type d -name "include")
FLAGS := -pedantic -Wall -Wextra -Werror -DZPP_ELF_BINARY_NO_HIDDEN \
	-Wno-format-security \
	-Wno-unused-command-line-argument \
	-DZPP_ELF_BINARY_PATH="\"../../out/$(CONFIGURATION_NAME)/$(TARGET_ABI)/zpp_hypervisor\"" \
	-nostdlib \
	-fno-exceptions \
	-ffreestanding \
	-fno-rtti \
	-ffreestanding \
	-mno-red-zone \
	-Wl,-subsystem:native \
	-Wl,/driver:wdm \
	-Wl,/dynamicbase \
	-Wl,/nodefaultlib \
	-Wl,/dll \
	-Wl,ntoskrnl.lib \
	-Wl,-entry:driver_entry
FLAGS_DEBUG :=
FLAGS_RELEASE := -D NDEBUG -D _NDEBUG \
	-Os \
	-flto -Wl,-flto \
	-ffunction-sections \
	-fdata-sections \
	-fvisibility=hidden \
	-Wl,--gc-sections
CFLAGS := -std=c11
CFLAGS_DEBUG :=
CFLAGS_RELEASE :=
CXXFLAGS := -std=c++17
CXXFLAGS_DEBUG :=
CXXFLAGS_RELEASE :=
ASFLAGS := -x assembler-with-cpp
ASFLAGS_DEBUG :=
ASFLAGS_RELEASE :=
STATIC_LIBRARIES := 
SHARED_LIBRARIES :=
STRIP_FLAGS := -s
SOURCE_DIRECTORIES := ./src ../loader/src
SOURCE_FILES :=

ifeq ($(ZPP_DEPENDENCIES), true)

$(BUILD_DIRECTORY)/../loader/src/elf_binary.o: \
	../out/$(CONFIGURATION_NAME)/$(TARGET_ABI)/zpp_hypervisor

endif
