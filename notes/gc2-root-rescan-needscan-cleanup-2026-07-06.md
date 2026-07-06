# GC2 root rescan and NEEDSCAN cleanup

Stability pass for weak/finalizer and thread-root coverage.

Key fixes:

- Arena bump allocation now publishes function/upvalue cells only after the
  corresponding object header/body is initialized. Bitmap sweep can observe the
  allocation bit before the object reaches the legacy root spine, so setting the
  bit first allowed marker/sweeper code to sample uninitialized closure/upvalue
  bodies.
- Legacy and GC2 root scans now treat local-cell upvalues, thread roots, and
  userdata roots as mutable containers. If the container object is already
  nonwhite/marked, a fresh semantic root hit still rescans its current payload
  so userdata metatables, thread stacks, upvalue payloads, and table/proto
  children are not skipped.
- Forced or preserve-abort transitions to IDLE clear cycle-local thread
  `LJ_GC_NEEDSCAN` handoffs and abandon grey work from that aborted cycle after
  the idle barrier. Normal sweep close does not discard this state; assert builds
  now check that no NEEDSCAN handoff leaks into normal sweep close.
- The GC2 traverse fixture now asserts that its SSB/grey frontier helper also
  has no pending thread NEEDSCAN work. Its synthetic TG checks assert owner
  lookup/dead publication instead of relying on an exact global live-TG count
  that can move while helper worker TGs detach.

Verification:

- `tools/test.lua m8_weak`
- `tools/test.lua m3_vm_safepoint`
- `tools/test.lua m3_gc2_paranoia`
- Standalone `t-gc2-traverse` with `LUA_USE_ASSERT` and `LJ_GC2_PARANOIA=1`

