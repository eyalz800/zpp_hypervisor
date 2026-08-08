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
