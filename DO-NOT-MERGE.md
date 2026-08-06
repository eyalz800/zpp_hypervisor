# Do not merge this branch

It unlinks the DMAR table from the XSDT so the guest never sees
remapping hardware. That is the point of the strategy and it is also
why the branch is parked: a guest that cannot see remapping hardware
**cannot do Kernel DMA Protection**, and keeping that working is a
hard requirement for this project.

The branch is kept because the work is done and correct for what it
is, not because it is wanted. Nothing here is a fallback that can be
quietly enabled - enabling it removes a protection the guest is
required to keep.

If the reserved region strategy on `feat/reserved-region` turns out
not to work, the answer is **not** this. It is virtualizing the
remapping hardware: the guest keeps its own tables and keeps
reporting protection, while we shadow them and hold a mapping of our
own. Roughly two to three thousand lines, and the reason it is
affordable at all is that a RAM shadow page for register reads plus
the existing monitor-trap step for writes removes any need for an
instruction decoder - and that fabricating the capability register
lets us report a smaller maximum guest address width than the
hardware has, so the guest's own allocator never reaches the range we
put our window in. That last part is what makes it correct rather
than merely likely to work.
