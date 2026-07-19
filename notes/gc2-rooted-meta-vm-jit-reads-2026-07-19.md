# Rooted metamethod, VM and JIT point-read cutover

Date: 2026-07-19

This b1.2.1 checkpoint extends the authoritative-root table-read protocol into
metamethod chains, public C API operations, live x64 VM reads and active-MT JIT
recording/generated code. It does not modify `plan/`, restore the retired
collector, or claim that structural table operations and ordinary waits are
complete.

## Metamethod chains

`lj_meta_tgettv_rooted()` gives non-VM callers an explicit enumerated output
root. Receiver, key, intermediate result and metamethod are retained in four
private TG anchors. Source SMR begins before the receiver/key loads, exact
leases cover both snapshots, and every returned GC value is release-published
to the caller's stack/TG root before its lease is released. Table-valued
`__index` and `__newindex` chains therefore carry no raw slot or naked table
body across resize, collection, stack growth or helper retry.

Function metamethod handoff publishes the function, receiver and key in the VM
continuation frame before the private roots are dropped. The set path keeps its
final owner/key roots through the common generation-aware keyed CAS and write
barriers. Deterministic type, nil/NaN-key and chain-loop errors explicitly pop
all private roots before throwing, including errors caught immediately by Lua
`pcall()`.

The compatibility `lj_meta_tget()` remains VM-only and uses the saved bytecode
destination. Host C callers no longer infer an output register from `vmstate`,
which had incorrectly classified a fresh host state as an interpreter frame.

## Public API and FFI dispatch

`lua_gettable()`, `lua_getfield()`, `lua_settable()` and `lua_setfield()` use
the rooted metamethod protocol. Parent/key/value stack shapes preserve negative,
positive, pseudo-index and table/self-alias semantics. `lua_rawget()` and
`lua_rawgeti()` first transfer a replaceable source edge directly into an
enumerated stack slot under source SMR plus an exact lease, then use the rooted
point reader; no TG anchor is left live across a catchable API error.

FFI table-valued `__index` and `__newindex` transfer the rooted metatype value
into the existing Lua argument slot, release-publish that natural stack root,
and only then drop the private CType root. The generic rooted metamethod helper
therefore survives stack relocation without keeping a private CType anchor
across a catchable semantic error. `__newindex` continues through the common
guarded table transaction instead of an explicit C-call/FFI-shape path.

Mutable C-function/global environment edges and CType metatype edges require
the same rule: the authoritative source must be admitted and transferred to an
enumerated root before any anchor allocation, hook or safepoint. Copying one of
these edges into an unenumerated C/TG scratch value first is not sufficient,
because replacement plus GC can reclaim and reuse its allocation before the
later rooted helper starts.

## x64 VM and generated traces

The x64 Lua `rawget` fast function and `BC_TGETR` use
`lj_tab_gettv_rooted()` with actual stack roots. The previous inline array/hash
loads are removed for the active runtime path, including the valid table/result
self-alias form. The helper ABI saves/restores the VM base and PC and covers
SysV and Win64 argument materialization.

`BC_GGET` and `BC_GSET` likewise pass the authoritative current-function
TValue at `BASE-16` to environment-aware rooted meta helpers. Those helpers
create the chain anchors first, open source SMR, lease the exact function,
load and lease its current environment child, and publish that child into the
receiver anchor before releasing either lease. The VM no longer copies the
mutable `fn->env` edge into unenumerated TG scratch. Both global bytecodes take
this path even before the sticky MT latch is set: a sole-mutator fast-path
check can race the first secondary entrant, so it cannot by itself retain an
old environment through the subsequent table helper or barrier. Recovering a
fast global path requires an equally rooted inline admission protocol, not a
pre-activation pointer shortcut.

The active-MT recorder samples table values from dedicated enumerated anchors,
then emits `TMPREF` parent/result and key inputs for the same rooted helper.
Generated IR performs the helper call before its typed `VLOAD` snapshot and
contains no raw `ALOAD`/`HLOAD` for these shared reads. Mutable metatable and
function-environment captures fail closed until their own rooted generated
helpers exist. `TNEW`/`TDUP` also receive no locality exemption until explicit
escape tracking can prove that a table was not published earlier in a trace.

CType metamethod lookup now follows the same lifetime rule. It begins source
SMR before loading CType topology, exact-leases every CTState/table/result edge,
sequence-checks parser publication, and publishes into a caller-owned root
before releasing its lease. Table-valued cdata `__index` recording currently
fails closed instead of embedding a mutable table result. Function-pointer
lookup remains generic CType topology; no signature or ABI shape matcher has
returned.

## Evidence and performance

Focused coverage includes source/result/key aliasing, forced admission retry,
physical stack relocation during table retry, host C API calls without a VM
cframe, pseudo-index environment replacement, immediate full collection in
function metamethods, repeated caught semantic errors, real `TGETR` alias bytecode,
global reads and writes through table- and function-valued metamethods with a
full collection during first-anchor creation, STOPREQ unwind, generated IR
inspection, concurrent C-API resize, and the full GC2 traversal fixture. The
traversal fixture now separates authentic trace
lowering from a deterministic direct helper barrier check: a `lua_call()` entry
checkpoint is allowed to complete a synthetic MARK cycle before executing the
store, so a post-call mark-bit assertion was not a valid barrier test.

The focused x64 point-read microbenchmark was roughly 4x slower than a direct
indexed load and the table-valued metamethod microbenchmark was roughly 9%
slower than its previous path. These are correctness-first measurements, far
below the temporary 100x cliff but not the final parity target.

Strict GCC and Clang assertion/helper builds pass with `-Wall -Werror`. The
focused rooted API/meta/x64/JIT/FFI suites, GC2 traversal fixture and both
normal and amalgamated no-retired-runtime gates pass. The Windows UCRT x64
cross-build and Wine global-read/write smoke pass as well.

## Remaining work

- Length, `NEXT`/iteration, range shifts, new-key collision publication,
  resize migration and `table.clear` need generation-bound readers and durable
  structural/range descriptors.
- The rooted helpers still reach `lj_tab_wait_l()` after releasing all leases
  and SMR scopes. The zero-wait tranche must replace those retries with bounded
  restart, helping or conservative defer and prove the ordinary-path allowlist.
- An asynchronous STOPREQ/OOM/table-overflow throw from inside a dynamic meta
  anchor scope can be caught by a nested Lua fast `pcall()` without the outer C
  unwind wrapper seeing a non-OK status. A protected-frame anchor checkpoint or
  a no-throw/no-wait helper redesign is still required; deterministic semantic
  throws are already balanced.
- Active-MT traced globals, restoration of table-valued cdata `__index`
  recording, structural JIT reads, string/JIT token publication and the
  broader generic FFI ABI/caller topology remain b1.2.1 work.
- This point-operation cutover is not full public-API signoff. Comparisons,
  metatable/fenv helpers, `lua_next`, object length, upvalue identity/mutation,
  `lua_copy`, and conversion/pointer-return APIs still contain pseudo-index or
  shared-body lifetime cases which need exact leases/root carriers.

## Checkpoint estimate

With this rooted point-read and CType tranche, the core b1.2.1 scope is roughly
50-55% complete when the later Lua `atomic` module and temporarily omitted
custom-allocator generalization are excluded. Including those eventual
requirements gives roughly 40-45%. Release readiness is approximately 45-50%:
the remaining structural-table, zero-wait, generic-ABI, lifecycle, platform and
sanitizer tranches contain several release-critical correctness risks.
