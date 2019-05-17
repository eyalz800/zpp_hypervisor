include ../environment.config
TARGET_NAME := zpp_loader.o
TARGET_ABIS := $(SUPPORTED_ARCHITECTURES)
OUTPUT_TYPE := relocatable
INCLUDE_DIRECTORIES := \
	$(shell find . -type d -name "include") \
	$(shell find ../hypervisor -type d -name "include")
FLAGS := -pedantic -Wall -Wextra -Werror \
	-Wno-format-security \
	-stdlib=libc++ \
	-Wl,--no-undefined \
	-Wno-unused-command-line-argument \
	-nostdlib \
	-ffreestanding \
	-mno-red-zone \
	-DZPP_ELF_BINARY_PATH="\"../../out/$(CONFIGURATION_NAME)/$(TARGET_ABI)/zpp_hypervisor\""
FLAGS_DEBUG := -g -D _DEBUG
FLAGS_RELEASE := -D NDEBUG -D _NDEBUG \
	-O2 \
	-ffunction-sections \
	-fdata-sections \
	-fvisibility=hidden \
	-Wl,--gc-sections
CFLAGS := -std=c11
CFLAGS_DEBUG :=
CFLAGS_RELEASE :=
CXXFLAGS := -std=c++17 -fno-rtti -fno-threadsafe-statics -fno-exceptions
CXXFLAGS_DEBUG :=
CXXFLAGS_RELEASE :=
ASFLAGS := -x assembler-with-cpp
ASFLAGS_DEBUG :=
ASFLAGS_RELEASE :=
STATIC_LIBRARIES := 
SHARED_LIBRARIES :=
STRIP_FLAGS := -s
SOURCE_DIRECTORIES := ../loader/src
SOURCE_FILES :=

ifeq ($(ZPP_DEPENDENCIES), true)

$(BUILD_DIRECTORY)/../loader/src/elf_binary.o: \
	../out/$(CONFIGURATION_NAME)/$(TARGET_ABI)/zpp_hypervisor

endif
