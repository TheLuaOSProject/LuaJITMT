# Bounded multi-threaded table-generation retirement

Table node and separated-array generations are no longer retained for the
entire lifetime of a process merely because more than one TG is attached.  The
old `gc2_n_threads > 1` reclamation veto hid raw C scans which can survive a
safepoint and made resize churn an unbounded memory leak in persistent MT
programs.

## Reader contract

Long C scans publish an owner-written `{tab_read_depth, tab_read_epoch}` pair in
their `TGState` before acquiring a raw array/node generation.  Nested scans keep
the epoch of the outermost scope.  Enter publishes the epoch before the nonzero
depth; leave release-publishes the decremented depth and clears the now-stale
epoch after an outermost leave. Reclaimers key on depth, so this ordering never
exposes an active reader with a cleared epoch. This adds no shared RMW and does
not change the VM/JIT fast table lookup path.

The covered long-scan sites are table-library length/maxn validation, parser
constant fixup, bytecode template emission, and recursive buffer
serialization/dictionary preparation.  Recorder template cleanup was instead
restructured to copy one key and reacquire the current generation after every
yield-capable call.  Resize sizing now abandons its raw snapshot before a retry
wait, allocates replacement/retire storage without dereferencing the old
generation, and initializes retire records only after structural ownership and
the current-root recheck.  Forward/length helpers similarly reacquire the table
root immediately after a wait.

Every VM protected-call boundary checkpoints the current TG's table-reader
depth and outer epoch.  On return (success, yield, or caught error), a checked
owner unwind drops only reader scopes opened below that boundary.  An outer raw
scan therefore remains pinned across a nested protected call, while a throw
from parser fixup, serialization, bytecode writing, or a table-library scan
cannot retain a generation forever.  The unwind rejects depth underflow or an
outer-epoch change, republishes the saved epoch before a nonzero depth, and
publishes depth zero before clearing the stale epoch.  Detaching a TG with a
nonzero pin remains an unrecoverable scope bug and aborts before lifecycle
detachment.

## Nonwaiting reclaim

The ordinary GC2 retired-metadata writer (`smr_reclaiming=1`, zero counted SMR
readers) computes the minimum active table-read epoch once per batch.  An armed
record is physically freed only when:

- two handshake epochs have elapsed;
- no active table pin has `epoch <= retire_epoch`; and
- a counted, non-marking body lease proves either that the original table is
  gone/differently typed, or that a live table no longer publishes the retired
  vector.

An old pin makes the record get pushed back without waiting or spinning.  A pin
started after retirement can acquire only the replacement root and has a newer
epoch, so it does not delay the old generation.  Direct multi-TG calls outside
the metadata writer gate fail closed; focused single-TG test calls remain
supported.

Retire records keep their owner identity until the bounded drain so unexpected
old-generation republication remains detectable. `ret->tab` is identity
metadata, not a semantic root: retired-vector scans retain only the raw record
and vector. The cold reclaim check uses `lj_gc2_tab_generation_current()`, which
holds a counted small-arena body lease across exact table validation and the
current-vector comparison without changing semantic mark state. A real table
edge therefore still reports NEW and traverses the child graph.

## Coverage

`t-tab-retire.c` covers writer-gate rejection, an old pin requeue, reclaim after
pin release while two TGs remain attached, owner-root republication, and both
zero-depth and nested protected-error unwind. `t-gc2-traverse.c` starts a mark
cycle, retires a table generation, proves retirement does not semantically mark
the owner, and then proves the first real owner edge traverses the table's child
graph.

This restores the plan's I-4 intent while explicitly supporting the few C scans
that must cross safepoints instead of relying on a process-wide MT leak.
