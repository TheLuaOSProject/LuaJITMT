# VM stack-topology dirty publication (2026-07-11)

The x64 VM now increments the owning TG's `stack_dirty_epoch` after three stack
write families which previously left a completed topology change unstamped:

- `vm_return`, after the result-copy loop (the existing earlier stamp can race
  the copies);
- taken `ISTC`/`ISFC`, after copying the selected value into `RA`;
- `VARG`, after fixed-result, nil-fill, copy-all, and grow-stack paths converge.

The cold collectable table-read root wrapper no longer repeats a dirty increment
and release self-store. Every in-tree DynASM caller stores its destination and
increments `stack_dirty_epoch` before branching to that wrapper, so it now only
performs the GC2 liveness barrier.

These stamps invalidate owner-scan snapshots and remove avoidable duplicate
increments. They are not odd/even critical sections and do not authorize phase
close. In particular they do not pin an old stack allocation during relocation;
the exact root descriptor and stack-storage lease remain required.

Validation covers the x64 build, table-read publication stress, local-cell/return
stock behavior, vararg/return hook cases, and generated VM instruction review.
