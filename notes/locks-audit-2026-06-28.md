# Lock audit - 2026-06-28

Scope: current `v2.1` x86_64/Linux lockless LuaJIT fork, outside direct
`ffi.cdef()` mutation itself.

## Short answer

There is no broad production VM mutex, `pthread_mutex`, rwlock, or `lua_lock`
wrapper. Remaining synchronization is mostly CAS owner tokens, sentinel values,
and Linux futex/native parking. Some of it is intentionally user-visible
blocking behavior; some is temporary bridge scaffolding.

## Locks or lock-like points that should stay

- Public `threading.mutex`: user-facing synchronization API in
  `src/lib_threading.c`. It uses CAS plus futex wait/wake and must keep normal
  mutex semantics.
- Channels, joins, and sleeps: blocking API semantics in `src/lj_chan.c` and
  `src/lib_threading.c`. The implementation parks in native time rather than
  spinning.
- Per-`lua_State` owner claims in `src/lj_thr.c`: prevent one thread mutating a
  stack while another thread or GC scans it.
- Safepoint leader tokens in `src/lj_safepoint.c`: serialize global handshake
  publication for stop requests, redispatch, GC, and trace flush.
- GC2 worker control tokens in `src/lj_gc2.c`: rare lifecycle ownership for
  starting, stopping, and parking the collector worker.
- JIT recorder token in `src/lj_trace.c`: nonblocking CAS token. Losers skip or
  abort recording instead of waiting, so this is already the right shape.
- Table KEYLOCK/FORWARD sentinels in `src/lj_tab.c`: publication state for
  concurrent hash insertion/resize. These are not legacy locks; they are part
  of the lock-free table protocol.

## Remaining parser-token users outside cdef

`CTState.parse_token` still appears outside direct `ffi.cdef()` where readers
need to parse string type names, query mutable layout while rollback may be in
flight, or protect current field/namespace fallback paths.

Current examples:

- `src/lib_ffi.c`: string type parsing and layout fallbacks for `ffi.new`,
  `ffi.cast`, `ffi.sizeof`, `ffi.alignof`, and `ffi.offsetof`.
- `src/lj_clib.c`: `ffi.C` namespace lookup fallback.
- `src/lj_crecord.c`: recorder-side parser-token ownership for string ctype
  recording, with busy paths aborting rather than blocking.

This slice removed parser-lock fallback from interpreted numeric cdata
element-size readers and pointer arithmetic. They now use `lj_ctype_size_wait()`
and refetch any `CType *` state after a native wait.

Follow-up in the same run removed the parser-lock fallback from interpreted
cdata string-key field lookup. Struct fields, constructor constants, and pointer
auto-deref now wait/retry through ID-rooted helpers and return copied field
snapshots instead of table-owned `CType *` values acquired before a wait.

## Worth making more lockless

1. Continue FFI read fallback cleanup. Best return on risk: non-mutating readers
   can use sequence-checked snapshots and wait/retry helpers, as long as callers
   only retain scalar IDs/sizes across waits and refetch `CType *` afterward.
2. Reduce remaining layout fallback locks in `lib_ffi.c` where a stable
   snapshot can preserve normal errors and size/alignment semantics.
3. Collapse bridge source-text checks into behavior tests when behavior can observe
   the invariant. Architecture, visibility, and memory-ordering boundaries that
   behavior cannot reliably prove belong in comments or notes, not CI source
   predicates.

## Not worth forcing now

- Mutable `ffi.cdef()` serialization. The type graph is still a mutable parser
  transaction with rollback, abandoned entries, and interning side effects.
- Public mutex/channel/join semantics.
- Explicit legacy-GC exclusion and GC2/legacy weak/finalizer bridge points until
  GC2 fully owns liveness, weak clearing, finalizer ordering, and sweep close.
- GDBJIT descriptor locking. It is low-frequency debugger metadata and not a
  meaningful runtime bottleneck.

## Hazards

- Never keep a `CType *`, `CTypeTab *`, hash node pointer, or table array/node
  pointer across a native wait unless the object is explicitly pinned by the
  relevant SMR/epoch protocol.
- Parser wait helpers should take stable IDs and return scalar results. Callers
  needing structural data must refetch by ID after the wait.
- Removing "legacy" names blindly is unsafe. Some are real current bridge
  semantics; remove them only after the bridge behavior has moved to GC2 or a
  current lock-free owner.
