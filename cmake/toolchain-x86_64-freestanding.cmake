# Toolchain for freestanding x86_64 ELF binaries (hypervisor, linux loader object)
# Cross-compiles to x86_64 using an LLVM located at configure time.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Find LLVM. Detected per platform rather than assumed - see find-llvm.cmake.
include("${CMAKE_CURRENT_LIST_DIR}/find-llvm.cmake")

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
# These must be CACHE variables so they survive into the project's CMakeLists.txt
# (toolchain set() without CACHE is only visible during toolchain processing).
set(LLVM_LIBCXX_INCLUDE "${LLVM_PREFIX}/include/c++/v1" CACHE PATH "libc++ headers")
set(LLVM_CLANG_INCLUDE "${LLVM_PREFIX}/lib/clang/${LLVM_CLANG_MAJOR}/include" CACHE PATH "Clang builtin headers")

# For freestanding builds we do NOT use the macOS SDK C headers — they conflict
# with C++26 (size_t scoping changes). Instead we use:
#   1. Clang builtins: stddef.h, stdint.h, stdbool.h, stdarg.h, etc.
#   2. Minimal freestanding C stubs: string.h, wchar.h, etc. (just type decls)
# These satisfy libc++'s #include_next without pulling in a full C library.
# ZPP_SOURCE_DIR must be passed to the sub-build so we can find the stubs.
# It's set as a cache variable by the ExternalProject_Add -D argument.

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# CMake's compiler test would try to link a hosted executable (with CRT).
# For a freestanding cross-build we compile a static library instead so that
# the link step (which would fail without a sysroot) is skipped entirely.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
