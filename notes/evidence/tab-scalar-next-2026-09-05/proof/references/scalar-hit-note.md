# Scalar table hits without global SMR admission

This change lets an ordinary interpreted `payload.offset` read finish while an
unrelated real IDLE metadata reclaimer remains paused with `META_EXCLUSIVE`.
The new path accepts positive number/boolean results for numeric, boolean, and
small string keys, using small table and vector allocations. It runs before
metamethod-chain anchor allocation and publishes directly to the existing
output slot. All misses, GC results, unsupported keys, Huge allocations,
function-environment lookups, and custom-allocator compatibility cases retain
the existing general path. No general SMR gate was removed.

This is a bounded scope of nonblocking progress, not completion of the wider
ordinary-read or runtime objective. In particular, a suspended plain-arena
writer can still deny admission to another live vector in that arena. CAS
contention in the existing registry/reader-count primitives uses lock-free
retry; the entire call is not claimed to have a wait-free instruction bound.

## Authority and ordering

`lj_gc2_small_lease_try` uses only the persistent shared small-arena registry.
Its exact mapping-slot reader pins the mapping until arena `remote_active`
admission succeeds. Admission precedes the SC fence and lifetime/block/READY
checks; these preserve the existing reader/destructor no-both-miss handshake.
The helper never follows a TG ownership list, enters global SMR, marks an
allocation, publishes semantic work, or falls back to Huge/custom admission.
Typed admission accepts only exact table/string starts, rejecting cdata
coverage and mismatched types before callers use their payload. Plain vector
admission requires a plain arena and a live allocation start. Every successful
lease is released on every return path.

The returned span is deliberately described as a conservative **cell-span
bound**, not an exact requested allocation size. Small metadata records starts;
an allocation followed by a private unused bump tail may have no immediate end
sentinel. The bound stops at the next acquired block/mark boundary, arena end,
or small-allocation cap. The vector's immutable header remains its physical
size contract. The lookup first admits enough bytes for that header, confirms
the authoritative vector pointer, then rejects any header-described span that
crosses the acquired bound. Header-derived sizes never authorize the initial
header read. Colocated arrays use the table's retained span; `g->nilnode` and
the embedded empty string have permanent global ownership.

`lj_tab_getscalar_rooted_try` retains the table and requested small string,
confirms both original sources, captures both vector pointers, retains each
separate vector, and confirms both vector sources before header reads. It
checks valid sizes and retiring flags, copies a scalar from a bounded array
slot or collision chain, then confirms the paired vector generation, source
words, and exact state owner before publishing. Output may alias either input:
the terminal output store follows the last source read. A miss leaves every
input/output word unchanged. Successful stack output keeps the existing stack
dirty/publication protocol; a scalar carries no GC edge to mark.

## Source provenance and key/value identity

The actor/L owner snapshot is not a proof that arbitrary `cTValue *` arguments
are authoritative. The API requires source-cell lifetime from its caller:

- VM TGETV uses the owned frame's stack cells. TGETB uses scalar scratch.
  TGETS materializes a string in TMP1; that copy is backed by the active
  function/prototype's immutable constant root. It is not a newly established
  enumerated scratch root. The callback-free attempt retains that ownership
  and does not grow/relocate the stack or replace the active frame.
- C API gettable/getfield claims the state. Environment pseudo-indices first
  materialize an exact stack root through their existing retained source
  capture. Stack, registry, and closure-upvalue sources keep their existing
  stable-cell/retained-container contracts. The new helper cannot repair a C
  caller that already lost provenance by copying a replaceable edge.
- The function-environment VM variant deliberately bypasses this fast path;
  its mutable child-edge capture retains the existing general protocol.
- FFI metatype table indexing transfers the exact `LJCTypeMetaRoot` into the
  enumerated argument slot and publishes it before releasing the private root.
  `ffi_index_meta` then passes `base`, `base+1`, and `base`, preserving both
  source provenance and the supported output alias.

After body admission, the actual source cell must still match the captured
word. A same-address successor is usable only if that authoritative source
really publishes it; rechecking a private copied temporary would not suffice.
No GC result is returned here, avoiding the separate result-slot-to-body ABA
gap identified in the design review of a general no-SMR point read.

Collision keys other than the requested key are opaque words. The lookup never
calls `lj_tv_gcref_type_match`/`lj_obj_equal` on them. Only the requested retained
string can match a string key; numeric/boolean matching requires no object
header. These accepted keys are not weak collectable keys (LuaJIT treats
strings as strong even in weak-key tables).

Canonical shared key publication is irreversible within a hash generation;
shared delete and `table.clear` replace values, leaving keys in place. Resize
publishes RETIRING before forwarding and changes the paired roots. Claim
rollback clears unpublished KEYLOCK/nil claims, not a matched canonical key.
Private raw key clearing requires the single-mutator path and cannot overlap
this owner-only callback-free attempt. The matched key is nevertheless
confirmed after the value load to fail closed on malformed/reentrant changes.
These facts, rather than the old general resolver alone, justify combining a
matched accepted key with its scalar value.

## Validation

Frozen runtime base: `d680421c4cb50b85437d88255bc89358c5e3a6b1`. All validation
and timing used that commit's arena implementation, excluding the separately
developed empty-reclaimed optimization. Production blobs and exact arena blobs
are recorded in `bench/tab-scalar-hit-2026-09-05/source-snapshot.json`.

Durable functional evidence and independent source review are in
`notes/evidence/tab-scalar-hit-admission-2026-09-05/`; the original build trees
and binaries are under `/tmp/lj-tab-scalar-hit-20260905-mosheh9q`.

- Strict helper/assert build and the dedicated fixture passed: number/boolean
  results, integral/fractional/boolean/string keys, both output aliases,
  unchanged miss/unsupported output, unchanged marks/accounting, source
  replacement at four boundaries, actual resize and retired-vector drainage,
  clear/resurrection, malformed vector sizes, and inaccessible candidate/key/
  vector-body pages. The default build disables public custom allocators, so
  that case tests only the new helper's compatibility preflight; it does not
  claim custom-runtime support or validation.
- The fixture starts the real IDLE reclaimer thread, pauses it after native
  quiescence, and calls ordinary Lua for 50 scalar field reads. The sum is 850,
  the reclaimer remains paused, and no table wait is recorded until after the
  operation finishes. A separate real plain-arena writer pause returns a
  bounded refusal without changing output; the same read succeeds after that
  writer releases. This explicitly retains the local progress limitation.
- The negative tree removes only the new meta fast-path call, restoring the
  exact old source-SMR route. The same paused-only fixture reaches SIGALRM at
  five seconds. `negative-stack.stdout` shows
  `meta_chain_capture_inputs -> lj_tab_wait_l -> lj_thr_retry_yield`, while
  the second thread remains paused in `gc2_idle_reclaim_enter`.
- Clang ASan passed the dedicated fixture and rooted get/length/reader C
  fixtures. Build/generator execution used `ASAN_OPTIONS=detect_leaks=0`;
  runtime executions used `detect_leaks=1:abort_on_error=1`. No runtime leak
  suppression or sanitizer suppression was used.
- Normal stock suites passed **387 interpreter / 509 JIT** tests.
- Ten canonical M5 cases passed: scalar hit, rooted get/length/reader, rooted
  reader Lua stress, meta chain, x64 rooted reads, clear-entering, forward
  filtering, and stable chain ordering. The canonical fixture is Linux-only
  because it uses pthreads, protected mappings, and alarm-based diagnostics;
  it is also registered in the M5 aggregate.

The final additional key and retired-queue assertions were rerun in helper/
assert and ASan builds after canonical registration had passed. Source hashes
for all runtime builds remained unchanged.

## Normal interpreter measurements

`bench/tab-scalar-hit-2026-09-05/` contains the Lua workload, every command and
raw result, binary hashes, and summary. Seven alternating fresh-process pairs
per case ran on CPU 30, with 100,000 reads and `-joff`. Both binaries use the
same normal static O2 build without helper/assert flags; the control removes
only the meta fast-path call. GC remains enabled, results are checked, and a
full collection completes after the timed loop. Other agents could perform
functional work on CPUs 0–15; the machine was not otherwise isolated.

| Timed loop | Before median | After median | After / before |
| --- | ---: | ---: | ---: |
| Numeric string field | 35.858 ms | 19.740 ms | 0.551 |
| Numeric array slot | 23.756 ms | 15.396 ms | 0.648 |
| Boolean string field | 35.861 ms | 20.073 ms | 0.560 |

These small-hit measurements establish the cost of this path under ordinary
interpreter execution. They do not establish application speedup, Huge or
metamethod performance, allocator progress, or general lockless completion.
