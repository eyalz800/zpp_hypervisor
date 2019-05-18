include ../environment.config
OUTPUT_DIRECTORY_ROOT := ../out
TARGET_NAME := zpp_hypervisor
TARGET_ABIS := $(SUPPORTED_ARCHITECTURES)
OUTPUT_TYPE := executable
INCLUDE_DIRECTORIES := \
	$(shell find . -type d -name "include")
FLAGS := -pedantic -Wall -Wextra -Werror \
	-stdlib=libc++ \
	-Wno-format-security \
	-fPIE -pie \
	-Wl,--no-undefined \
	-Wno-unused-command-line-argument \
	-nostdlib \
	-mno-red-zone
FLAGS_DEBUG := -g -D _DEBUG -DZPP_HYPERVISOR_WAIT_FOR_DEBUGGER=$(HYPERVISOR_WAIT_FOR_DEBUGGER)
FLAGS_RELEASE := -D NDEBUG -D _NDEBUG \
	-O2 \
	-flto -Wl,-flto \
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
SOURCE_DIRECTORIES := ./src
SOURCE_FILES :=

