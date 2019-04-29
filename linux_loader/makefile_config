CC := clang
CXX := clang++
AR := -ar

ifeq ($(TARGET_ABI), x86_64)
	CC := $(CC) -m64
	CXX := $(CXX) -m64
else ifeq ($(TARGET_ABI), x86)
	CC := $(CC) -m32
	CXX := $(CXX) -m32
endif

AS := $(CC)
LINK := $(CXX)
STRIP = strip

