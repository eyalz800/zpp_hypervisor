# Reading the hypervisor's own state from gdb.
#
# `source scripts/zpp.gdb` after the symbols are loaded, then `zpplog`.
# See CLAUDE.md, "Reading the log ring", for the whole recipe including
# how to attach to an already running guest without restarting it.
#
# These walk the containers by hand because there are no pretty printers
# here: the hypervisor is built against libc++ headers only and gdb has no
# libc++ printers loaded, so a std::list prints as its internal nodes and a
# std::string as its raw union.

# The log, oldest line first.
#
# The list is zpp::list<zpp::string>, so each node is {__prev_, __next_,
# value} and the value starts sixteen bytes in. The string is libc++'s,
# with the small-string optimisation: the low bit of the first byte says
# which representation is in use, a long one keeps its pointer sixteen
# bytes in, and a short one keeps its characters one byte in.
define zpplog
  set $L = &'zpp::hypervisor::log_storage::m_lines'
  set $n = $L->__end_.__next_
  set $i = 0
  while $n != &$L->__end_
    set $s = (char *)$n + 16
    if (*(unsigned char *)$s & 1)
      printf "%4d %s\n", $i, *(char **)($s + 16)
    else
      printf "%4d %s\n", $i, ($s + 1)
    end
    set $n = $n->__next_
    set $i = $i + 1
  end
end

document zpplog
Print the hypervisor's log ring, oldest line first.
Repeated lines carry a [times=N] marker rather than a slot each.
end

# The singleton, as a convenience variable, so the members below can be
# read as $h->whatever.
#
# Spelled out because it is a function-local static: its symbol carries the
# enclosing function in the name, and nothing else in the tree names it.
define zpph
  set $h = &'zpp::hypervisor::hypervisor::instance()::instance'
  printf "hypervisor at %p\n", $h
end

document zpph
Set $h to the hypervisor singleton.
end

# Why a CPU stopped, and what it was doing just before.
#
# The log says what happened in order; this says what the state WAS at the
# moment a CPU gave up. Check `occurred` first - every other field in those
# two records is meaningless until it is set, and zeroes read as plausible
# values.
define zppwhy
  zpph
  printf "unhandled_exit.occurred = %lu\n", $h->unhandled_exit.occurred
  if $h->unhandled_exit.occurred != 0
    printf "  reason 0x%lx qual 0x%lx linear 0x%lx rip 0x%lx cs 0x%lx\n", \
      $h->unhandled_exit.reason, $h->unhandled_exit.qualification, \
      $h->unhandled_exit.guest_linear_address, $h->unhandled_exit.guest_rip, \
      $h->unhandled_exit.guest_cs_selector
  end
  printf "vm_entry_failure.occurred = %lu\n", $h->vm_entry_failure.occurred
  if $h->vm_entry_failure.occurred != 0
    printf "  cpu %lu vp %lu from_trampoline %lu vector 0x%lx\n", \
      $h->vm_entry_failure.cpu, $h->vm_entry_failure.virtual_processor, \
      $h->vm_entry_failure.from_trampoline, $h->vm_entry_failure.start_up_vector
    printf "  activity %lu intr 0x%lx entry_ctls 0x%lx\n", \
      $h->vm_entry_failure.activity_state, \
      $h->vm_entry_failure.interruptibility_state, \
      $h->vm_entry_failure.entry_controls
    printf "  cr0 0x%lx cr4 0x%lx rflags 0x%lx rip 0x%lx cs 0x%lx base 0x%lx ar 0x%lx\n", \
      $h->vm_entry_failure.guest_cr0, $h->vm_entry_failure.guest_cr4, \
      $h->vm_entry_failure.guest_rflags, $h->vm_entry_failure.guest_rip, \
      $h->vm_entry_failure.guest_cs_selector, $h->vm_entry_failure.guest_cs_base, \
      $h->vm_entry_failure.guest_cs_access_rights
  end
end

document zppwhy
Print the unhandled-exit and VM-entry-failure records, if either fired.
end

# The most recent exits on one CPU, oldest of the window first.
#
#   zppexits 0
#
# The ring is sampled after each exit was handled, so an entry shows the
# state the guest was about to be RESUMED with, not the state it exited in.
define zppexits
  zpph
  set $cpu = $arg0
  set $n = $h->exit_trace_count[$cpu]
  printf "cpu %d: %lu exits total, %lu distinct in the ring\n", \
    $cpu, $h->exit_total[$cpu], $n
  set $cap = 32
  set $count = $n
  if $count > $cap
    set $count = $cap
  end
  set $i = 0
  while $i < $count
    set $slot = ($n - $count + $i) % $cap
    set $e = &$h->exit_trace[$cpu][$slot]
    printf "  %2d reason 0x%lx qual 0x%lx activity %lu cs 0x%lx rip 0x%lx", \
      $i, $e->reason, $e->qualification, $e->activity_state, \
      $e->cs_selector, $e->rip
    if $e->guest_physical != 0
      printf " gpa 0x%lx", $e->guest_physical
    end
    if $e->repeated > 1
      printf " [times=%lu]", $e->repeated
    end
    printf "\n"
    set $i = $i + 1
  end
end

document zppexits
Print the recent VM exits recorded for one CPU: zppexits <cpu>.
end

# Select a processor that is inside our module.
#
# Everything above reads memory in the hypervisor's own pages, and those
# pages are hidden from the guest - the module clears every extended page
# table permission on them. So from a processor in guest context they read
# as "Cannot access memory", and the class type resolves against the
# selected frame, so `$h->member` answers "not a structure pointer" rather
# than anything useful. Both failures look like the module being gone.
#
# gdb selects thread 1 by default and thread 1 is the boot processor, which
# is the one *least* likely to be in root operation. So pick by where the
# program counter is instead of by number.
#
# Needs $zpp_base and $zpp_end, which scripts/rig-dump-log.sh sets from the
# serial log and the ELF. $zpp_cpus bounds the search.
define zppcpu
  if $zpp_cpus == 0
    set $zpp_cpus = 8
  end
  set $i = 1
  set $found = 0
  while $i <= $zpp_cpus && !$found
    eval "thread %d", $i
    if $pc >= $zpp_base && $pc < $zpp_end
      set $found = 1
      printf "reading from cpu %d, inside the module at %p\n", $i - 1, $pc
    else
      set $i = $i + 1
    end
  end
  if !$found
    printf "no processor is inside the module - every read below will fail\n"
    printf "the module hides its own pages, so guest context cannot see them\n"
  end
end

document zppcpu
Select a processor currently executing inside the hypervisor module.
Required before zpplog/zpph/zppwhy: the module's pages are unreadable
from guest context. Set $zpp_base, $zpp_end and $zpp_cpus first.
end

# The counters that say whether a nested guest is still making progress.
#
# A boot processor spinning on the VMX-preemption timer looks identical
# whether the layer above it is idle or wedged - the exit ring shows one
# reason at one RIP either way. What separates them is whether the second
# level is still being entered: an idle guest hypervisor still runs its
# guest, a wedged one does not.
define zppstat
  zpph
  set $i = 0
  while $i < 8
    printf "cpu %d: l2 entries %llu, shadow leaves %llu\n", \
      $i, $h->l2_entries[$i], $h->shadow_ept_leaves_filled[$i]
    set $i = $i + 1
  end
  printf "watched writes: emulated %llu, stepped %llu, filtered %llu\n", \
    $h->emulated_writes, $h->stepped_writes, $h->filtered_writes
  printf "offset from: decoded %llu, unknown %llu\n", \
    $h->access_offset_decoded, $h->access_offset_unknown
  printf "physical offset: present %llu, agreed %llu, disagreed %llu\n", \
    $h->physical_offset_present, $h->physical_offset_agreed, \
    $h->physical_offset_disagreed
  printf "length disagreement %llu (reported %llu, decoded %llu)\n", \
    $h->emulated_length_disagreement, $h->emulated_length_reported, \
    $h->emulated_length_decoded
  printf "nmis reinjected %llu\n", $h->guest_nmis_reinjected

  # What a guest hypervisor was last told when its own VMX instruction
  # failed. A non-zero count with l2 entries frozen means it is being
  # refused, not declining to ask - and those have different causes.
  # Error 5 is "VMRESUME with non-launched VMCS".
  set $i = 0
  while $i < 8
    if $h->nested_vmfail_count[$i]
      printf "cpu %d: %llu vmfails, last error %llu\n", \
        $i, $h->nested_vmfail_count[$i], $h->nested_last_vmfail[$i]
    end
    set $i = $i + 1
  end

  # A step that never completed. Any of these still set means the
  # monitor trap flag was armed and its exit never arrived, which leaves
  # the watched page - the local APIC's - writable for every processor
  # and this VMM blind to every write on it from that moment. It also
  # invalidates any later claim about what the guest wrote to it.
  set $i = 0
  while $i < 8
    if $h->stepping_watch[$i]
      printf "cpu %d: STEP STILL PENDING, offset 0x%llx\n", \
        $i, $h->stepping_offset[$i]
    end
    set $i = $i + 1
  end
end

document zppstat
Print the nested-entry counters. Run twice: what matters is whether
l2 entries moves, not its value.
end
