# zpp_hypervisor

Intel VT-x hypervisor with cross-platform loader drivers. Freestanding C++26 codebase
cross-compiled from macOS ARM64 to x86_64 using Homebrew LLVM (clang 22) + CMake + Ninja.

## Build

```sh
cmake --preset debug && cmake --build --preset debug      # debug
cmake --preset release && cmake --build --preset release  # release
cmake --build --preset debug --target clean               # clean (preserves deps)
cmake --build --preset debug --target linux_loader_ko     # linux .ko (needs Podman)
```

Output goes to `out/{debug,release}/x86_64/`.

## Architecture

Four build targets, each an ExternalProject with its own toolchain:

| Target | Toolchain | Output | Source |
|--------|-----------|--------|--------|
| **hypervisor** | freestanding ELF | `zpp_hypervisor` (PIE ELF) | `hypervisor/` |
| **windows_loader** | MSVC ABI (PE/COFF) | `zpp_loader.sys` | `windows_loader/` + `loader/` |
| **uefi_loader** | MSVC ABI (PE/COFF) | `zpp_loader.efi` | `uefi_loader/` + `loader/` |
| **linux_loader** | freestanding ELF | `zpp_loader.o` → `.ko` via kbuild | `linux_loader/` + `loader/` |

The hypervisor builds first; loaders embed its ELF binary via `#embed` (C++26).

Sources are **globbed, never listed by hand**. Each sub-build uses
`file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` over `*.cpp` and `*.S`, so adding a file needs no
CMake edit and no manual re-configure. `*.cppm` files are globbed separately and added via
`target_sources(... FILE_SET CXX_MODULES ...)` so CMake scans and builds them as real C++
modules — this is why `cmake_minimum_required` is 3.28.
`linux_loader/src/main.c` is the one exception: it is compiled by kbuild, not CMake.

### Key directories

- `cmake/` — toolchain files, sub-build CMakeLists, freestanding libc stubs, dependency fetching
- `hypervisor/include/zpp/` — hypervisor headers (error types, VMX, EPT, page tables, heap)
- `hypervisor/src/` — hypervisor implementation (main, hypervisor, CRT, x64 arch code)
- `loader/src/` — shared loader code (elf_binary.cpp with `#embed`, main.cpp with `zpp_load_elf`)
- `{windows,uefi,linux}_loader/` — platform-specific loader glue + CRT stubs

### Freestanding constraints

The hypervisor has no OS, no libc, no C++ runtime library. It uses:
- libc++ **headers only** (no linking) with custom `__config_site` (no threads/locale/filesystem)
- Custom CRT: `crt.cpp` (memcpy/memmove/memset/memcmp/strlen, the `__cxa_*` ABI, every
  `operator new`/`operator delete` overload — plain, `nothrow`, `align_val_t` — plus the
  global heap instance and `zpp::crt::init`), `heap.cpp` (the `heap` type only)
- `-D_LIBCPP_VERBOSE_ABORT(...)=__builtin_trap()` for `<expected>` support
- `std::expected<T, zpp::error>` (replaced custom `zpp::maybe<T>`)
- `zpp::scope_exit` (matches P0052 `std::scope_exit` API — not in libc++ until C++29)
- `zpp::allocator<T>` + `zpp/containers.h` aliases (`zpp::vector`, `zpp::map`, `zpp::string`, …)
  route all container allocation through the global heap

### Heap lifecycle

Storage and size live in `crt/crt.cpp`, owned by the CRT rather than the hypervisor. That
deliberately breaks a bootstrap cycle: static constructors may allocate, so the heap has to
be usable before any hypervisor object exists. The size stays out of `heap.h` — it is a
property of the global heap, not of the `heap` type.

`zpp::crt::init::run()` initializes the heap **before running any constructor**, so
`zpp::global_heap()` is a plain accessor with **no initialization check on the allocation
path**. Do not reintroduce one. Consequences:

- Allocating before `crt::init::run()` traps — the heap has an empty free list, `allocate`
  returns `nullptr`, and `operator new` calls `__builtin_trap()`. Loud, not silent.
- Initialization happens exactly once at a defined point, so there is no concurrent-init
  race on the allocation path.
- The 20 MB arena is always retained in release, since `crt::init::run()` references it
  unconditionally. It is a fixed arena, so this is intended.

`operator new` also traps on allocation failure, since `-fno-exceptions` means `bad_alloc`
cannot be thrown.
The Windows loader deliberately does **not** define `mem*`/`strlen` — `ntoskrnl.lib`
provides them, and defining them made the link order-sensitive.

### Constant initialization (critical)

No `.init` sections execute — there is no C runtime startup. All globals must be
**constant-initialized** (value determined at compile time, baked into the binary).
Use `constinit` to enforce this at compile time. Types used as globals must have
`constexpr` constructors. For complex types that can't be constant-initialized,
use raw byte storage in BSS + placement new at runtime (see `state.cpp` pattern):

```cpp
alignas(T) static std::byte storage[sizeof(T)];
T & ref = *reinterpret_cast<T *>(&storage);
// construct via placement new at first use
```

Consequences worth remembering:
- **Containers can never be globals.** `zpp::allocator`'s default constructor calls
  `global_heap()`, so it is not `constexpr`; a `zpp::vector` at namespace scope would
  require dynamic initialization. Use them as locals or as members of an object that is
  itself placement-new'd.
- Placement-new the global state with `new (&g) state` (default-init), **not**
  `state{}` (value-init) — value-init memsets the whole object, which for a 20 MB
  `heap_storage` member means redundantly zeroing BSS the loader already cleared.
- Verify the invariant on the built ELF, not by inspection:
  `llvm-readelf -S out/release/x86_64/zpp_hypervisor | grep init_array` must be empty,
  and `llvm-nm -u` must report no undefined symbols.

### Dynamic initialization (the escape hatch)

`constinit` is still the default discipline, but `crt/crt.cpp` now supports
globals that genuinely cannot be constant-initialized:

- `zpp::crt::init::run()` brings up the global heap, then walks `.preinit_array` and
  `.init_array`. Called from `zpp_hypervisor_main` before anything touches a global, so a
  constructor is free to allocate.
- `zpp::crt::init::teardown()` runs `__cxa_atexit`-registered destructors in reverse registration
  order, then `.fini_array` in reverse. Called only when the hypervisor fails to go
  resident — on success its globals must outlive every guest, so destructors deliberately
  never run.
- Provides `__cxa_atexit`, `__cxa_finalize`, `__dso_handle` (crtbegin normally supplies the
  last one; `-nostdlib` does not pull it in), and `__cxa_guard_acquire/release/abort`.
  The destructor registry is a fixed 2048-entry table on purpose: registration happens from
  inside constructors and must not allocate. It traps rather than silently dropping.
- All builds keep `-fno-threadsafe-statics` (no threads, no mutexes in this codebase), so the
  compiler does not emit guard calls and `__cxa_guard_*` are currently inert. They exist to
  keep the ABI complete if the flag is ever dropped. **Consequence:** a function-local static
  with a non-constant initializer gets no guard, so two CPUs reaching it concurrently would
  both initialize it. Prefer namespace-scope `constinit` state over function-local statics
  in anything reachable from a VM exit.

Note `.fini_array` usually stays empty: destructors of globals are registered at runtime via
`__cxa_atexit`, and `.fini_array` only receives `__attribute__((destructor))` functions.
Both paths are handled. Verified to survive `--gc-sections --strip-all`.

### Include order (critical for freestanding)

1. `cmake/freestanding-config/` — `__config_site` overrides
2. libc++ headers — `<cstdint>`, `<expected>`, etc.
3. `cmake/freestanding-libc/` — minimal C stubs for `#include_next`
4. Clang builtins — `stddef.h`, `stdint.h`

## Conventions

- C++26 standard, `-pedantic -Wall -Wextra -Werror`
- No exceptions, no RTTI (`-fno-exceptions -fno-rtti`)
- Headers use `#pragma once`
- Project namespace: `zpp`
- `constexpr` everything that can be — all getters, constructors, destructors, operators
- Formatted with `.clang-format` (75 column limit). Not enforced by the build — run
  `clang-format -i` on what you touch.
- **Buffers are `std::span`, never `(pointer, size)` pairs.** Any API taking a contiguous
  range takes `std::span<T>` / `std::span<const std::byte>`. Applies to new APIs too.
  The one deliberate exception is address-range APIs like `page_table::map_from` and the
  ELF `protect` callback: those exist in a `std::uint64_t` address form as well, the pointer
  overload just forwards to it, and the range is never read through — wrapping in
  `as_bytes(span{...})` only to immediately decay back to an integer adds noise, not safety.
- **No inline assembly outside `x64/`.** Generic code uses an abstraction — spin loops call
  `zpp::spin_hint()` from `zpp/spin_lock.h`, which selects `__builtin_ia32_pause()` on x86
  and `__builtin_arm_yield()` on aarch64. `zpp::spin_lock` is the only lock shape available;
  there is no scheduler to block against, and it is **not recursive**.
- Error handling: always `std::expected<T, zpp::error>`, and `std::expected<void, zpp::error>`
  for fallible functions with no value (never a bare `zpp::error` return). `return {}` on
  success, `return std::unexpected(zpp::error{error::whatever})` on failure, and
  `if (auto result = f(); !result) { return result; }` at call sites.
- Constructor templates taking an error-code enum must be constrained
  (`requires std::is_enum_v<ErrorCode>`) — an unconstrained one makes `zpp::error`
  constructible from anything and sends `std::expected`'s constraints into infinite recursion.
- Loader entry points use `extern "C"` (called from C code in linux_loader)
- `compile_commands.json` is auto-copied to each source directory after build (for clangd)

## Dependencies (auto-fetched by CMake)

- Windows SDK/WDK headers + libs: NuGet packages via `cmake/dependencies.cmake`
- EDK2 headers: GitHub archive via FetchContent
- Linux kernel headers: inside Podman container (Alpine linux-headers)
