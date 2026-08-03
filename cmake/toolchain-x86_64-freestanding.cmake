# Toolchain for freestanding x86_64 ELF binaries (hypervisor, linux loader object)
# Cross-compiles from macOS ARM64 to x86_64 using Homebrew LLVM.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Find Homebrew LLVM
set(LLVM_PREFIX "/opt/homebrew/opt/llvm" CACHE PATH "Homebrew LLVM prefix")

set(CMAKE_C_COMPILER "${LLVM_PREFIX}/bin/clang")
set(CMAKE_CXX_COMPILER "${LLVM_PREFIX}/bin/clang++")
set(CMAKE_ASM_COMPILER "${LLVM_PREFIX}/bin/clang")

# ld.lld ships as a separate Homebrew formula; prefer the versioned one next to
# clang, fall back to the one on PATH.
if(EXISTS "${LLVM_PREFIX}/bin/ld.lld")
    set(CMAKE_LINKER "${LLVM_PREFIX}/bin/ld.lld")
else()
    find_program(_LLD_BIN ld.lld REQUIRED)
    set(CMAKE_LINKER "${_LLD_BIN}")
endif()

set(CMAKE_C_COMPILER_TARGET "x86_64-unknown-linux-elf")
set(CMAKE_CXX_COMPILER_TARGET "x86_64-unknown-linux-elf")
set(CMAKE_ASM_COMPILER_TARGET "x86_64-unknown-linux-elf")

# Use lld
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")

# Detect the installed Clang major version dynamically so the path does not
# need to be updated when LLVM is upgraded.
execute_process(
    COMMAND "${LLVM_PREFIX}/bin/clang" --version
    OUTPUT_VARIABLE _CLANG_VERSION_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REGEX MATCH "version ([0-9]+)" _CLANG_VER_MATCH "${_CLANG_VERSION_OUTPUT}")
set(LLVM_CLANG_MAJOR "${CMAKE_MATCH_1}")

# libc++ headers for freestanding C++ (cstdint, type_traits, etc.)
set(LLVM_LIBCXX_INCLUDE "${LLVM_PREFIX}/include/c++/v1")
set(LLVM_CLANG_INCLUDE "${LLVM_PREFIX}/lib/clang/${LLVM_CLANG_MAJOR}/include")

# macOS SDK C headers — used as the underlying C stdlib for freestanding builds.
# The Homebrew libc++ headers expect `#include_next <string.h>` etc. to find real
# C declarations; the macOS SDK headers satisfy this without introducing a Linux
# sysroot dependency. Only applicable on macOS hosts.
if(CMAKE_HOST_APPLE)
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE MACOS_SDK_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    set(MACOS_SDK_INCLUDE "${MACOS_SDK_PATH}/usr/include")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# CMake's compiler test would try to link a hosted executable (with CRT).
# For a freestanding cross-build we compile a static library instead so that
# the link step (which would fail without a sysroot) is skipped entirely.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
