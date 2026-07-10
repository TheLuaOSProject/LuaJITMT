# GC2 current C-frame stack top, 2026-07-10

The GC2-only collector transition temporarily widened a current VM thread with
any `cframe` to `maxstack` in both GC2 stack scanners and the companion color
root traversal. That treats stale values above the active Lua/C call window as
semantic roots.

The broad bound was found while investigating a bulk FINREG shortfall. That
shortfall ultimately had a separate cause: a pre-CTState cdata strong edge was
discarded and its freed address was reused by a later finalizable object. It is
documented in `gc2-rooted-pre-ctstate-cdata-2026-07-10.md`; it is not evidence
for the stack-bound change.

Same-thread collection already has `gc2_active_thread_top()` /
`gc_active_thread_top()`. These combine authoritative `L->top`, validated frame
boundaries, the active cframe PC, fixed call operands, multi-result rules, and
live debug local ranges. Open local cells and frame functions are scanned
separately. The current-thread path now uses that bounded top again without
merging the declared full-frame `used` bound back into it. A bytecode-derived
call window may widen `L->top`, but never shrink below it: a running C API
function can push live values before invoking `lua_gc()`. Remote, native, and
JIT-owned threads still conservatively scan `maxstack` until their owner
publishes an authoritative boundary, preserving the concurrency safety rule.

The companion whole-`maxstack` search for every function-tagged slot was also
removed. It kept popped closures and prototypes alive even on an authoritative
same-thread scan (the arena resweep fixture exposed this with a stale dumped
closure above `L->top`). Validated frame-chain walkers already mark each live
frame function separately, including remote/native/JIT paths, so arbitrary
function-looking payload above the active boundary is neither necessary nor a
valid semantic root.

Arena sweep's last-chance stack-name validation was removed entirely. Its
GC2-only implementation searched every stack once for every unmarked arena
cell: `O(dead cells * aggregate stack capacity)`. Besides the cost, it made
sweep silently repair an incomplete root snapshot and therefore obscured root
scanner bugs. Mark/root closure is now the sole authority before sweep, as it is
for other object edges. The deterministic pre-CTState cdata fix makes the full
traversal fixture pass without this fallback.

A Lua-to-C callback regression pushes a table and its only child, forces a full
GC, and verifies both remain live and connected. This protects the important
`L->top` rule for values pushed by running C API code. Existing C-call/root
fixtures continue to pin preservation of live helper operands, while popped
stale slots no longer influence collection through a sweep back door.
