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
- `hypervisor/include/zpp/` — hypervisor headers (error types, page tables, heap)
- `hypervisor/include/zpp/arch/x86_64/` — the architecture layer: instruction wrappers,
  descriptors, page tables, MSRs, MTRRs. Anything true of x86-64 regardless of vendor
- `hypervisor/include/zpp/arch/x86_64/vmx/` — VT-x only: VMCS, EPT, the VMX instructions and
  the VMX capability MSRs. An AMD-V layer would sit beside it as `svm/`, and another
  architecture as `zpp/arch/aarch64/`
- `hypervisor/src/` — hypervisor implementation (main, hypervisor, CRT, `arch/x86_64/`)
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
- `zpp::allocator<T>` (`zpp/allocator.h`) + `zpp/containers.h` aliases (`zpp::vector`,
  `zpp::map`, `zpp::string`, …) route all container allocation through the global heap

Header layering, deliberately one-directional:
`heap.h` (the `heap` type only) → `crt.h` (declares `zpp::crt::heap()`, since `crt.cpp` owns
the instance and its storage) → `allocator.h` → `containers.h`. Do not move `crt::heap()`
back into `heap.h`: the declaration belongs with the definition's owner. Note that inside
`namespace zpp::crt` the name `heap` resolves to the accessor, not the type, so the type is
spelled `zpp::heap` there.

### Heap lifecycle

Storage and size live in `crt/crt.cpp`, owned by the CRT rather than the hypervisor. That
deliberately breaks a bootstrap cycle: static constructors may allocate, so the heap has to
be usable before any hypervisor object exists. The size stays out of `heap.h` — it is a
property of the global heap, not of the `heap` type.

`zpp::crt::init::main()` initializes the heap **before running any constructor**, so
`zpp::crt::heap()` is a plain accessor with **no initialization check on the allocation
path**. Do not reintroduce one. Consequences:

- Allocating before `crt::init::main()` traps — the heap has an empty free list, `allocate`
  returns `nullptr`, and `operator new` calls `__builtin_trap()`. Loud, not silent.
- Initialization happens exactly once at a defined point, so there is no concurrent-init
  race on the allocation path.
- The 20 MB arena is always retained in release, since `crt::init::main()` references it
  unconditionally. It is a fixed arena, so this is intended.

`operator new` also traps on allocation failure, since `-fno-exceptions` means `bad_alloc`
cannot be thrown.
The Windows loader deliberately does **not** define `mem*`/`strlen` — `ntoskrnl.lib`
provides them, and defining them made the link order-sensitive.

### Global initialization

There is no OS-provided C runtime startup, so the CRT does it itself. Prefer
**constant initialization** — `constinit` enforces it at compile time and costs nothing at
runtime — but it is no longer a hard requirement, because `crt/crt.cpp` walks the init
array. Two valid shapes:

```cpp
constinit foo g_foo{};   // preferred: baked into the binary, zero startup cost
bar g_bar;               // fine: compiler emits an .init_array entry, crt::init::main() runs it
```

The raw-storage-plus-placement-new dance that `state.cpp` used to do is **gone** — deleting
it was the point of adding init array support. `hypervisor::instance()` is now a lazy
function-local static (`static hypervisor instance; return instance;`). `constinit` is not an
option there: constant-evaluating the EPT tables alone blows the compiler's constexpr step
budget.

Two things follow from it being a *local* static rather than a namespace-scope global, both
verified in the disassembly: construction is lazy, so it produces **no** `.init_array` entry;
and because the build uses `-fno-threadsafe-statics` the compiler emits a plain
`cmpb`/`movb` on the guard byte with **no** `__cxa_guard_acquire` call. The first call must
therefore not race — it does not, because the loader launches CPUs strictly one at a time. It
does emit a `__cxa_atexit` registration for the destructor, so that path is live.

Consequences worth remembering:
- **A container as a global costs you an init array entry.** `zpp::allocator`'s default
  constructor calls `crt::heap()`, so it is not `constexpr`. That is legal now, just not
  free — prefer locals or members.
- Anything allocating from a constructor is fine: `crt::init::main()` brings the heap up
  before walking the arrays.
- Verify on the built ELF rather than by inspection. `llvm-nm -u` must report **no undefined
  symbols**, and `llvm-readelf -S … | grep init_array` is currently **empty** — nothing in
  the tree needs dynamic initialization today. An `.init_array` appearing is not a failure,
  but it should be a deliberate choice rather than a surprise.

### The init array machinery

`crt/crt.cpp` provides:

- `zpp::crt::init::main()` brings up the global heap, then walks `.preinit_array` and
  `.init_array`. Called from `zpp_hypervisor_main` before anything touches a global, so a
  constructor is free to allocate.
- `zpp::crt::init::cleanup()` runs `__cxa_atexit`-registered destructors in reverse registration
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

## Debugging

Run Bochs and gdb **inside a named tmux session**, never as a detached one-shot command, so
the session can be watched live and reattached to across turns instead of restarting the
emulator to look at something again. One window for the emulator, one for gdb:

```sh
tmux new-session -d -s zpp-debug -n bochs
tmux new-window  -t zpp-debug    -n gdb
tmux attach -t zpp-debug
```

Bound every wait on the emulator to something short (30–60s) and then report state. A long
blocking wait is indistinguishable from a hang.

### Use hardware breakpoints only

**Always `hbreak`, never `break`.** This is not a preference, it is a correctness
requirement here:

- A software breakpoint works by writing `0xCC` and caching the byte it replaced. Bochs
  halts at reset, which is long before the loader has copied the hypervisor ELF into memory,
  so the byte gdb caches at insert time is pre-load garbage. When gdb later removes the
  breakpoint it writes that garbage back **over real instructions**, corrupting the code.
- Observed symptom: a corrupted `ltr` turned into a bogus `#GP`, and a corrupted stub
  reported the wrong exception vector. Hours can go into chasing a fault the debugger
  created.
- `hbreak` uses the debug registers and touches no memory, so it is safe to set at reset by
  raw address before symbols exist.

Also avoid gdb **inferior calls** (`print somefunc()`) on this target. Everything in
`zpp/arch/x86_64/asm.h` is `__attribute__((naked))`, so calling one from gdb faults and leaves the
session in a broken called-frame state.

### Session recipe

```sh
cmake --preset debug -DZPP_VERIFY_HYPERVISOR=ON   # adds the serial tracing
cmake --build --preset debug
./scripts/bochs/setup.sh debug                       # builds OVMF.fd + esp.img
./scripts/bochs/run.sh                               # gdbstub on :1337
./scripts/bochs/debug.sh                             # attaches x86_64-elf-gdb
```

Notes that cost time when forgotten:

- Set `reset_on_triple_fault=0` in the bochsrc when chasing a fault, so Bochs panics with a
  register dump instead of silently rebooting.
- Bochs leaves an `esp.img.lock` behind after a hard kill; the next run dies with
  `image locked`. Remove it.
- The module load address is printed on serial (`ZPP_TRACE allocate_rwx done at …`) and is
  stable for a given build. `load-symbols <addr> out/debug/x86_64/zpp_hypervisor` needs the
  module to already be in memory, so it cannot run at reset — break first, load symbols
  after.
- Serial output lands in `build/bochs/serial.out`.
- On failure the UEFI loader prints the hypervisor's own error code, which `zpp_load_elf`
  passes through unflattened. That is usually enough to skip gdb entirely.

Never pass `CLAUDE.md` (or any other Markdown) to `clang-format` — it will happily reflow it
as C++ and destroy the file.

## Conventions

- C++26 standard, `-pedantic -Wall -Wextra -Werror`
- No exceptions, no RTTI (`-fno-exceptions -fno-rtti`)
- Prefer the standard spelling over a compiler builtin where one exists —
  `std::unreachable()` rather than `__builtin_unreachable()`. It lowers to the same
  intrinsic, so it costs nothing here and needs no runtime (verified: `llvm-nm -u` stays
  empty). `__builtin_trap()` has no standard equivalent and stays as it is.
- Headers use `#pragma once`
- Project namespace: `zpp`. Architecture-specific code lives in `zpp::arch::x86_64`, and
  virtualization-extension code one level further in `zpp::arch::x86_64::vmx`. The split is
  by *what it is true of*, not by vendor: `rdmsr`, `mtrr` and the segment descriptors are
  architectural and stay out of `vmx`, so an AMD-V layer can use them. Spelled out in full
  at call sites rather than aliased — `arch::x86_64::cr3()` — so there is one spelling
  everywhere. `x64` was the old name; do not reintroduce it, it is MSVC's spelling
- `constexpr` everything that can be — all getters, constructors, destructors, operators
- Formatted with `.clang-format` (75 column limit). Not enforced by the build, but CI checks
  it. Use **clang-format 20.1.7 exactly** — majors disagree on formatting, so a different
  version will fight CI. Install it pinned rather than using whatever LLVM ships:
  `pipx install clang-format==20.1.7`. Keep the version in step with
  `CLANG_FORMAT_VERSION` in `.github/workflows/ci.yml`.
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
