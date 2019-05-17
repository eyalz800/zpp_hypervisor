.SUFFIXES:
.SECONDARY:
.PHONY: \
	all \
	build \
	_build \
	verify_mode \
	verify_output_type \
	build_start_message \
	build_mkdirs \
	build_strip \
	build_finished_message \
	rebuild \
	_rebuild \
	clean_mode \
	clean \
	_clean

mode ?= debug
assembly ?= false

CONFIGURATION_NAME := $(mode)
GENERATE_ASSEMBLY := $(assembly)

all: build
THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))
OUTPUT_DIRECTORY_ROOT := out
OBJECT_DIRECTORY_ROOT := obj

include zpp_config.mk
include zpp_project.mk

ifeq ($(TARGET_ABIS), )
build: _build
clean: _clean
rebuild: _rebuild
else
build:
	@for target_abi in $(TARGET_ABIS); do \
		$(MAKE) -s -f $(THIS_MAKEFILE) TARGET_ABIS= TARGET_ABI=$$target_abi; \
	done
clean:
	@for target_abi in $(TARGET_ABIS); do \
		$(MAKE) -s -f $(THIS_MAKEFILE) clean TARGET_ABIS= TARGET_ABI=$$target_abi; \
	done
rebuild:
	@for target_abi in $(TARGET_ABIS); do \
		$(MAKE) -s -f $(THIS_MAKEFILE) rebuild TARGET_ABIS= TARGET_ABI=$$target_abi; \
	done
endif

COMMA := ,
EMPTY :=
SPACE := $(EMPTY) $(EMPTY)

ZPP_FLAGS := $(FLAGS) $(patsubst %, -I%, $(INCLUDE_DIRECTORIES)) $(patsubst %, -l:%, $(SHARED_LIBRARIES)) $(patsubst %, -l:%, $(STATIC_LIBRARIES))

ifeq ($(CONFIGURATION_NAME), debug)
ZPP_FLAGS := $(ZPP_FLAGS) $(FLAGS_DEBUG)
ZPP_CFLAGS := $(CFLAGS) $(CFLAGS_DEBUG) $(ZPP_FLAGS)
ZPP_CXXFLAGS := $(CXXFLAGS) $(CXXFLAGS_DEBUG) $(ZPP_FLAGS)
ZPP_ASFLAGS := $(ASFLAGS) $(ASFLAGS_DEBUG) $(ZPP_FLAGS)
else ifeq ($(CONFIGURATION_NAME), release)
ZPP_FLAGS := $(ZPP_FLAGS) $(FLAGS_RELEASE)
ZPP_CFLAGS := $(CFLAGS) $(CFLAGS_RELEASE) $(ZPP_FLAGS)
ZPP_CXXFLAGS := $(CXXFLAGS) $(CXXFLAGS_RELEASE) $(ZPP_FLAGS)
ZPP_ASFLAGS := $(ASFLAGS) $(ASFLAGS_RELEASE) $(ZPP_FLAGS)
endif

ifeq ($(SOURCE_DIRECTORIES), )
ZPP_SOURCE_FILES := $(SOURCE_FILES)
else
ZPP_SOURCE_FILES := $(SOURCE_FILES) \
	$(shell find $(SOURCE_DIRECTORIES) -type f -name "*.S") \
	$(shell find $(SOURCE_DIRECTORIES) -type f -name "*.c") \
	$(shell find $(SOURCE_DIRECTORIES) -type f -name "*.cc") \
	$(shell find $(SOURCE_DIRECTORIES) -type f -name "*.cpp")
endif

ifeq ($(ZPP_SOURCE_FILES), )
$(error No source files)
endif

BUILD_DIRECTORY := $(OBJECT_DIRECTORY_ROOT)/$(CONFIGURATION_NAME)/$(TARGET_ABI)
BINARY_DIRECTORY := $(OUTPUT_DIRECTORY_ROOT)/$(CONFIGURATION_NAME)/$(TARGET_ABI)

PATH_FROM_ROOT := $(shell echo $(ZPP_SOURCE_FILES) | grep -o "\(\.\./\)*" | sort --unique | tail -n 1)
ifneq ($(PATH_FROM_ROOT), )
BUILD_SUBDIRECTORY := $(shell realpath . --relative-to $(PATH_FROM_ROOT))
BUILD_DIRECTORY := $(BUILD_DIRECTORY)/$(BUILD_SUBDIRECTORY)
endif

OBJECT_FILES := $(patsubst %.c, %.o, $(ZPP_SOURCE_FILES))
OBJECT_FILES := $(OBJECT_FILES) $(patsubst %.cpp, %.o, $(ZPP_SOURCE_FILES))
OBJECT_FILES := $(OBJECT_FILES) $(patsubst %.cc, %.o, $(ZPP_SOURCE_FILES))
OBJECT_FILES := $(OBJECT_FILES) $(patsubst %.S, %.o, $(ZPP_SOURCE_FILES))
OBJECT_FILES := $(patsubst %, $(BUILD_DIRECTORY)/%, $(OBJECT_FILES))
OBJECT_FILES := $(filter %.o, $(OBJECT_FILES))
OBJECT_FILES_DIRECTORIES := $(dir $(OBJECT_FILES))

DEPENDENCY_FILES := $(patsubst %.o, %.d, $(OBJECT_FILES))

ifeq ($(OUTPUT_TYPE), default)
	LINK_COMMAND := $(LINK) -o $(BINARY_DIRECTORY)/$(TARGET_NAME) $(OBJECT_FILES) $(FLAGS)
else ifeq ($(OUTPUT_TYPE), executable)
	LINK_COMMAND := $(LINK) -o $(BINARY_DIRECTORY)/$(TARGET_NAME) $(OBJECT_FILES) $(FLAGS)
else ifeq ($(OUTPUT_TYPE), shared-lib)
	LINK_COMMAND := $(LINK) -shared -o $(BINARY_DIRECTORY)/$(TARGET_NAME) $(OBJECT_FILES) $(FLAGS)
else ifeq ($(OUTPUT_TYPE), static-lib)
	LINK_COMMAND := $(AR) rcs $(BINARY_DIRECTORY)/$(TARGET_NAME) $(OBJECT_FILES)
else ifeq ($(OUTPUT_TYPE), relocatable)
	LINK_COMMAND := $(LINK) -r -o $(BINARY_DIRECTORY)/$(TARGET_NAME) $(OBJECT_FILES) $(FLAGS)
endif

_build: build_finished_message

build_start_message:
	@echo "Building $(TARGET_ABI)/$(TARGET_NAME) in $(CONFIGURATION_NAME) mode..."

build_finished_message: build_strip
	@echo "Done."

verify_mode:
	@if [ "$(mode)" = "debug" ]; then \
		:; \
	elif [ "$(mode)" = "release" ]; then \
		:; \
	else \
		echo "Error: mode can only be \"debug\" or \"release\", note: you entered: '$(mode)'"; \
		exit 1; \
	fi

verify_output_type: verify_mode
	@if [ "$(OUTPUT_TYPE)" = "executable" ]; then \
		:; \
	elif [ "$(OUTPUT_TYPE)" = "shared-lib" ]; then \
		:; \
	elif [ "$(OUTPUT_TYPE)" = "static-lib" ]; then \
		:; \
	else \
		echo "Error: OUTPUT_TYPE can either be \"executable\" \"shared-lib\" or \"static-lib\", note: you entered: '$(OUTPUT_TYPE)'"; \
		exit 1; \
	fi

build_mkdirs: build_start_message
	@mkdir -p $(BUILD_DIRECTORY); \
	mkdir -p $(BINARY_DIRECTORY); \
	mkdir -p $(OBJECT_FILES_DIRECTORIES)

$(BINARY_DIRECTORY)/$(TARGET_NAME): $(OBJECT_FILES)
	@echo "Linking..."; \
	$(LINK_COMMAND)

ifeq ($(GENERATE_ASSEMBLY), true)
$(BUILD_DIRECTORY)/%.S: %.c | build_mkdirs
	@echo "Compiling $<..."; \
	$(CC) -S $(ZPP_CFLAGS) -o $@ $< -MD -Wp,-MD,`dirname $@`/`basename $@ .S`.d

$(BUILD_DIRECTORY)/%.S: %.cpp | build_mkdirs
	@echo "Compiling $<..."; \
	$(CXX) -S $(ZPP_CXXFLAGS) -o $@ $< -MD -Wp,-MD,`dirname $@`/`basename $@ .S`.d

$(BUILD_DIRECTORY)/%.S: %.cc | build_mkdirs
	@echo "Compiling $<..."; \
	$(CXX) -S $(ZPP_CXXFLAGS) -o $@ $< -MD -Wp,-MD,`dirname $@`/`basename $@ .S`.d
	
$(BUILD_DIRECTORY)/%.o: $(BUILD_DIRECTORY)/%.S
	@$(CC) -s -Wno-unicode -c -o $@ $<
else ifeq ($(GENERATE_ASSEMBLY), false)
$(BUILD_DIRECTORY)/%.o: %.c | build_mkdirs
	@echo "Compiling $<..."; \
	$(CC) -c $(ZPP_CFLAGS) -o $@ $< -MD -Wp,-MD,`dirname $@`/`basename $@ .o`.d

$(BUILD_DIRECTORY)/%.o: %.cpp | build_mkdirs
	@echo "Compiling $<..."; \
	$(CXX) -c $(ZPP_CXXFLAGS) -o $@ $< -MD -Wp,-MD,`dirname $@`/`basename $@ .o`.d

$(BUILD_DIRECTORY)/%.o: %.cc | build_mkdirs
	@echo "Compiling $<..."; \
	$(CXX) -c $(ZPP_CXXFLAGS) -o $@ $< -MD -Wp,-MD,`dirname $@`/`basename $@ .o`.d
else
$(error "GENERATE_ASSEMBLY must either be true or false.")
endif

$(BUILD_DIRECTORY)/%.o: %.S | build_mkdirs
	@echo "Assemblying $<..."; \
	$(AS) -c $(ZPP_ASFLAGS) -o $@ $< -MD -Wp,-MD,`dirname $@`/`basename $@ .o`.d

-include $(DEPENDENCY_FILES)

build_strip: $(BINARY_DIRECTORY)/$(TARGET_NAME)
	@if [ "$(mode)" = "release" ]; then \
		if [ "$(OUTPUT_TYPE)" = "static-lib" ]; then \
			:; \
		else \
			echo "Stripping..."; \
			$(STRIP) $(STRIP_FLAGS) $(BINARY_DIRECTORY)/$(TARGET_NAME); \
		fi \
	fi \

_clean:
	@echo "Cleaning..."; \
	rm -rf $(OBJECT_DIRECTORY_ROOT)/debug/$(TARGET_ABI); \
	rm -rf $(OBJECT_DIRECTORY_ROOT)/release/$(TARGET_ABI); \
	rm -f $(OUTPUT_DIRECTORY_ROOT)/debug/$(TARGET_ABI)/$(TARGET_NAME); \
	rm -f $(OUTPUT_DIRECTORY_ROOT)/release/$(TARGET_ABI)/$(TARGET_NAME); \
	find $(OBJECT_DIRECTORY_ROOT) -type d -empty -delete 2> /dev/null; \
	find $(OUTPUT_DIRECTORY_ROOT) -type d -empty -delete 2> /dev/null; \
	echo "Cleaned."

clean_mode: verify_mode
	@rm -rf $(BUILD_DIRECTORY); \
	rm -rf $(BINARY_DIRECTORY)/$(TARGET_NAME)

_rebuild: clean_mode
	@$(MAKE) -s build

ZPP_DEPENDENCIES := true
include zpp_project.mk