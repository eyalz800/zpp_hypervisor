# Locates an LLVM installation without assuming a platform or a package
# manager. Sets LLVM_PREFIX.
#
# Resolution order:
#   1. LLVM_PREFIX given explicitly (-DLLVM_PREFIX=... or the environment).
#   2. llvm-config on PATH, which is authoritative when present.
#   3. clang on PATH, walking up from its real location.
#   4. Well known layouts, newest version first.
#
# Included from toolchain files, so it must not depend on project state.

if(NOT DEFINED LLVM_PREFIX AND DEFINED ENV{LLVM_PREFIX})
    set(LLVM_PREFIX "$ENV{LLVM_PREFIX}")
endif()

if(NOT LLVM_PREFIX)
    # llvm-config knows its own prefix, so prefer it over guessing.
    find_program(_ZPP_LLVM_CONFIG
        NAMES llvm-config
              llvm-config-22 llvm-config-21 llvm-config-20
              llvm-config-19 llvm-config-18
    )
    if(_ZPP_LLVM_CONFIG)
        execute_process(
            COMMAND "${_ZPP_LLVM_CONFIG}" --prefix
            OUTPUT_VARIABLE LLVM_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
endif()

if(NOT LLVM_PREFIX)
    # Fall back to clang itself. Resolve symlinks so that a wrapper on PATH
    # does not yield a prefix with no lib or include directory under it.
    find_program(_ZPP_CLANG NAMES clang)
    if(_ZPP_CLANG)
        get_filename_component(_ZPP_CLANG "${_ZPP_CLANG}" REALPATH)
        get_filename_component(_ZPP_CLANG_BIN "${_ZPP_CLANG}" DIRECTORY)
        get_filename_component(LLVM_PREFIX "${_ZPP_CLANG_BIN}" DIRECTORY)
    endif()
endif()

if(NOT LLVM_PREFIX)
    # Newest first, so a machine with several installs picks the newest.
    file(GLOB _ZPP_LLVM_CANDIDATES
        "/usr/lib/llvm-*"          # Debian and Ubuntu
        "/usr/lib64/llvm-*"        # Fedora and RHEL
        "/opt/homebrew/opt/llvm"   # Homebrew on Apple Silicon
        "/usr/local/opt/llvm"      # Homebrew on Intel macOS
        "/opt/local/libexec/llvm"  # MacPorts
        "/usr"                     # distribution clang in the default prefix
    )
    list(SORT _ZPP_LLVM_CANDIDATES ORDER DESCENDING)
    foreach(_candidate IN LISTS _ZPP_LLVM_CANDIDATES)
        if(EXISTS "${_candidate}/bin/clang")
            set(LLVM_PREFIX "${_candidate}")
            break()
        endif()
    endforeach()
endif()

if(NOT LLVM_PREFIX OR NOT EXISTS "${LLVM_PREFIX}/bin/clang")
    message(FATAL_ERROR
        "Could not locate an LLVM installation. Install LLVM/Clang 18 or newer "
        "and either put it on PATH or pass -DLLVM_PREFIX=<prefix>.")
endif()

set(LLVM_PREFIX "${LLVM_PREFIX}" CACHE PATH "LLVM installation prefix" FORCE)
