# Toolchain for x86_64 Windows PE/COFF targets (windows driver, UEFI app)
# Cross-compiles from macOS ARM64 using Homebrew LLVM with MSVC ABI.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(LLVM_PREFIX "/opt/homebrew/opt/llvm" CACHE PATH "Homebrew LLVM prefix")

set(CMAKE_C_COMPILER "${LLVM_PREFIX}/bin/clang")
set(CMAKE_CXX_COMPILER "${LLVM_PREFIX}/bin/clang++")
set(CMAKE_ASM_COMPILER "${LLVM_PREFIX}/bin/clang")
set(CMAKE_LINKER "${LLVM_PREFIX}/bin/lld-link")

set(CMAKE_C_COMPILER_TARGET "x86_64-pc-windows-msvc")
set(CMAKE_CXX_COMPILER_TARGET "x86_64-pc-windows-msvc")
set(CMAKE_ASM_COMPILER_TARGET "x86_64-pc-windows-msvc")

# Use lld-link via clang driver
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")

# Detect Clang version to locate builtin headers (stddef.h, stdint.h, etc.)
execute_process(
    COMMAND "${LLVM_PREFIX}/bin/clang" --version
    OUTPUT_VARIABLE _CLANG_VERSION_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REGEX MATCH "version ([0-9]+)" _CLANG_VER_MATCH "${_CLANG_VERSION_OUTPUT}")
set(LLVM_CLANG_MAJOR "${CMAKE_MATCH_1}")

# Clang builtin headers (stddef.h, stdint.h, stdbool.h, etc.)
# For MSVC target with -nostdinc, these provide the C primitive type headers
# needed by <cstddef> and <cstdint> wrappers.
set(LLVM_CLANG_INCLUDE "${LLVM_PREFIX}/lib/clang/${LLVM_CLANG_MAJOR}/include"
    CACHE PATH "Clang builtin headers")

# libc++ headers for C++ standard types (cstddef, cstdint, algorithm, etc.)
# Used with -nostdinc to provide only the headers we explicitly list.
set(LLVM_LIBCXX_INCLUDE "${LLVM_PREFIX}/include/c++/v1"
    CACHE PATH "libc++ headers")

# Do not inject MSVC CRT library references (-Xclang --dependent-lib=msvcrtd
# etc.) — we are a freestanding kernel driver with our own CRT stubs.
# Setting this to an empty string suppresses CMake's MSVC_RUNTIME_LIBRARY
# selection which would otherwise add -D_DEBUG/-D_DLL/-D_MT and the
# --dependent-lib pragma flags.
set(CMAKE_MSVC_RUNTIME_LIBRARY "" CACHE STRING "")

# Wipe CMake's default MSVC debug/release compile flags (which inject CRT
# selections) so we control all flags explicitly in CMakeLists.txt.
set(CMAKE_CXX_FLAGS_DEBUG_INIT "")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "")
set(CMAKE_C_FLAGS_DEBUG_INIT "")
set(CMAKE_C_FLAGS_RELEASE_INIT "")

# Remove CMake's default Windows import libraries from the link step.
# For a kernel driver we link only ntoskrnl.lib; all others are wrong.
set(CMAKE_CXX_STANDARD_LIBRARIES "" CACHE STRING "")
set(CMAKE_C_STANDARD_LIBRARIES "" CACHE STRING "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# CMake's compiler test would try to link a hosted executable (with CRT).
# For a cross-build targeting Windows from macOS, compile a static library
# instead so that the link step (which would fail without a Windows sysroot)
# is skipped entirely.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
