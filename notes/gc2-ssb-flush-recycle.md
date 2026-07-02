# GC2 SSB flush recycle

Mutator SSB flush can need a fresh SSB node while the current TG has no free
node available. The flush path may recycle already-published SSB nodes only by
claiming the GC2 worker owner gate and converting published SSB items to grey
work. It must not directly drain arbitrary grey work from that writer-side
overflow path, because the grey deque has a single logical owner at a time.

This is an internal GC2/threading rule. It does not change stock LuaJIT API
behavior, and it is covered by runtime GC2 traversal and worker-scheduler
fixtures rather than by source-text tests.
