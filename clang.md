Clang Toolchain Setup
=====================

The project cross-compiles from macOS ARM64 to x86_64 using LLVM/Clang.
All four targets (hypervisor, windows driver, UEFI app, linux kernel module)
are built with the same Clang installation.

Requirements
------------
- **LLVM 18+** (tested with 22) via Homebrew: `brew install llvm`
- **CMake 3.25+**: `brew install cmake`
- **Ninja**: `brew install ninja`
- **Podman** (optional, for linux kernel module): `brew install podman`

The toolchain files expect LLVM at `/opt/homebrew/opt/llvm`. To override:
```sh
cmake --preset debug -DLLVM_PREFIX=/path/to/llvm
```

Toolchain Files
---------------
Located under `cmake/`:

| File | Target | ABI |
|------|--------|-----|
| `toolchain-x86_64-freestanding.cmake` | Hypervisor ELF, Linux loader object | Linux ELF, freestanding |
| `toolchain-x86_64-windows-msvc.cmake` | Windows .sys driver, UEFI .efi app | Windows PE/COFF, MSVC ABI |

Both toolchains auto-detect the Clang major version to locate builtin headers.

Freestanding C++26
------------------
The hypervisor is compiled as freestanding C++26 with libc++ headers but no C library.
The include order is critical for `#include_next` chains:

1. `cmake/freestanding-config/` — `__config_site` overrides (no threads, no locale, no filesystem)
2. libc++ headers — `<cstdint>`, `<type_traits>`, `<expected>`, etc.
3. `cmake/freestanding-libc/` — minimal C stubs (`string.h`, `wchar.h`) to satisfy libc++ `#include_next`
4. Clang builtins — `stddef.h`, `stdint.h`, etc.

Key compiler flags:
- `-ffreestanding -nostdlib` — no hosted C library
- `-fPIE -pie` — position-independent ELF (loaded at arbitrary physical address)
- `-mno-red-zone` — required for code that runs in interrupt context
- `-fno-exceptions -fno-rtti` — no runtime support available
- `-stdlib=libc++` — use libc++ headers (not libstdc++)
- `-D_LIBCPP_VERBOSE_ABORT(...)=__builtin_trap()` — redirect libc++ abort to trap (needed for `<expected>`)

Windows/UEFI Cross-Compilation
------------------------------
The Windows and UEFI targets use the MSVC ABI (`x86_64-pc-windows-msvc`) with:
- Windows SDK and WDK headers fetched via NuGet (CMake `FetchContent`)
- EDK2 headers fetched from GitHub (for UEFI)
- No MSVC CRT — freestanding with custom CRT stubs
- `lld-link` as the linker

The `#embed` directive (C++26) is used to embed the hypervisor ELF binary
directly into the loader executables at compile time.
