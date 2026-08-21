# zpp_hypervisor

Intel VT-x hypervisor with cross-platform loader drivers. Freestanding C++26 codebase
cross-compiled from macOS ARM64 to x86_64 using Homebrew LLVM (clang 22) + CMake + Ninja.

## Build

```sh
cmake --preset debug && cmake --build --preset debug      # debug
cmake --preset release && cmake --build --preset release  # release
cmake --build --preset debug --target clean               # clean (preserves deps)
cmake --build --preset debug --target linux_loader_ko     # linux .ko (needs Podman)
./scripts/ci/run-host-tests.sh                            # host test suite
```

Output goes to `out/{debug,release}/x86_64/`.

**Run the suite through that script, never `ctest` on its own.** Every harness
is `EXCLUDE_FROM_ALL` on purpose - the suite is native and the rest of the tree
is cross-compiled, so a harness that fails to compile must not be able to stop
a deploy - which means `cmake --build --preset debug` does not build them. Bare
`ctest` then runs whatever binaries happen to be there and reports them
passing. Measured: a stale `resume_guest` reported "70 checks, 0 failures"
against a source that had 115 checks and a failing one, so a new regression
test appeared to pass before its fix existed. The script builds `zpp_tests`
first, and that is the whole difference.

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
- Custom CRT: `crt.cpp` (memcpy/memmove/memset/memcmp/strlen, the `__cxa_*` ABI, the
  global heap instance, the over-alignment helpers and `zpp::crt::init`),
  `operators.cpp` (every `operator new`/`operator delete` overload — plain, `nothrow`,
  `align_val_t`), `init_array.cpp` (the six linker-synthesized array bounds, behind three
  accessors), `heap.cpp` (the `heap` type only). The last two are separate translation
  units so `tests/crt` can compile the rest: linking a replacement `operator new` into a
  hosted binary routes the standard library's own pre-main initializers through the arena,
  and the array bounds cannot be reproduced on a host at all
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
therefore not race. It does emit a `__cxa_atexit` registration for the destructor, so that
path is live.

**It does not race, but not for the reason this used to give.** The old reason — "the loader
launches CPUs strictly one at a time" — is true of the Windows and Linux loaders, which loop
`call_on_cpu(i, …)` blocking per processor, and is *not the mechanism* under UEFI: since
`1c8bfdd` `number_of_cpus()` returns 1 unconditionally, the boot processor is launched alone,
and application processors are adopted much later from the guest's own start-up IPIs. So the
serialisation the comment appealed to is absent on the platform that matters most.

The real reason is structural and platform independent: `instance()` has exactly three
callers — `zpp_hypervisor_main`, `zpp_x86_64_exception` and `zpp_ap_start_up_main` — and the
latter two are only *reachable* once the boot processor has armed them from inside `main`. The
host IDT is built by `initialize_host_idt` and loaded by `main`, both boot processor only; the
trampoline page is written by `initialize_start_up_memory`, also boot processor only, and that
is what puts `zpp_ap_start_up_main`'s address where a starting processor will find it. Both
therefore come alive strictly after construction returned.

What would break it: a fourth caller reachable before `main` has armed those two. That is the
thing to check, and it is checkable by grep — which the old justification was not.

Consequences worth remembering:
- **A container as a global costs you an init array entry.** `zpp::allocator`'s default
  constructor calls `crt::heap()`, so it is not `constexpr`. That is legal now, just not
  free — prefer locals or members.
- Anything allocating from a constructor is fine: `crt::init::main()` brings the heap up
  before walking the arrays.
- Verify on the built ELF rather than by inspection. `llvm-nm -u` must report **no undefined
  symbols**. `llvm-readelf -S … | grep init_array` is no longer empty — the hypervisor log's
  line list is a namespace-scope `zpp::list<zpp::string>` and needs a constructor — so
  `check-invariants.sh` reports that as a WARN, not a failure. An entry appearing should
  still be a deliberate choice rather than a surprise.

**Dynamic initialization works, and the reason it once did not was the ELF loader.** Worth
keeping, because the false lead cost a lot: the first `.init_array` entry the tree ever had
hung the boot CPU inside `crt::init::main()`, and every plausible suspect — the `list`
constructor, `zpp::allocator`'s call to `crt::heap()`, the `__cxa_atexit` registration, the
array bounds, the ordering against `g_heap.init` — was innocent. So was the *shape* of
`elf_file::relocate`, which is why reading it proved nothing.

The bug was one trait spelling in `elf_file::relocate`:

```cpp
using relocation_kind = std::remove_pointer_t<std::remove_cv_t<decltype(relocations)>>;
```

The variant holds `const elf_rela *`. `remove_cv_t` strips *top level* cv, and the top level
there is the pointer, which is not const — so it did nothing, and stripping the pointer
afterwards left `const elf_rela`. That is not `elf_rela`, so
`if constexpr (std::is_same_v<relocation_kind, elf_rela>)` was **false for RELA files** and
every relative relocation went down the REL branch, `*target += base`. `sizeof` is the same
either way, so the stride, the entry count and every `r_offset` were right — only the value
written was wrong, and since a RELA file leaves the target word `0`, every relocated slot
came out as **exactly the module base**. The fix is to strip the pointer first and the cv
second.

That was silent for as long as it was, because nothing ever *read* a relocated slot: the
targets were a vtable and its pointers in `.data.rel.ro` that no live code touches, plus
`__dso_handle`, whose address is taken and whose value is never used. `.init_array` was the
first slot ever dereferenced, so the boot CPU called the module base, executed the ELF header
as code and took a `#UD` at `base + 0x40`, inside the program header table. The clue that
identifies this instantly if it ever recurs: **a fault at a tiny offset from the module base**.

Lessons that generalize past this bug:

- A `constinit`-only codebase never exercises its own loader. The relocation writes had never
  been checked against memory, only against the source. Read the *result*: break at the entry
  point and compare `x/gx base+<r_offset>` against `base + r_addend` from `llvm-readelf -r`.
- `std::remove_cv_t` on a pointer-to-const is a no-op. Prefer
  `std::remove_cvref_t<decltype(*p)>` when what you want is the pointee.
- `#UD` on this target means `__builtin_trap()` *or* executed data. Do not assume the former.

One latent trap found while looking: `__preinit_array_start` and `__preinit_array_end` are
both link-time address `0`, which PC-relative addressing turns into *the module base* at
runtime rather than `0`. They are equal, so the preinit loop is a no-op and this is harmless
today — but it would walk from the module base if they ever differed.

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

Both teardown paths have now been run under Bochs, not merely read. The probe was a temporary
namespace-scope object with a destructor plus an `__attribute__((destructor))` function, each
recording the order it ran in, reported out through the error code the loader prints — the
hypervisor has no other channel. Result: the `__cxa_atexit` registrations ran first and in
reverse registration order, `.fini_array` ran after them, and the log list's own destructor
freed its nodes. Note `.fini_array` slots are `R_X86_64_RELATIVE` too, so the relocation bug
above broke that path identically; it was just even further from ever being reached.

### The hypervisor's own pages are not writable and executable

The host page table maps this module read-only as a floor and then adds what each
PT_LOAD segment's program header asks for, through `elf_file::protect` and
`page_table::add_protection`. Text is read-execute, read-only data is read, `.data` and
`.bss` are read-write-no-execute, and the padding between segments is read-only. The
loaders are unchanged: they allocate one RWX region because that is what a platform
allocator gives them, and relocation writes into pages protection would close.

What that means for anything added here:

- **Nothing writes to `.text` or `.rodata` any more.** A store into either takes `#PF`
  with error code 3, and on the launch path that comes out as loader error `0x60e03`.
  Recognise it: it is *not* the relocation bug's signature, which is a fault at a tiny
  offset from the module base with vector 6 or 14 on a *fetch*.
- **Nothing executes from `.bss`.** The 512 KB per-processor stacks, the 20 MB heap
  arena, the EPT tables and the VMCS regions all lose execute with it. There is no code
  generation in this tree, so nothing needed it.
- **Two preconditions, both checked rather than assumed.** CR0.WP is set beside the CR3
  load and carried in `host_control_register_0` for root mode, the VMCS host field and the
  application-processor trampoline - without it a read-only mapping denies nothing at ring
  0 (SDM 5.6.1). IA32_EFER.NXE must be set or bit 63 is a *reserved* bit and every mapping
  carrying it faults on any access, not only a fetch (SDM 5.5.4); `main` refuses to launch
  with `execute_disable_not_enabled` rather than take that fault. `BACKLOG.md` 20 records
  that the guest can still clear NXE, and what separating the two EFERs would cost.
- **One page is still writable and executable**: the start-up trampoline below one
  megabyte, which writes its own progress marker and per-processor data area while
  executing from that same page. It cannot be split - a start-up IPI names a page.
- **Build with `-DZPP_TEST_MODULE_PROTECTION=ON` to check it is real.** It stores a byte
  of `.text` back over itself and the launch ends in the fault code above. A protection
  nobody has watched fault is one that may not have been applied - every step of it fails
  silently and in the safe-looking direction. `scripts/check-bootable.sh` refuses such a
  loader, since it ends in a verdict rather than a guest.

### Include order (critical for freestanding)

1. `cmake/freestanding-config/` — `__config_site` overrides
2. libc++ headers — `<cstdint>`, `<expected>`, etc.
3. `cmake/freestanding-libc/` — minimal C stubs for `#include_next`
4. Clang builtins — `stddef.h`, `stdint.h`

## Checking architectural claims

Recalled knowledge of the SDM is a good way to form a hypothesis and a bad way
to settle one. Every architectural claim this project acts on gets checked
against **both** references before it goes in, and the check is cited in the code:

```sh
./scripts/fetch-references.sh      # into .references/, git ignored
grep -n "wait-for-SIPI activity state" .references/sdm.txt
grep -n "kvm_vcpu_reset" .references/kvm/x86.c
```

- `.references/sdm.txt` — the Intel SDM, all four volumes, as text with
  `[[PAGE n]]` markers so a hit can be cited.
- `.references/kvm/` — KVM's x86 sources, pinned to a tag, as the reference
  implementation. Read how it *implements* a behaviour, not only how it handles
  ours from underneath.

Cite the section, table or function in the comment, and **only cite what was
actually looked up** — an unverified section number is worse than none, because
it stops the next person checking.

The two references disagree sometimes, and the disagreement is the interesting
part. Worked example, which is why this section exists:

- SDM Table 12-1 gives CR0 = `60000010H` in the INIT column. Taken literally
  that sets CD and NW, disabling the processor's caches for the rest of its
  life.
- KVM writes `X86_CR0_ET` plus CD/NW *preserved*, with a comment saying the SDM
  contradicts itself.
- Footnote 2 on that very row settles it: "The CD and NW flags are unchanged,
  bit 4 is set to 1, all other bits are cleared." KVM is right and the column
  is the power-up value.

Three positions were held on that question in one afternoon, two of them wrong,
and only the footnote ended it. The same exercise found that our INIT emulation
never reset the guest's general purpose registers, which both references agree
it must.

## What the guest is told

Everything this VMM presents to its guest lives in the exit handler in
`hypervisor/src/hypervisor/hypervisor.cpp`. Every case in it is required for
Windows to boot, and each was found by Windows failing in a way that pointed
somewhere else entirely.

- **VMX is hidden** — CPUID leaf 1, ECX bit 5 cleared, and CR4.VMXE reads back
  clear through its read shadow. The two are keyed on the same
  `nested_vmx::enabled` constant and must stay that way: "no VMX in CPUID, VMXE
  set in CR4" exists on no real processor, and a guest that trusts CR4 faults on
  its own `vmxon` — `BACKLOG.md` item 1. Hyper-V launches ahead of Windows
  whenever VBS is on and stands down cleanly when it finds no VMX.
- **The VMX instructions fault, they do not halt.** All thirteen exit
  unconditionally in non-root operation (SDM 28.1.2), and until `8412b76` every
  one of them fell to `default:` and halted the processor — a guest instruction
  could stop a CPU. They now take `#UD`, which is the answer a processor with
  CR4.VMXE clear gives (SDM 28.1.1 puts invalid-opcode above the exit).
  **Nothing a guest can execute may reach `default:`**; a case that faults is
  always available and always better.
- **Nested VMX exists but is off** — `-DZPP_NESTED_VMX=ON`, default off. On, the
  guest is told VMX exists and `nested_vmx.cpp` answers the instructions against
  a shadow VMCS in the guest's own region. `VMLAUNCH` is still refused, so a real
  guest hypervisor fails at launch instead of standing down, which is worse than
  the lie. `hypervisor/include/zpp/hypervisor/nested_vmx.h` lists what has to
  exist first; the short version is nested EPT.
- **The whole hypervisor CPUID range is answered**, `0x40000000`–`0x4fffffff`,
  not just the leaf holding the signature. Unanswered leaves fall through to
  whatever is underneath, and underneath is not nothing. A guest that reads
  vendor `ZppZppZppZpp` from one leaf and another interface from the next acts
  on the more specific claim, and starts using synthetic MSRs that do not exist
  here.

  **Re-read on 2026-08-17: that check was wrong, and only half of it was
  ever true.** It said "the rig does not pass `hv-passthrough`; both
  launchers run plain `-cpu host,kvm=on,topoext` and neither mentions
  `hv-` at all". Only `boot-zpp.sh` does. **`boot.sh` - the plain-KVM
  launcher, the one reached for as a control - runs
  `-cpu host,kvm=on,hv-passthrough,topoext`**, and also uses a different
  QEMU binary (`qemu-system-x86_64` against
  `/home/tc/vm/qemu-system-x86_64-new`) and 1.5 GB more guest RAM. So the
  two launchers differ in four substantive ways and a comparison between
  them is **not** a single-variable control until they are equalised.

  What survives of the original point is what matters under
  `boot-zpp.sh`, which is the launcher every measurement in this tree
  uses: it passes no `hv-` flag, so nothing underneath advertises a
  Hyper-V interface to us there. The paragraph before it asserted the
  opposite of that, and asserting it
  cost a boot and a wrong conclusion: an experiment that forwarded the whole
  range downward was read as "the guest was given the Hyper-V frequency MSRs
  and still failed", when those MSRs were never there to give. **Read the
  launcher before arguing from what is underneath it.** The goal is to need
  nothing under us at all, so anything that only works while something else
  implements it is not a fix.
- **Unimplemented MSRs fault.** An access outside the two ranges the MSR bitmap
  covers, `0`–`0x1fff` and `0xc0000000`–`0xc0001fff`, exits *unconditionally* —
  no bitmap can stop it, which is why an all-zeroes bitmap does not. Nothing
  real lives outside those ranges, so the answer is the one bare hardware
  gives: `#GP`, with RIP left at the faulting instruction.
- **Unhandled exits stop the CPU.** They are not resumed from, because the
  resume path advances RIP by the faulting instruction's length, so the guest
  silently skips it and continues as though it had worked. This is for exits
  that indicate a bug *here*, never for an instruction a guest chose to
  execute — see the VMX case above for why.

The recurring mistake in all of these is the same: answering *part* of an
interface, or resuming as though an unhandled instruction had succeeded. Both
produce a guest that has been lied to, and it fails later somewhere unrelated.
Windows reported `0xc000000d`, `STATUS_INVALID_PARAMETER`, on a screen blaming
its own boot configuration data; the boot configuration was fine, and the cause
was a skipped `rdmsr` of `0x40000022`. **When adding a case, answer the whole of
whatever it is, or fault.**

### Chainloading a boot manager

`uefi_loader/src/main.cpp` has to do more than load a file:

- **Connect every controller first.** Firmware connects only as much as it needs
  to reach its own boot option, so a disk nothing has booted from carries no
  block IO handle at all — the passed-through NVMe was enumerated as a PCI
  device and invisible as a file system until driven. This is also why the EFI
  shell shows no `fs1:` there until `connect -r`.
- **Require the boot manager's configuration beside it.** Picking whichever file
  system `LoadImage` first accepts is too weak: a recovery or vendor partition
  can carry its own `bootmgfw.efi`, and starting that copy fails because the
  `BCD` is not there. Agreeing with the firmware's own boot option is *not*
  evidence of a correct choice either — firmware generates options by scanning
  for bootable files, by the same weak test.
- **Forward the boot option's optional data** as the started image's load
  options, which is what the firmware's own boot path does. Auto-generated
  options carry none, so an empty result is normal.
- `BootOrder` is not a complete list of options. Firmware regenerates and
  reorders it as devices come and go, so an option can be present in NVRAM and
  absent from `BootOrder`.

### Reading hypervisor state from a debugger

Once the guest is running there is no other channel: there is no console, the
serial port belongs to the guest, and a debugger attached from outside sees
guest state only — the VMCS fields that decide whether a CPU runs at all cannot
be read without being on that CPU with that VMCS current. So the interesting
state is recorded into members as it happens:

- `exit_trace` / `exit_trace_count` — a ring of the most recent exits per CPU,
  sampled after handling, so it shows what the guest was about to be resumed
  with. Newest entry is at `(count - 1) % capacity`.
- `unhandled_exit`, `vm_entry_failure` — filled in immediately before the CPU
  stops. Check `occurred` first; the rest is meaningless until it is set.
- `zpp::hypervisor::log_storage::lines()` — the hypervisor's own log as text,
  oldest first, written with `log("...{}", value)` from
  `zpp/hypervisor/log.h`. The records above say what the state *was*; this says
  what *happened*, in order, across CPUs. A record only describes the last of
  its kind, and a boot failure is usually a sequence — the `rdmsr` of
  `0x40000022` that cost an afternoon is one `log` line now.

  Where its memory comes from is settled in `log_storage` alone, behind the
  `line` and `line_list` aliases, so moving the log to a per-CPU heap is a
  change to that class and to nothing else.

### Reading the log ring

Four thousand lines, and a line identical to the one before it does not take
a slot — it grows a `[times=N]` marker on the line already there. Both of
those exist for the same measured reason: before them, all 512 lines of a
real boot were the same shadow EPT rebuild and the sequence being chased had
been evicted. **Do not remove the deduplication in favour of a bigger ring.**
One event inside an exit handler repeats faster than any ring can absorb.

Getting it out, with no restart and nothing perturbed:

```sh
# 1. QEMU can open a gdb stub on an ALREADY RUNNING guest, from its monitor.
#    This is the whole trick - a wedged guest does not have to be re-run,
#    and re-running it is usually how the state gets lost.
printf 'gdbserver tcp::1234\n' | nc <rig> 4446

# 2. The module base is on serial, and is NOT stable across runs.
ssh <rig> 'grep -ao "allocate_rwx done at 0x[0-9a-f]*" serial.out | tail -1'

# 3. Attach, in tmux, and load symbols at that base.
x86_64-elf-gdb -q
  (gdb) target remote <rig>:1234
  (gdb) add-symbol-file out/debug/x86_64/zpp_hypervisor -o <base>
  (gdb) source scripts/zpp.gdb
  (gdb) zpplog          # the log, oldest first
  (gdb) zpph            # $h = the singleton, for $h->exit_trace and friends
```

`scripts/zpp.gdb` walks the list and the strings by hand. It has to: the
hypervisor is built against libc++ headers only, gdb has no libc++ pretty
printers loaded, and a `std::list<std::string>` therefore prints as raw
nodes and unions.

To capture the whole ring rather than a screen of it, `set logging file …`,
`set logging redirect on`, `set logging enabled on`, then `zpplog`.

Two things that made this work where the earlier attempts did not:

- **`info threads` first.** It says which CPUs are inside our module and
  which are parked in firmware, by symbol, before any member is read — that
  one line said more than an hour of reading memory by offset had.
- **Do not compute member offsets by hand.** `llvm-dwarfdump --name=<member>`
  will happily answer with a *different* DIE of the same name, and the
  resulting addresses read as plausible zeroes. With symbols loaded,
  `$h->member` is both correct and checkable.

Build with `-DZPP_HYPERVISOR_WAIT_FOR_DEBUGGER=ON` and the hypervisor spins at
its entry point until released with `set var gdb_attached = 1`. Use it. Racing a
gdb attach against the guest does not work when the failure being chased kills
the guest: symbols cannot be loaded until the loader has mapped the module, and
by then it is over.

Two things that cost time:

- `load-symbols` needs an address inside the module, and gdb resolves *types*
  relative to the selected frame — so `(zpp::hypervisor::hypervisor *)` fails
  with "No type ... in namespace" whenever no CPU happens to be inside our code.
  Either select a thread that is, or read the members as raw memory at
  `symbol + offset`, taking offsets from
  `llvm-dwarfdump --name=<member> --show-children` and the singleton's address
  from `llvm-nm`.
- The module load address is printed on serial (`allocate_rwx done at …`) and is
  **not** stable across configurations, so read it per run rather than reusing it.

### Never boot a ZPP_VERIFY_HYPERVISOR build

`verify::present` is destructive by design: it takes every application
processor the firmware had parked and leaves it **halted in real mode**,
and leaves the boot processor's local APIC in x2APIC mode. A firmware that
then waits for those processors spins for ever - EDK2's `MpInitLib` does
exactly that in `WaitApWakeup`, on a per-processor `WAKEUP_AP_SIGNAL`
semaphore - and the machine never reaches the operating system.

**This cost a full day.** The switch persisted in a CMake cache and was
inherited silently by every later build. The resulting hang was
indistinguishable from a hypervisor bug, and seven candidate causes were
bisected and eliminated - the instruction decoder, the whole diagnostic
channel, NMI exiting, the module base hand-over, the mapping window size,
connecting controllers early, per-exit logging - before the *build option*
was suspected. `git bisect` could not find it either, because every commit
tested carried the same stale cache entry.

Three things now stop it recurring, and the first is the one that matters:

- `scripts/check-bootable.sh` greps the built loader for strings only
  `verify::present` emits and refuses. It is wired into
  `deploy-to-rig.sh` and `rig-boot-test.sh`, so the bytes cannot reach a
  real machine without passing it.
- A verify build no longer chainloads at all. It used to return early only
  on *failure* and fall through to the chainload on success, which is the
  wrong way round - a passing destructive check is exactly the case that
  must not continue. It now prints its verdict and stops.
- The loader says so on serial, unmissably, before stopping.

The general lesson, worth more than the specific bug: **when a hang
survives every code change you can think of, suspect the build, not the
code.** Check what is actually compiled in - `strings` on the binary
answers it in seconds, and it is how this was finally found: the working
binary lacked `hypervisor_bit=` and `leaf 0x40000000 ebx=`, which only
`verify.h` emits.

### An address that moved is not a guest that never started

Two members added to the singleton grew the binary, the module base
moved `0x67210000` -> `0x6720f000`, and a script with the old base
hardcoded read a singleton that was not there. It reported **every
counter as zero for eight minutes** - `exits 1`, every histogram empty,
`coverage 0.0%` - which is indistinguishable from a guest that never
started, and is exactly the failure somebody would then go and debug.

This is the second member of the same class, and the pair is worth
stating together because the fix is the same both times:

- **The singleton's offsets move when a member is added**, so a reader
  pointed at a rebuilt ELF against a deployed older binary reads
  plausible garbage. `rig-dump-state.py` resolves offsets from the ELF
  for this reason, and `--elf .rig-deployed-hypervisor.elf` is what
  `deploy-to-rig.sh` keeps so the reader can be pointed at the binary
  that is actually running.
- **The module base moves when the binary's size changes.** Read it per
  run - `grep -ao "allocate_rwx done at 0x[0-9a-f]*" serial.out | tail -1`
  - never carry one between builds.

The tell that distinguishes them from a real failure, and it takes one
line: `reader proven: host_page_table[0] = ...023`. A wrong base fails
that; a stopped guest does not. **Check the reader before believing the
reading**, which is what that line is for.

### A CMake cache reading ON is not evidence. Read the manifest.

That class has now cost this project three separate things: a build
switch declared and forwarded but never added to the compiler's list
(`cmake/hypervisor/CMakeLists.txt` records it), four counter readings
that were fiction, and `ZPP_PUBLISH_REFERENCE_TSC` - ON in both caches
and in `compile_commands.json`, with a *stale object file*, so
`publish_reference_tsc_page` linked in as a bare `ret` and two sessions
of measurements were taken of a configuration nobody had built. It was
the switch aimed at the largest exit reason on the machine.

So the binary now says what it is:

```sh
strings out/debug/x86_64/zpp_hypervisor | grep 'zpp switches'
zpp switches: nested=1 evmcs=0 shadowvmcs=1 tpr=1 reftsc=1 selfipi=0 \
              defer=1 shadowgs=0 stepvtl=0 profile=0 stretch=1
```

`hypervisor/src/hypervisor/build_switches.cpp` assembles that from the
same `constexpr bool`s the code branches on - **not** from the `-D`
macros behind them, which would have agreed with the cache and been just
as wrong. `check-bootable.sh` prints it on every deploy, so it is on the
path to the rig and cannot be skipped.

**Check it before turning anything on, and before believing any run.**
Adding a switch is now four edits, not three: the option, the forward,
the compiler's list, and a field here.

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

### Reading the guest's own memory, including its blue screen

The guest's kernel is readable from outside with **no rebuild and
nothing perturbed**: walk its page tables by hand with the QEMU
monitor's `xp`, using a guest CR3 the state dump already prints. The
guest hypervisor's extended page tables have measured identity for
these pages - the VP assist page reads the same address by all three
paths - so a second-level physical address is an `xp` address directly.

```python
# index by (va >> 39) & 0x1ff, then 30, then 21, then 12
# `xp /1gx` prints the address column with NO `0x` prefix
```

This is the only way to see a blue screen on this rig: the display is a
passed-through GPU, so QEMU answers `screendump` with **"There is no
console to take a screendump from"**. Read `KiBugCheckData` instead -
five words, the stop code and its four parameters, written before
anything is displayed. Its address comes from the PDB, and the kernel
base from the hypervisor's own log (`second-level guest kernel image at
...`, different every boot: KASLR).

Two things this has already settled that nothing else could:

- **`KeQuantumEndTimerIncrement` = 17,400.** Windows' 574.7 Hz tick is
  its own hardcoded constant, not anything this VMM tells it.
- **Bugcheck `0x1CA` from `HalpWatchdogCheckPreResetNMI`**, which is
  what a guest lied to about time does here.

Traps:

- **The monitor takes one connection.** A socket left open makes
  `rig-dump-state.py` fail with no diagnosis, and a poller that leaks
  one reports every field as `None` for ever after. Close it.
- **`-no-reboot` alone destroys what you were about to read**: QEMU
  *exits* on the guest's reset, taking its memory with it. Use
  `-no-reboot -no-shutdown`, which leaves `paused (shutdown)` with
  everything intact. Pass both through `ZPP_QEMU_EXTRA`.
- The guest's symbols are per build. `llvm-pdbutil dump --publics`
  gives `segment:offset` with the **offset in decimal**, and the
  segment indexes the PE section table - reading either as hex puts
  every symbol somewhere plausible and wrong.

### Windows' tick is 574.7 Hz and that is not negotiable

Settled by disassembly and confirmed against the running guest, so it
does not need re-deriving: the guest arms a *periodic* 1.74 ms
synthetic timer because `KeQuantumEndTimerIncrement` is a literal in
`ntoskrnl.exe`, behind a `Feature_ShortThreadQuantum` gate whose test is
constant-folded on. `KeMinimumIncrement` is 5,000 and
`KeMaximumIncrement` is 156,250, so 17,400 is neither a clamp nor a
rounding of either.

**The "the guest never leaves the clock handler" reading of that is
withdrawn - measured 2026-08-21, and it was wrong.** It said a tick costs
this VMM about 2.46 ms so the handler cannot finish inside its own period.
The gap histogram says otherwise:

    time-stamp counter between clock interrupts (241,551 gaps, 1.992 GHz)
      2^21 ( 1.05 - 2.11 ms)   232,690   96.3%    <- the 1.74 ms period, met
      2^22 ( 2.11 - 4.21 ms)     4,139    1.7%

    virtual task priority at second-level entry (1,085,015 entries)
      0x00    27,169   2.5%      0x10     2,777   0.3%   <- PASSIVE_LEVEL

96.3% of intervals land in the bucket holding the guest's own 1.74 ms
period, and it enters at task priority `0x00` twenty-seven thousand times.
**The guest meets its clock and reaches PASSIVE_LEVEL constantly.** What is
still true is the *shape* of the hot set - eight instruction pointers, all
in the clock path, zero new memory - but that is a guest **waiting**, not
one saturated. Those look identical in a profile and are opposite problems.

**Four attempts to help the guest cope have now failed**, and the reason
they all failed is most likely this: every one of them was aimed at a
saturation that is not happening. That is a better account than the four
separate ones recorded beside them.

| intervention | outcome |
|---|---|
| `ZPP_STRETCH_GUEST_TIMER=8` - multiply the period | bugcheck loop |
| `ZPP_DELIVER_SELF_IPI` - inject the vector it asked for | one delivery, then a spin |
| `ZPP_TICK_FLOOR=156250` - refuse a shorter period | one refusal, then shutdown |
| `ZPP_TIME_DILATION=8` - slow *every* clock together | bugcheck `0x1CA` |

The fourth was built specifically to avoid what the first three shared -
changing one thing and leaving the rest honest - and it failed for two
independent reasons, both measured. **The guest hypervisor's clock is
not the time-stamp counter**, so a VMCS TSC offset does not reach it:
the fitted reference-page scale came out 3.59x larger and the clock-gap
histogram did not move at all. And Windows *checks its clocks against
each other* - `HalpWatchdogCheckPreResetNMI` is the only reachable
caller of bugcheck `0x1CA`.

So there is no lie about time left to tell, and the tick rate cannot be
moved from underneath. What remains is the per-exit cost, which is a
different question and is not this one.

### Never single-step the guest through QEMU's gdbstub

`stepi` on a guest thread under QEMU/KVM does not just fail, it **destroys what
you were measuring**. KVM implements `KVM_GUESTDBG_SINGLESTEP` by ORing
`X86_EFLAGS_TF` into the guest RFLAGS; with pending-debug-exceptions `BS` still
clear that combination fails KVM's nested guest-state check, so the next
`vmresume` takes an entry-failure exit. Measured: `vm_entry_failure` recorded
`reason=0x80000021` with `guest_rflags=0x102`, TF set where nothing in this tree
writes it, and the CPU then sat in `on_vm_entry_failure`'s halt loop. The stub
also never returns a stop reply for the stepped thread.

Use the QEMU **monitor** instead — `info registers -a`, `xp` — which reads state
without perturbing it. Or arm the Monitor Trap Flag from inside the VMM, which
forces an exit after one retired instruction and answers "is it executing?"
without a debugger at all.

### Read device registers narrow, and repeat

`xp` is trustworthy on RAM and **not** across a passed-through device's
BAR. Measured on the rig's NVMe: `xp /16xw` over the controller's
registers reported `CC.EN = 0` and `CSTS.RDY = 0` — a disabled
controller — while `xp /2xw` of the same address, repeated sixty times
without one disagreement, reported `CC = 0x00460001` and `CSTS = 1`.
The wide read was about to be reported as "the disk is dead".

The tell was **possibility, not plausibility**: the wide dump carried
`0x00000030` at offsets 0x04, 0x14 *and* 0x24, and three unrelated
registers do not hold the same value at the same position in three
consecutive lines. Ask whether a reading is possible before asking
whether it is believable.

Two rules follow, and they sit beside the reader-proof rule that
`rig-dump-state.py` prints (`reader proven: host_page_table[0] = …023`):

- **On memory-mapped device registers, read one or two words at a time
  and sample repeatedly.** Wide reads are for RAM.
- **NVMe doorbells are write-only.** Reading one is architecturally
  undefined, so a doorbell reading zero says nothing about whether
  anything was submitted — and it looks exactly like proof that no I/O
  was issued.

### Census two fields, not one, and let them disagree

Three instruments in one session gave confident answers to questions
nobody asked, and none of the three looked wrong:

| instrument | what it reported | what was true |
|---|---|---|
| wide `xp` over a VFIO BAR | `CC.EN = 0`, controller disabled | enabled and ready |
| an NVMe doorbell read | zero, "no I/O submitted" | write-only, undefined |
| a census over RDX at a hypercall | "the same request 99.8% of the time" | RDX is a sentinel; RBP changes every call |

The last is the sharpest, because it did not merely mislead — it would
have **confirmed** the hypothesis under test. A loop was suspected, and
censusing the wrong register reported near-total repetition.

**A single-field instrument cannot tell you it is aimed at the wrong
field.** It has nothing to disagree with. All three of these were caught
by reading a *second* thing — a narrower read, the specification, the
other register — and none by doubting the first.

So when the field that carries the value is not certain, **record both
candidates and let the data say which one moves.** It costs one more
word per sample and it is the only thing that distinguishes "this value
never changes" from "I am not reading the value".

### KVM's own statistics are on the rig, and they are the cheapest instrument

**This section used to say the target had no tracefs and no
`/sys/kernel/debug/kvm`. That was wrong about both, and designing around
their absence cost a session.** The rig runs a KVM built with tracepoints
(93 events under `/sys/kernel/tracing/events/kvm`), and debugfs simply is
not mounted at boot:

```sh
sudo mount -t debugfs none /sys/kernel/debug
ls /sys/kernel/debug/kvm/<pid>-<fd>/          # ~60 per-VM counters
```

Two of them answer, from the host, without touching the guest, what
otherwise needs a full state dump:

- **`nested_run`** - KVM's entries into *its* guest, which is this VMM
  plus everything under it. **Not the same quantity as the resident
  `l2-entries`**, and this section said it was. Measured over one
  352-second window on a settled guest: `nested_run/s` 5,334 against
  this VMM's own `exits/s` 5,319 and its `l2-entries/s` 2,653. It tracks
  **our exit count** - every exit we take is one re-entry by KVM - and
  is about twice the second-level entry count.
- **`exits`** - every VM exit L0 took. Divided by `nested_run` it gives
  VMCS accesses per second-level entry, which is the nesting tax in one
  number.

**Read them as deltas over a window**, never cumulatively - a boot has
phases and the cumulative figure averages them. Three consecutive 20 s
windows on a settled guest agreed to 1.5%, which makes `nested_run/s` the
most reproducible metric in this tree and the right headline for any
comparison. Prefer it to anything needing a dump.

`/proc/<tid>/stack` really is absent. `/proc/<pid>/task/*/schedstat` is
not, and its `run_time wait_time` pair settles "is this thread waiting to
be scheduled" in one reading - measured 0.2% wait on every vCPU thread,
which retired a whole theory for the cost of one command.

For streaming tracepoints rather than counters, the `trace-kvm` skill's
FIFO recipe still applies and its warnings still stand: `trace_pipe`
only, never `trace`, and never as an ssh child.

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
- **The guest coverage suite needs `-DZPP_DIAG=OFF`.** `ZPP_DIAG` defaults ON for a debug
  build, and with it on the loader establishes the ESP reservation and then warm-resets the
  machine so the channel is live on the next boot. Nothing after that point runs, so the
  suite never starts. Build it the way `ci.yml` does:

  ```sh
  cmake --preset debug -DZPP_GUEST_TESTS=ON -DZPP_DIAG=OFF
  ```

  This cost four runs and most of a session, and the wrong conclusions it produced are worth
  more than the fact: the emulator was suspected, then the medium, then the boot option,
  then a change to the coverage table, then the event re-queue — each eliminated by another
  seven-minute run, because the symptom is *exactly* a hang. Serial stops after the
  firmware's own `starting Boot0001` and nothing else is ever written. The loader now
  announces it with `trace::raw` (`ZPP_RESTART …`), which survives `ZPP_TRACE` being off,
  and `scripts/ci/bochs-exit-coverage.sh` fails on that line in about 28 seconds naming the
  flag. The general lesson is the one `ZPP_VERIFY_HYPERVISOR` already taught in the section
  above: **when a hang survives every code change you can think of, suspect the build.**

### Never edit a shell script while `sh` is interpreting it

`sh` reads a script incrementally and remembers a byte offset, so rewriting the file under a
running interpreter makes it resume in the middle of whatever now occupies that offset. Seen
here as `bochs-exit-coverage.sh: line 107: chine: command not found` — the tail of `machine`
— which killed a coverage run that had already produced 142 result lines. Edit a copy, or
wait.

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
  it. **Use whatever clang-format you have, 22 or newer** — whatever LLVM ships is fine.
  The version is deliberately *not* pinned: majors do disagree on formatting, but the pin
  cost more than the disagreement, since anyone with a current LLVM had to install a second
  clang-format to touch one line. When a new major reflows something, reformat the tree in
  its own commit rather than pinning against it.
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

## Working mode

**Treat the objective as standing until its done-condition is met.** Not the
next step - the objective. State the done-condition out loud at the start
("the channel logs continuously while Windows runs, verified on the medium")
and keep going until it is met or genuinely blocked.

Stop for exactly three things:

- an irreversible action outside what has already been authorised,
- a decision the evidence cannot settle,
- done.

Everything else: pick the next item and go. In particular, these are **not**
reasons to stop and hand back control:

- a verified increment (commit it and continue),
- a failed experiment (a failure is an input to the next step, not a place
  to stop and explain),
- a milestone worth reporting (report *while* passing it, not instead of
  passing it),
- permission that has already been given once.

**Report as you pass, not instead of passing.** A run that produced a number
is worth a line; it is not worth a summary and a wait.

**Document decisions and alternatives as they are made**, in the commit that
makes them and in `BACKLOG.md` when they shape future work. What was chosen
is half the record - the other half is what was rejected and the measurement
or citation that rejected it. Several decisions in this tree were reversed
later by somebody re-deriving the same argument, because only the conclusion
had been written down. Specifically:

- when a choice was settled by a measurement, record the measurement,
- when it was settled by the SDM or KVM, cite the section or function,
- when an alternative was rejected, say which and why, so it is not
  re-proposed,
- when something is switched off, the switch's comment carries the reason
  and what would have to change to switch it on.

## Integrating branches

**Rebase onto the target and fast-forward. Never create a merge commit.**

```sh
git switch <branch> && git rebase develop
git switch develop && git merge --ff-only <branch>
```

The history stays linear, which matters more here than it would elsewhere:
every commit carries the reasoning for its change, and reading them in order
is how that reasoning is recovered months later. A merge commit adds a node
that explains nothing and breaks the sequence.

If `--ff-only` refuses, the rebase was not done or the target moved while it
was happening. Redo the rebase - do not fall back to a merge. This applies to
agent worktree branches too, before they are integrated.

## Dependencies (auto-fetched by CMake)

- Windows SDK/WDK headers + libs: NuGet packages via `cmake/dependencies.cmake`
- EDK2 headers: GitHub archive via FetchContent
- Linux kernel headers: inside Podman container (Alpine linux-headers)
