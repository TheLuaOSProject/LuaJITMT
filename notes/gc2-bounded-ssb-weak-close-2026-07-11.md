# GC2 bounded SSB drain and weak-close checkpoint (2026-07-11)

Status: implemented checkpoint, not a proof of complete weak-table, root, or
reclamation safety. The plan files are unchanged.

## Published-chain ownership

An SSB consumer no longer makes a partially consumed MPSC chain globally
walkable. It atomically detaches published work, keeps a partial suffix in the
collector-owned `ssb_drain` pointer, processes at most its caller's budget, and
only then republishes the remainder state. `ssb_consumer_active` brackets the
detach-to-remainder interval. The empty predicate samples published head,
private drain, and active-consumer count on both sides of a fence, so it cannot
declare closure in the middle of that ownership handoff.

Embedded SSB nodes pin their owning `TGState` from publication through recycle.
A detached/dead TG is reclaimed only after its published-node reference count
reaches zero. Detach has a terminal flush which does not allocate a replacement
buffer, and terminal state shutdown explicitly discards published, private, and
owner-active SSB references only after threading children and GC2 workers have
stopped.

The GC2 worker token still serializes the global grey-deque owner. The separate
consumer counter is not a lock and is not used to wait; it makes an otherwise
invisible detached interval part of exact completion detection. Bounded callers
return and retry when a peer owns progress.

Idle-generational entries carry an exact remembered-suffix count in the SSB
node header. The dynamic-allocation flag remains in bit zero of `pad`; the
suffix count occupies the upper bits. A transition flushes the old active node
before active-cycle publication can use its replacement, while an abort can
only leave non-remembered entries as a prefix. Because consumers pop from the
end, each bounded pop can distinguish and account a remembered entry without a
pointer tag, per-slot side allocation, synchronous mark-start drain, or scan of
the remaining chain. Major/terminal consumption clears the tag without
incrementing minor-start telemetry.

## Weak mark closure

Weak clearing is now preceded by a serialized GC2 mark-closure protocol:

1. the weak closer owns the worker token;
2. it drains already visible mark work within the supplied budget;
3. exactly one root/SSB snapshot is taken and recorded by the tri-state
   `weak_root_scanned` latch;
4. later bounded calls finish the work created by that snapshot without
   repeatedly manufacturing new root rescans;
5. closure requires empty published/private/active SSB state, empty grey work,
   fresh owner stack snapshots, no NEEDSCAN/table-rescan work, no weak writer or
   clear peer, and a stable marks-this-round observation;
6. finalizer publication is drained through the same owner path before
   `weak_mark_closed` is published;
7. the WEAK-to-SWEEP transition revalidates the closed frontier and reopens it
   if any producer has invalidated the snapshot.

Weak snapshot clearing remains cursor-bounded. Snapshot overflow and legacy
bridge gaps stay GC2-owned: overflow nodes or bridge tables are traced and
cleared under the serialized owner rather than falling back to the removed old
collector.

## Scheduling contract

Completion routines are nonwaiting attempts. A call may return incomplete when
a worker, mutator assist, SSB consumer, weak writer, or weak clearer owns work;
the scheduler wakes/retries later. Tests therefore assert the eventual bounded
retry contract rather than assuming one call waits for a peer and completes the
entire graph.

This checkpoint does not finish the owner/global root split. The current global
root pass still duplicates some TG-owned root scanning, and active stacks must
ultimately be read only by their owning safepoint participant. That is a
separate P0 slice, not evidence that weak closure is ready for release.
