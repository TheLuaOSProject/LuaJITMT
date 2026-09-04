# JIT and FFI review against the full lockless goal

Date: 2026-09-04

Reviewed source baseline: `a649f737d9841e1bf17f9102fb526d6bfb6c29e3`.

This is a source review, not a release or completion claim. The review inspected
the current production call paths, the relevant test assertions, `plan/08`,
`plan/11`, and the recent JIT/FFI notes. No builds, test executables, benchmarks,
Wine runs, or Darling runs were executed during this review. A cited test below
means its source contains the stated check; it does not mean this review reran
or passed it. Function names are the durable references; line numbers refer to
the reviewed baseline and may move.

The subsequent isolated-build measurements are recorded in the separate
[performance review](runtime-performance-review-2026-09-04.md), including its
complete JIT pilot and deliberately incomplete interpreter diagnostic.

The user goal remains fully nonblocking and thread-safe ordinary GC, JIT, VM,
and FFI operations, with `ffi.cdef` the explicitly named FFI exception,
ordinary Lua/LuaJIT behavior, and performance near or better than stock on x64
Linux, macOS, and Windows. Parking a TG in native state can help GC observe it,
but does not make an operation lock-free if completion requires a suspended
peer to resume. Bounded inner retries also do not establish progress when the
public caller retries the same owner-dependent state indefinitely.

## Ranked findings

### 1. P1: the global CType parser sequence blocks ordinary FFI

`lj_ctype_snapshot()` (`src/lj_ctype.c:394`) returns RETRY while the shared
`parse_token` is odd. General layout/name/size snapshot functions use the same
sequence rule. Their ordinary wrappers, including `lj_ctype_info_wait()`
(`:2001`), `lj_ctype_getname_wait()` (`:1685`), and `lj_ctype_size_wait()`
(`:2464`), retry until the writer publishes an even sequence.
`lj_ctype_parse_wait()` (`:327`) parks on that token through the platform wait
helper. This is an exact dependency on the parser owner, including when the
requested type predates the currently active, unrelated `ffi.cdef`.

Production callers include:

- `ccall_ctype_snapshot_wait()` in `src/lj_ccall.c:85`, on ordinary FFI calls;
- `callback_ctype_snapshot_wait()` in `src/lj_ccallback.c:1000`, on callback
  type/argument/result processing;
- cdata field, element-size and pointer operations in `src/lj_cdata.c`; and
- library symbol layout/name lookup in `src/lj_clib.c`.

The predefined and shallow-container fast paths are useful, but only remove
selected cases. In addition, `ffi_checkctype()` (`src/lib_ffi.c:892`) falls back
to acquiring `lj_ctype_parse_lock()` at `:910` for full abstract type-string
grammar. This makes some `ffi.new`/`typeof` operations parser-token writers
outside the allowed `ffi.cdef` boundary.

Removing the reader checks alone is unsafe. `cp_rollback_log()`,
`cp_ctype_snapshot_mut()`, and `cp_ctype_publish()` in
`src/lj_cparse.c:407-444` mutate previously published IDs and restore them after
parse failure; struct layout completion uses that mechanism too. The original
plan's simple "immutable once published" CType premise is not implemented by
these rollback transactions.

Required repair: private parser transactions and immutable committed CType
versions, with explicit handling of stable IDs, forward declarations, completion,
named-type lookup, interning, and rollback. Ordinary readers need a committed
view without waiting for the parser's private transaction. Full non-cdef type
construction must also have a nonwaiting publication contract; optimizing only
the common spelling of a type does not cover the requested semantics.

Required evidence: pause a parser while readers of already committed structs,
function types, fields, casts, callbacks, and type strings run to completion.
Include failed definitions and forward-declaration completion. A transient
contention result must not become a false missing-type error.

### 2. P1: foreign calls of the same callback serialize on one carrier

`callback_auto_attach()` (`src/lj_ccallback.c:876`) loads the callback slot's
hidden `lua_State *` carrier and invokes `lj_threading_attach_wait()`.
`threading_attach()` (`src/lib_threading.c:2366`) repeatedly claims that same
state, waiting on its owner at `:2413-2428`. The wrapper at `:2473` explicitly
documents serialization on the owner carrier.

Consequently, two TLS-less foreign OS threads invoking the same callback cannot
make independent progress: if the first callback is paused after obtaining the
carrier, the second waits for it. Native-state publication of the provisional
TG keeps some handshakes moving, but does not remove this callback dependency.

Required repair: independently admitted per-actor or per-invocation carriers,
with exact callback-slot/function/universe lifetime admission. The Lua closure
can remain shared; the execution stack need not be the same carrier. Preserve
the existing callback exception, errno/LastError, native-frame and detach
contracts.

Required evidence: two foreign threads call the same callback pointer, one is
paused inside Lua, and the other completes. Extend through nested callbacks,
normal return, exception unwind, slot lifetime, GC and shutdown. The current
auto-attach/runtime tests are useful behavioral coverage, but their existence
does not prove this paused-owner condition.

### 3. P1: FINREG sentinels remain owner-dependent across keys

`lj_cdata_setfin()` loops on RETRY or unresolved slot claims
(`src/lj_cdata.c:607-714`), calling `cdata_fin_claim_wait()` (`:266`). This
affects ordinary `ffi.gc(cd, finalizer)` and `ffi.gc(cd, nil)`.

The broader bottleneck is `lj_ctype_fin_newgen()` (`src/lj_ctype.c:912`). Before
publishing a new generation it calls `ctype_fin_has_claim_smr()` (`:867`),
which traverses all generations and all hash slots looking for any FINREG
claim sentinel. A found claim makes publication retry at `:989-993`. Thus a
paused replacement of one registration can block a new registration for an
unrelated cdata object when the current generation is full. The 256-attempt
inner bound ends in an error/retry outcome that the outer setfin loop retries;
it is not a bound on the public operation.

The current test source makes this limitation explicit:
`tests/t-ffi-finreg-clear-races.c:241` states that a clear can only retry while
replacement owns FINCLAIM. It observes that retry, then releases the paused
replacement before expecting the clear to finish. That is useful ordering and
accounting evidence, not nonblocking progress evidence.

Required repair: persistent, helpable registration transactions carrying the
key, finalizer, order identity, and terminal/accounting state. The registry's
key uniqueness and generation publication must compose with those transactions
without requiring every earlier claim owner to resume. Preserve exactly-once
finalization and the required registration order.

Required evidence: same-key replacement/clear and unrelated-key generation
publication complete with the first owner still paused, including collector
claim races, rollback, and delayed helpers. Do not replace the dependency with
spurious public errors after a fixed retry count.

### 4. P1: ordinary trace publication and automatic flushing still wait

`trace_stop()` calls the looping `lj_gc2_smr_read_enter()` for side/stitch
publication at `src/lj_trace.c:5815`, `:5827`, `:5878`, and `:5899`.
The admission helper (`src/lj_gc2.c:6965`) is an unbounded loop around
`lj_gc2_smr_read_try()` and retry-yield. A paused exclusive metadata reclaimer
therefore blocks this ordinary recording caller.

Trace-number exhaustion also reaches a synchronous flush from ordinary
recording: after releasing terminal recorder ownership,
`trace_state()` calls `lj_trace_flushall_hs()` at `src/lj_trace.c:6160-6162`.
`trace_flushall_hs_impl()` (`:5209`) acquires the token with
`lj_jit_token_acquire_wait()` (`:5233`) and waits for
`lj_gc2_handshake(EXIT_TRACES|FLUSHJ)` (`:5239`). Scoped public flushes also use
the wait helper in `lj_trace_flushscope()`/`lj_trace_flushscope_hs()`.

The central token wait (`src/lj_trace.c:2306`) repeatedly requests an abort and
tries again. A request does not let a suspended recorder relinquish ownership.

Required repair: distinguish speculative recording from committed publication.
Before irreversible publication, failed bounded admission should abort the
optional recording and resume interpretation. After publication, completion
and retirement need persistent exact descriptors. Automatic pressure flushes
must publish durable work and let the caller continue. Public invalidation
requires a defined logical linearization point independent of waiting for
physical reclamation.

This review does not assert a reproduced token/SMR deadlock. The inspected
current GC reclaim paths use opportunistic `lj_jit_token_try()`
(`src/lj_gc2.c:5777`, `:7138`), so an older note about a token/reclaimer cycle
is not enough to claim that exact cycle still exists. The blocking admission
and suspended-owner dependencies above are directly established by source.

### 5. P1: START/STOP/ABORT/RECORD callbacks still retain recorder ownership

Production FLUSH has the detached event transaction, but these producers still
use the legacy `lj_vmevent_send_l` path:

- START: `src/lj_trace.c:5741`;
- STOP: `src/lj_trace.c:5934`;
- ABORT: `src/lj_trace.c:6043`, plus the lost-root-publication abort; and
- RECORD: `src/lj_trace.c:6184`.

`lj_vmevent_call()` saves token-owned `J->L` at `src/lj_vmevent.c:1114`, calls
arbitrary Lua through `lj_vm_pcall_unwind()`, then restores it while retaining
ownership. A paused callback monopolizes the recorder. Other mutators can keep
interpreting, but synchronous JIT controls/flushes still wait for this owner.

Required repair: retain the detached FLUSH session/stream foundation. Extend
STOP/ABORT using immutable terminal trace evidence, and START/RECORD using exact
continuation ownership, resume and terminal pairing. Do not erase the event
semantics expected by `jit.dump` merely to avoid the ownership work.

Required evidence: paused handlers with peer JIT controls, recording,
interpretation, GC, nested controls, handler errors, and exact trace visibility
through the callback. Treat the FLUSH-only source/tests as FLUSH-only evidence.

### 6. P1: Windows excludes the general x64 table-store JIT bridge

`LJ_HAS_X64_MT_JIT_HELPERS` is enabled only by
`LJ_TARGET_X64 && (defined(__linux__) || LJ_TARGET_OSX)` in
`src/lj_record.c:44`. Windows x64 therefore selects the disabled branch. In
`lj_record_idx()` at `:2579-2586`, indexed stores reaching the general raw
hash/array recording path raise `LJ_TRERR_NYIBC` for both `IR_HLOAD` and
`IR_ALOAD`. There is no active-MT condition on this rejection, so ordinary
pre-MT table-store traces also hit it. Earlier helper-backed active-MT store
branches must be assessed separately; this is not a claim that no store shape
can ever trace on Windows.

This is a broader supported-platform gap than the missing Win64 aggregate FFI
classes. Linux table-store/JIT results cannot establish Windows parity while
this target gate remains. Required repair: complete and validate the Windows
x64 helper/assembler calling convention, then enable the appropriate paths.
Required evidence includes pre-MT and active-MT hash/array stores, old-nil and
existing values, helper fallback, resize, GC, and value-type transitions.

The platform reviewer reported investigating the corresponding Wine fixture
failure at this revision. This review independently verified the source gate;
it did not execute Wine and does not use that report as a fresh runtime pass
or failure result.

### 7. P2: generic CALLXS is live, but ABI/caller coverage is partial

The production function is `crec_call()` (`src/lj_crecord.c:2498`), using
`crec_call_args_collect()`, `crec_call_args_emit()`, XSAVE, exact native entry,
`IR_CALLXS`, and native leave. The native lifecycle implementation is in
`lj_ffi_native_trace_enter()`/`lj_ffi_native_trace_leave()`
(`src/lj_ccall.c:2222`, `:2323`), with callback suspend/resume/unwind nearby.
`ffrecord_postcall_snap()` (`src/lj_ffrecord.c:1571`) supplies the matching Lua
caller snapshot after the physical foreign call. There is no basis for the
older claim that production generic CALLXS remains blanket-disabled.

The following is source admission and test-source coverage, not a claim of
fresh runtime verification on any platform:

| Area | Current production boundary | Relevant test source |
| --- | --- | --- |
| Scalar calls | Generic numeric/pointer/enum arguments; scalar/void results; narrow integer and float conversion; varargs use normal CType inference and call ABI metadata. | `tests/t-ffi-callxs-authentic.lua` covers mixed integer/FP/pointer arguments, narrow signed/unsigned results, float/double, varargs, void stores and errno. `tests/t-ffi-callxs-production.lua` requires XSAVE and CALLXS for a normal `ffi.C.abs` loop. |
| Rooted scalar results | Pointer/reference, int64/uint64 and enum results use preallocated exact-CType boxes. Dynamic booleans preserve a real Lua boolean across post-call exits. | Authentic test requires CALLXS for pointer/reference, enum, int64/uint64, attributed type cases, and booleans; it checks boolean mismatch effect counts. |
| Indirect aggregate results | Fixed struct/union results are admitted when unconditionally indirect: SysV size >16; Win64 size other than 1/2/4/8. Declared result ID and alignment are retained. | Authentic test contains 24-byte struct/union results, over-aligned result storage, zero-argument and register-pressure cases, and exact per-function effect counts. |
| SysV direct aggregate arguments/results | `crec_call_sysv_aggregate_type()` (`src/lj_crecord.c:2254`) accepts exact aggregate cdata with size 1/2/4/8 and one recursive INTEGER or SSE eightbyte. Exact aggregate reference arguments are accepted. | `tests/t-ffi-callxs-sysv-small-aggregate.lua` requires XSAVE/CALLXS and exact effect counts for narrow INTEGER, SSE, nested struct/array, union merge, and whole-argument stack fallback after GPR/XMM exhaustion. It explicitly skips Windows. |
| Caller/result modes | Admission requires `J->framedepth > 0`, a Lua frame and saved CALL/CALLM/ITERC PC (`src/lj_crecord.c:2532`). Inlined tailcalls can satisfy this because they retain the outer call PC. The post-call helper also requires an ordinary Lua caller and no terminal trace link. | Authentic test explicitly covers CALLM, inlined CALLT/CALLMT wrappers, ITERC, ignored results, excess fixed results and open results, including boxed/aggregate/boolean cases. These tests must not be mistaken for arbitrary root-tailcall coverage. |
| Physical-call lifetime | Exact trace pin and native frame span the foreign call and any post-call exit; callback and remote flush fixtures exercise related lifecycle paths. | `tests/t-ffi-callxs-postcall.c`, `tests/t-ffi-callxs-callback.c`, and `tests/t-ffi-callxs-remote-flush.lua`, registered by `m7_ffi_callxs_authentic`. |

Incomplete admission remains explicit in source:

- SysV two-eightbyte (9-16 byte), mixed/two-register result classes;
- odd-width direct aggregates, and MEMORY-class aggregate arguments or small
  MEMORY-class results, including unaligned fields;
- implicit table/string conversion to by-value aggregate arguments;
- Win64 by-value aggregate arguments and direct scalar-sized aggregate results;
- complex/vector shapes not represented by the admitted scalar or one-class
  aggregate path;
- protected/continuation/non-Lua immediate callers, direct-vararg frames,
  root-tail and terminal-return topologies beyond the existing caller contract;
  and
- non-x64 lowering, which is outside the currently requested architecture.

The generic path is the correct foundation. Complete aggregate grouping,
register rollback and multiple result registers in the ABI lowering, then the
missing caller contracts. Flattening a two-component SysV aggregate into
unrelated scalar arguments would lose its all-register-or-stack rule. Do not
restore the removed C-symbol/signature matcher or disable all CALLXS as a
substitute for completing the missing contracts.

The prior reported Win64 nested-callback unwind problem was not reproduced or
disproved by this source-only review. It remains a platform validation item,
not a fresh verified failure claim.

### 8. P2: current performance acceptance does not establish parity

`tests/suites/m9_m10_gc.lua:532` defaults the stock comparison ceiling to
`100.0`, mostly one sample. The default filters at `:526` omit FFI and active-MT
behavior. Missing stock LuaJIT skips this comparison rather than failing it.
This is explicitly a catastrophic-cliff guard, not the plan's <=1.10 JIT
geomean target.

The July release note reports 9.45x new-key insertion and 8.39x closure/upvalue
slowdowns. Those are dated historical measurements; this review did not
establish their current ratios. A passing current 100x check would not show
that these regressions are fixed.

There are also deliberate JIT fallback boundaries worth measuring separately:
for example active-MT shared `next()` aborts recording in `recff_next()`
(`src/lj_ffrecord.c:635`). Correct interpreter fallback preserves operation
semantics but is not performance evidence for all ordinary Lua idioms.

Required evidence: reproducible repeated comparison against pinned stock,
including pre-activation, sticky-MT with one remaining mutator, concurrent
readers/writers, GC churn, and admitted/rejected FFI ABI/caller shapes. Track
per-workload ratios and MT throughput/progress floors. A tighter interim 3x
ceiling can be a development gate, but does not replace the final parity goal.

## Plan corrections and work to preserve

At the reviewed baseline, `plan/11_ffi_concurrency.md` section 11.5 says generic CALLXS is disabled;
the current source and tests contradict that. Section 11.2's immutable CType
design and its later rollback-reader bridge need a single explicit final
publication model. The callback section needs independent carrier admission.
FINREG's owner-only claims are a production design blocker, not merely cleanup.
The original Linux-only scope in the plan entry is also superseded by the
user's Linux/macOS/Windows x64 requirement.

Keep the useful foundations: immutable published mcode with dual mappings,
writable exit targets, TG-local execution state, opportunistic recorder
acquisition, exact native trace frames and no-replay snapshots, rooted bounded
readers, and detached FLUSH sessions. Correct the remaining ownership protocols
instead of replacing these working abstractions wholesale.

A completion roadmap should tie each production operation to its publication,
lifetime, progress and performance proof. Source-count inventories, many small
passing helper tests, and dated percentage estimates are not substitutes for
public operations completing with owners paused. The final gate must include
the supported platform/ABI matrix and retain the full user scope.
