include ../environment.config

CLANG_TARGET := $(TARGET_ABI)-pc-win32-msvc

ifeq ($(TARGET_ABI), x86_64)
	MICROSOFT_DEFINES := -D_MT -D_WIN32 -D_WIN64 -D_AMD64_
	VISUAL_STUDIO_PROCESSOR := x64
	WINDOWS_KITS_PROCESSOR := x64
	EDK2_PROCESSOR := X64
else ifneq ($(TARGET_ABI),)
$(error Unsupported target)
endif

EMPTY := 
SPACE := $(EMPTY) $(EMPTY)

VISUAL_STUDIO_ROOT := $(subst $(SPACE),+,$(VISUAL_STUDIO_ROOT))
WINDOWS_KITS_ROOT := $(subst $(SPACE),+,$(WINDOWS_KITS_ROOT))
EDK2_ROOT := $(subst $(SPACE),+,$(EDK2_ROOT))

VISUAL_STUDIO_INCLUDES := $(VISUAL_STUDIO_ROOT)/include

WINDOWS_KITS_INCLUDES := \
	$(WINDOWS_KITS_ROOT)/Include/$(WINDOWS_KITS_VERSION)/ucrt \
	$(WINDOWS_KITS_ROOT)/Include/$(WINDOWS_KITS_VERSION)/shared

EDK2_INCLUDES := $(EDK2_ROOT)/MdePkg/Include

ENVIRONMENT_INCLUDES := \
	$(EDK2_INCLUDES) \
	$(EDK2_INCLUDES)/$(EDK2_PROCESSOR) \
	$(WINDOWS_KITS_INCLUDES) \
	$(VISUAL_STUDIO_INCLUDES)

ENVIRONMENT_INCLUDES := $(patsubst %, -isystem %, $(ENVIRONMENT_INCLUDES))
ENVIRONMENT_INCLUDES := $(subst +,$(SPACE),$(ENVIRONMENT_INCLUDES))

ENVIRONMENT_FLAGS := \
	-target $(CLANG_TARGET) \
	-nostdinc \
	-nostdinc++ \
	-nostdlib \
	-nostdlib++ \
	-D_CRT_SECURE_NO_WARNINGS \
	-D_CRT_SECURE_NO_DEPRECATE \
	-D_NO_CRT_STDIO_INLINE \
	$(MICROSOFT_DEFINES) \
	-U__GNUC__ \
	-U__gnu_linux__ \
	-U__GNUC_MINOR__ \
	-U__GNUC_PATCHLEVEL__ \
	-U__GNUC_STDC_INLINE__ \
	-fuse-ld=lld \
	-Wl,-ignore:4217

CC := clang \
	$(ENVIRONMENT_INCLUDES) \
	$(ENVIRONMENT_FLAGS)

CXX := clang++ \
	$(ENVIRONMENT_INCLUDES) \
	$(ENVIRONMENT_FLAGS)

AR := ar
AS := $(CC)
LINK := $(CXX)
STRIP = strip
