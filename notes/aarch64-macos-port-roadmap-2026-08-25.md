# AArch64 macOS lockless port roadmap (2026-08-25)

## Goal and current claim

This branch ports the existing LuaJIT-MT x86-64 implementation to native
AArch64 macOS.  The target is behavioral and lock-freedom parity with the
current x86-64 branch for the VM, GC2, threading, JIT, and FFI.  It does not
silently upgrade the pre-existing x86-64 implementation's maturity claim: the
current b1.2.1 notes still identify unfinished lockless/runtime work.  A port
checkpoint is complete only when the AArch64 path has positive runtime and
artifact evidence; compiling after deleting architecture guards is not a port.

Development branch: `codex/aarch64-macos-port`.

Native audit host:

- Darwin 25.5.0 / macOS 26.5.1, arm64
- Apple clang 21.0.0
- 16 KiB pages
- AArch64 LSE is present on the audit machine

## Baseline

`env MACOSX_DEPLOYMENT_TARGET=13.0 make` fails before a target VM is produced.
The first failures are:

1. `src/lj_arch.h`: the lockless build requires GC64 and x86-64.
2. `src/lj_gc2token.h` and `src/lj_tgslot.h`: exact 128-bit authorities are
   restricted to the x86-64 CX16 contract.  Equivalent guards in
   `src/lj_markword.h` and `src/lj_universe.h` are reached later.
3. `src/vm_arm64.dasc`: the stock ARM64 VM refers to removed layouts:
   `jit_State.trace`, `CTState.L`, and the old shared `CCallback` argument
   arrays/slot/stack.

The generated ARM64 VM currently reports 19 compile errors at this boundary.
No test suite can start because every suite first builds the VM.

The size of the missing architecture work is visible in the history since the
pinned pre-MT base: `vm_x64.dasc` changed by roughly +2345/-482 lines and
`lj_asm_x86.h` by +2338/-856, while `vm_arm64.dasc` changed only by comment
cleanup and the ARM64 assembler/emitter received no matching MT port.

## Non-negotiable invariants

The ARM64 implementation must preserve the existing runtime protocols, not
reintroduce a per-universe VM lock or hide a lock in an atomic helper:

- exact 128-bit authorities for GC2 activation, arena markwords, universe
  admission, TG lifecycle, table descriptors, and arena registry entries;
- TG-local dispatch, hotcounts, VM state, scratch state, recorder ownership,
  and safepoint polling;
- acquire/release publication on weakly ordered ARM64 (x86 plain-MOV/TSO
  arguments are not portable evidence);
- full-TValue no-tear publication and forward/generation validation for shared
  tables;
- immutable trace publication, live slot/incarnation validation, trace-body
  retention, exit indirection, and safe mcode retirement;
- native-call/callback state, exact stack/result roots, callback suspension,
  and error/unwind cleanup;
- signal handlers that do not allocate, enter the loader, use unsafe TLV
  lookup, or wait on the scheduler.

## Port slices

### P0: native atomic and executable-memory substrate

Files:

- `src/lj_atomic.h`
- `src/lj_arch.h`
- `src/lj_markword.h`
- `src/lj_gc2token.h`
- `src/lj_tgslot.h`
- `src/lj_universe.h`
- `src/lj_mcode.c`
- focused C tests and their suite definitions

Apple clang 21 lowers the existing non-x86 `__atomic_compare_exchange_n()`
path for an aligned 128-bit object to one inline `caspal`; the native probe ran
successfully and had no helper call.  This is useful evidence for this machine,
but the source contract still needs to make the following explicit:

1. 16-byte alignment is checked at every authority type.
2. the compiler considers 16-byte atomics always lock-free for the selected
   target;
3. Mach-O contains CASP (or a reviewed inline LL/SC pair) and no `libatomic` or
   compiler atomic helper dependency;
4. split `hi/lo/hi` readers are audited separately.  Several are intentionally
   approximate snapshots, while recurring-state authorities require an exact
   compare/exchange snapshot;
5. acquire/release semantics are sufficient at every operation and no x86 TSO
   comment is treated as an ARM proof.

macOS `MAP_JIT` and execute-stable dual mappings are currently restricted to
x64 in `lj_mcode.c`.  ARM64 must use the hardened-runtime-compatible path and
must retain explicit instruction-cache synchronization and publication
ordering.  Concurrent code generation cannot proceed by globally flipping a
shared mapping between writable and executable.

P0 exit gate:

- focused 128-bit authority tests pass under contention on arm64;
- disassembly proves inline lock-free atomics and no hidden helper;
- JIT-disabled target objects can proceed past all architecture guards;
- the existing x64 artifact contract remains unchanged.

### P1: TG-owned ARM64 interpreter and safepoints

Files:

- `src/lj_target_arm64.h`
- `src/lj_emit_arm64.h`
- `src/vm_arm64.dasc`
- `dynasm/dasm_arm64.lua`
- `src/lj_dispatch.[ch]`
- `src/lj_safepoint.[ch]`
- `src/lj_frame.h`, `src/lj_err.c`, and state/thread helpers as required

The fixed ARM64 register model currently carries `global_State` directly and
indexes the shared GG dispatch/hotcount area.  It must instead carry a stable
TG/dispatch base, with G and J derived through reviewed helpers.  Entry,
resume, C-call, return, unwind, hot-loop, and function-header edges must publish
the same TG-local state and stack epochs as x64.

The ARM VM has no MT `vm_safepoint` equivalent and its bytecode backedges and
returns do not poll the TG request word.  Polling and native-entry/exit
acknowledgement are correctness requirements, not optional optimization.
The current ARM64 DynASM table cannot spell `ldar`, `stlr`, `dmb`, or `isb`, so
the assembler grammar and target/emitter opcode tables must be extended before
the VM or generated traces can express their ordering contracts.

For the first correct ARM path, shared-object operations should call the
already reviewed C helper protocols.  Architecture-specific inline fast paths
come only after helper-backed semantic tests pass.

P1 exit gate:

- clean native JIT-disabled build and amalgamated build;
- stock interpreter suite;
- threading attach/spawn/join, safepoint, table, GC2, weak/finalizer, and
  coroutine ownership suites;
- positive tests that force ARM polling and helper-backed shared table paths.

### P2: ARM64 trace lifecycle and JIT backend

Files:

- `src/vm_arm64.dasc`
- `src/lj_asm_arm64.h`
- `src/lj_emit_arm64.h`
- `src/lj_record.c`, `src/lj_opt_loop.c`
- `src/lj_trace.[ch]`, `src/lj_snap.[ch]`
- `src/lj_mcode.c`

The current ARM JLOOP and exit paths dereference the removed raw trace vector
and shared `jit_base`.  They need exact TraceVec/slot validation, live-body
retention, entry gating, safe redispatch for stale bytecode, TG-local
`jit_base`, and the current trace-exit restore ABI.

`XPOLL` recording and lowering are x64-gated and `asm_xpoll` is a no-op outside
x86.  ARM64 must emit a real poll with the required alias/fence semantics.
Shared table/upvalue/allocation stores must initially lower through the same C
helpers used by the interpreter; direct ARM load/store lowering is enabled only
with explicit generation, retirement, barrier, and memory-order proofs.

P2 exit gate:

- stock suite with JIT on;
- trace attach/flush/retirement, JLOOP stale-slot, side-exit, snapshot replay,
  recorder token, and concurrent table JIT tests;
- ARM Mach-O/disassembly checks for acquire/release/poll and mcode publication;
- repeated concurrent JIT+GC stress with no stale mcode or trace-body access.

### P3: ARM64 FFI calls and callbacks

Files:

- `src/vm_arm64.dasc`
- `src/lj_ccall.[ch]`
- `src/lj_ccallback.[ch]`
- `src/lj_ctype.h`
- `src/lj_crecord.c`
- ARM64 assembler CALLX lowering

The stock ARM callback trampoline writes a shared `CTState.cb` and `CTState.L`.
The x64 runtime now prepares a TG-local `CCallbackRuntime`, publishes a native
frame, handles nested callbacks and auto-attach/detach, and returns zero for a
stale callback on a TLS-less foreign thread.  ARM64 must use that protocol
while respecting the Darwin AAPCS64 argument/result ABI, PAC, BTI, and unwind
rules.

Generic traced `CALLXS` is currently rejected outside x64.  Interpreter FFI
parity comes first, callback error/unwind parity second, and traced generic
calls only after native-frame/root retention is proven.

P3 exit gate:

- scalar, mixed FP/GPR, struct, indirect aggregate, variadic, and errno FFI
  calls;
- callbacks from attached and raw pthreads, nested callbacks, stale callback
  entry, error/unwind, and callback-driven GC/JIT flush;
- traced calls retain exact result boxes and trace/mcode authority;
- no shared CTState scratch is reintroduced.

### P4: platform-positive validation and performance recovery

Files:

- `.github/workflows/*.yml`
- `tools/ci/platform_build.sh` and release/artifact scripts
- `tests/suites/*.lua` and architecture-specific C/Lua fixtures

The current macOS job and artifact profile explicitly target x86-64, several
suites compile with `-mcx16`, and some C tests silently skip their meaningful
assertions outside x64.  Add `macos-arm64` profiles and ARM-positive checks;
renaming a job without making its assertions execute is not coverage.

Required validation matrix:

- native arm64 JIT off/on, normal and amalgamated builds;
- stock LuaJIT suite and applicable M2-M10 suites;
- the L1-L7 weak-memory litmus set: message passing, join happens-before,
  Dekker/fence, channel FIFO, cell visibility, TValue no-tear, init-publish;
- focused GC2, table resize/forwarding, trace flush/retirement, FFI callback,
  weak/finalizer, profiler, PAC/BTI, and unwind stress;
- ASAN/UBSAN plus the lock-free C TSAN drivers;
- sustained mixed GC/table/JIT/FFI soak on multiple performance cores.

After semantic parity, replace helper routes only where ARM64-specific inline
fast paths have equivalent behavioral and artifact gates.  Benchmark changes
must be reported separately from correctness.

## Commit and push policy

Each coherent slice is committed and pushed after its focused gate passes.
Commits must not combine unrelated x64 cleanup with ARM work.  Every checkpoint
updates this note or adds a focused note with:

- what changed and why;
- the exact invariant being preserved;
- commands/tests that passed;
- tests not yet run and remaining failure modes.

## Initial risk register

1. A compiler-emitted `caspal` on this machine is not by itself proof for every
   deployment target or compiler; artifact checks remain mandatory.
2. Mixed-width subloads racing a 128-bit update need an ARM-specific coherence
   argument or replacement with an exact native snapshot.
3. ARM64 weak ordering exposes places where x64 relied on TSO, especially
   table forward/retirement publication and cross-address ordering.
4. macOS JIT write protection, I-cache synchronization, PAC/BTI, and unwind
   metadata can each produce builds that compile but fail only under runtime
   callbacks or trace retirement.
5. The existing test matrix is x64-shaped and includes silent non-x64 skips;
   every major phase needs a positive ARM execution assertion.
6. Parity with the current x64 fork is not evidence that all pre-existing
   lockless goals are finished.  Completion claims must distinguish port
   parity from upstream project maturity.

## Checkpoint log

### 1. Native lock-free 128-bit authority substrate

The first source checkpoint adds an explicit Apple AArch64 contract in
`lj_atomic.h`: aligned 16-byte compare/exchange must be compile-time
always-lock-free, and the typed struct builtin must lower inline.  ARM64 uses
exact 128-bit CAS snapshots for the markword, GC2 activation, TG slot, TG body,
and universe body helpers during bring-up.  The established x86-64 split
snapshot and inline `cmpxchg16b` paths are unchanged.

The new `tools/ci/arm64_cas128_contract.sh` gate builds for a macOS 11 minimum,
runs a success/failure/snapshot fixture, verifies the Mach-O architecture and
deployment metadata, and rejects undefined atomic helper symbols and
out-of-line calls.  The normal Apple target must emit acquire-release `caspal`;
a separate `-mcpu=generic` probe must emit an inline `ldaxp`/`stlxp` loop with
no helper.

Validation passed natively with Apple clang 21:

- ARM64 CAS128 runtime and Mach-O artifact contract;
- `t-gc2-markword-token`;
- `t-tgslot-token`;
- `t-tgregistry-slot`;
- `t-universe-token`;
- the exhaustive GC2 root-gate/store model;
- an x86-64 cross-object still contains inline `cmpxchg16b` and no helper.

The architecture gate intentionally remains x86-64 at this checkpoint.  A
full ARM64 VM is not yet buildable and must not be advertised merely because
its authority primitives now pass.  Remaining atomic audit work includes the
HugeTab split scouts and the overlapping 64/128-bit `GCtab` structural/weak
descriptor views.

### 2. Exact Apple ARM64 HugeTab scouts

Every production read of a `LJHugeEnt` address or metadata half now goes
through an architecture-aware accessor.  x86-64 retains the existing aligned
64-bit subload/CX16 contract and generated path.  Apple AArch64 instead takes
each scout from an exact lock-free 128-bit snapshot, so a CASP update never
races an overlapping 64-bit load in the compiler model.  The later full-slot
CAS still supplies the operation's linearization point where the protocol
requires one.

This is intentionally a conservative correctness-first path: an ARM64 scout
is currently a no-op compare/exchange and is more expensive than a plain
load.  It may be optimized only after an ARM-positive mixed-width or seqlock
artifact and stress contract proves an alternative.

Validation passed with Apple clang 21 for a macOS 11 minimum:

- the standalone `t-arena-hugetab` state-machine and concurrency fixture;
- the native object contains inline `caspal` instructions and no undefined
  atomic helper;
- an x86-64 cross-object still contains inline `cmpxchg16b` and no helper.

The overlapping `GCtab.struct_control`/`weak_record` views remain the next
128-bit audit boundary.

### 3. Exact Apple ARM64 GCtab structural/weak pair

Published Apple AArch64 accesses to the overlapping
`GCtab.struct_control`/`weak_record` union now use the complete 128-bit
authority.  Control and weak-record updates retry a full-pair CAS while
preserving the other logical half; reads take an exact snapshot.  The two
64-bit relaxed stores remain only in the private table initializer, before the
table can be discovered.  x86-64 retains its existing independent 64-bit
operations and pair-CX16 descriptor cutover.

`tools/ci/arm64_tab_pair_contract.sh` starts three contending workers: 200,000
structural-owner updates, 200,000 weak-cycle/capacity updates, and 200,000
same-value full-pair CAS operations.  It checks both capacity mirrors and final
values.  Separate no-inline probes cover every published helper; the Mach-O
gate rejects atomic runtime calls or any memory access other than inline
`caspal` in those probes.  The fixture passes natively with Apple clang 21 at a
macOS 11 minimum, including ASan+UBSan and TSan builds.  ARM64 and x86-64
cross-compiles of the production table object also contain their expected
inline `caspal`/`cmpxchg16b` instructions with no helper import.

### 4. Experimental Apple ARM64 interpreter/FFI bootstrap

The opt-in `LUAJIT_MT_ARM64_BOOTSTRAP` profile now produces a native macOS
ARM64 interpreter when combined with `LUAJIT_DISABLE_JIT`.  It includes the
fork's local-cell bytecodes, coroutine ownership claims, protected C-call ABI,
and TG-local FFI callback runtime.  Darwin callback stack decoding now follows
the packed scalar ABI rather than assuming eight-byte stack slots.

This was deliberately not the P1 exit gate.  At that checkpoint ARM still
used global dispatch, hook, and VM-state fields; it had no TG safepoint
polling; and ordinary bytecode stack/result stores lacked complete dirty-
epoch/root publication parity.  The focused interpreter/FFI bootstrap note
records the exact validation, ABI scope, and next root-publication hotspots.

### 5. TG-local Apple ARM64 interpreter dispatch

The JIT-disabled ARM64 interpreter now dedicates callee-saved `x25` to the
current `TGState.dispatch` table.  Every active VM entry, unwind landing, and
FFI callback entry reconstructs that pointer from the running state's
`tg_hint`; dynamic and static bytecode dispatch and hot counters no longer
index the universe-global dispatch table.  `x22` remains the global-state
register, and the old `TISNUMhi` cache is materialized in scratch only at its
six type checks.

Current-state and VM-state publication now use release stores to the TG
fields, with the universe-global VM-state word retained only as a transitional
mirror.  Hook dispatch acquire-loads and combines the TG-local and global hook
masks.  The shared hook counter is decremented by an assembly-callable atomic
C helper, because a raw ARM load/sub/store would lose decrements between
workers.  A first runtime pass exposed and fixed a post-helper register-
liveness bug in static redispatch.

`tools/ci/arm64_tg_dispatch_contract.sh` verifies the fixed-register ABI and
generated acquire/release/RMW instructions, rejects active JIT-off
`GG_G2DISP` addressing, and rejects atomic runtime helpers.  The integrated
bootstrap gate passed a clean assert build, the 387 stock tests, threading API
and hook tests, coroutine handoff/dead-resume coverage, and 320 FFI callback
rounds.  Six consecutive focused hook runs also passed.  The existing VM
safepoint fixture remains intentionally red at its first acknowledgement:
Stage 1 changes dispatch authority only and does not claim that the ARM stack
is safe to scan.

The next safe slice is complete stack/result dirty and collectable-root
publication.  Only after that parity is reviewed may ARM begin acknowledging
TG safepoint requests.

### 6. Rooted Apple ARM64 table and continuation paths

The active JIT-disabled ARM64 interpreter now routes global/table reads,
pair-aware table writes, raw integer/range stores, iteration, table length,
and the corresponding continuation/result slots through the fork's rooted,
generation-safe helper protocols. Allocating helpers reload the relocated Lua
stack and re-decode bytecode destinations; TSETR also performs the required
post-store table barrier, while TSETM uses the self-contained keyed range
helper.

A clean assert bootstrap, all 387 stock tests, threading/hook/coroutine and FFI
callback checks, and a 3,000-round GC-worker table/metamethod stress passed on
the native Apple ARM64 host. The focused rooted-table note records the exact
paths and ABI rules.

This checkpoint deliberately does not enable safepoint acknowledgement. The
remaining Stage 2 work is call/return and fast-result topology plus every
ordinary collectable-producing stack write. Stock ARM metatable/equality fast
checks and ISNEXT bytecode publication are also retained as explicit
lockless-interpreter debts rather than hidden by the rooted-table claim.

### 7. Apple ARM64 frame topology and result ranges

The JIT-disabled ARM64 VM now release-publishes complete call frames and the
result ranges moved by C returns, fast-function returns, Lua returns, generic
iterator calls, vararg copies, and vararg-function setup. The dirty-epoch
invalidation follows the completed release stores, and C-return source frames
are not retired through `L->base` until their replacement results are visible.
`IFUNCV` additionally copies each fixed argument to its destination before
release-clearing the old source root.

A shared `vm_call_publish` subroutine self-publishes the function/PC/argument
range before every interpreter call. This is deliberately conservative and
also avoids numeric-label collisions from expanding a range loop inside
`ins_callt`; the discarded inline form produced a reproducible SIGBUS in a
recursive vararg tailcall.

The clean assert bootstrap, all 387 stock tests, threading/hook/coroutine
checks, 320 FFI callback rounds, recursive tailcall/protected-call smoke, and
the focused full-GC result-retention fixture passed natively. The focused note
records the exact ordering and an important test boundary: compound dirty
deltas are currently influenced by the newly published call/return envelope,
so the deterministic Stage 2 contract remains red at the first deferred leaf
write rather than claiming per-opcode runtime attribution.

The next checkpoint must finish ordinary leaf writes, C-built metamethod
frames, generation-safe metatable/equality handling, and ISNEXT bytecode
publication. Safepoint acknowledgement remains disabled until that complete
gate is green.

### 8. Apple ARM64 ordinary leaf stack publication

The active JIT-disabled ARM64 interpreter now release-publishes the result of
`ISTC`/`ISFC`, `MOV`, `KSTR`, `KCDATA`, `UGET`, `FNEW`, `TNEW`, and `TDUP` and
invalidates the TG stack-scan stamp after the completed write. The conditional
test-copy bytecodes skip both publication and dirtying when their copy branch
is not taken. Allocation paths reload the potentially relocated stack and
re-decode RA after returning from C; fresh closures additionally use the
collectable-root publication helper, while fresh-table constructors retain
their existing object-publication responsibility.

The clean native assert bootstrap passed the deterministic source/emitted-
object contract, the focused full-GC root-retention fixture, all 387 stock
tests, threading/hook/coroutine checks, and 320 concurrent FFI callback
rounds. The runtime fixture owns semantic/reachability and retention coverage;
the static/emitted contract owns exact per-opcode store and dirty attribution
because the surrounding call/return envelope also advances the dirty epoch.

Stage 2 is still open at C-built metamethod frames, generation-safe protected
metatable lookup, rooted distinct equality/comparison, and `ISNEXT` bytecode
publication. ARM safepoint acknowledgement and the JIT remain disabled.

### 9. Apple ARM64 C-built metamethod frames

The C helpers behind ARM64 arithmetic, length, comparison/equality,
concatenation, and callable-object dispatch now release/root-publish every
method and operand slot in their manually assembled VM frames. Direct string
concatenation preserves its destination as a stack offset across allocation
and publishes the fresh result before a possible GC step. Callable-object
argument shifting remains high-to-low and publishes each destination before
the final method overwrite, so the transition may duplicate roots but never
drops one.

The shared `lj_state_stack_pubtv` abstraction was corrected to perform its
release self-store before dirty-epoch invalidation and its GC barrier last.
This restores the ordering promised to callers that make an owner-private
plain store before invoking the helper.

The clean assert bootstrap, deterministic root contract, all 387 stock tests,
threading/hook/coroutine and 320-callback checks, the focused metamethod fixture
under GC-worker pressure, and a 1,000-round full-GC metamethod stress passed
natively. The metatable contract remains deliberately red at the next missing
piece: rooted protected `getmetatable`. Rooted distinct equality/comparison,
the unconditional `setmetatable` C fallback, and `ISNEXT` publication also
remain before Stage 2 can close.

### 10. Apple ARM64 rooted protected metatables

ARM64 `getmetatable` now passes the authoritative argument and result stack
roots to a C helper which admits the receiver, captures and leases one exact
metatable generation, performs a non-waiting held `__metatable` lookup, and
leases any collectable protection value before publishing it. Stable absence,
nil, or stale protection exposes that same captured raw metatable, so concurrent
replacement cannot splice a protection decision from one generation to a raw
table from another. All retry paths release SMR and leases before waiting and
rebase both stack roots afterward.

ARM64 `setmetatable` now always uses the C implementation for protection,
replacement/clear, release publication, and GC barriers. The clean normal
assert bootstrap and full native gates passed, as did an amalgamated ARM64
assert build and focused raw/protected/full-GC semantics.

The raw C API `lua_getmetatable` remains a separate concurrency debt; this
checkpoint only certifies the protected base-library operation. Rooted distinct
equality/comparison and target-first full-word `ISNEXT` despecialization remain
the final lockless-interpreter pieces before Stage 2 safepoint work.

### 11. Apple ARM64 rooted equality and comparison

Distinct table/userdata equality and ordered comparisons now enter dedicated
ARM helpers with authoritative frame-relative operand roots. Both operands and
both candidate methods remain in TG anchors through retry-capable exact
metamethod lookup and release-publication of the complete call frame. ARM no
longer reads table/userdata metatable pointers or the negative metamethod cache
inline for equality, and the shared comparison return path reloads a relocated
`BASE` before dispatch.

The equality helper enforces Lua 5.1's exact same-handler rule for both
operands. The comparison helper preserves exact-handler `__lt`/`__le` rules and
the swapped-and-negated `not (rhs < lhs)` fallback. Caught comparison errors
raw-copy only their type tags and explicitly release all anchors before
throwing, because ARM fast protected-call frames do not carry the x64 anchor
checkpoint.

The clean native assert bootstrap, all 387 stock tests, threading/hook/
coroutine checks, 320 FFI callback rounds, the strengthened source/object
contract, nested full-GC retention, equality/fallback semantics, and repeated
`pcall`/`xpcall` comparison-error anchor restoration passed. Legacy API and
non-ARM comparison/equality entry points remain isolated; C pseudo-index
capture is recorded as separate API debt.

Target-first full-word `ISNEXT` despecialization is now the remaining known
JIT-off interpreter mutation gap before Stage 2 can close and ARM safepoint
acknowledgement can begin.

### 12. Apple ARM64 JIT-off ISNEXT publication

Failed `ISNEXT` specialization now release-publishes the complete target word
as `ITERC` before release-publishing the complete guard word as `JMP`. The
full-word helper acquire-loads the current instruction and replaces only its
opcode, preserving all operands and eliminating both the old guard-first
`JMP+ITERN` window and byte-store/full-word lost updates.

The transition keeps guard and target addresses in callee-saved ARM registers,
saves the authoritative Lua BASE and PC across both C calls, reloads BASE, and
redispatches at the target. The guard's decoded RD field supplies the target
address, preserving ARM's data/address-dependency ordering after guard
publication.

The clean combined interpreter gate passed all 387 stock tests, threading/
hook/coroutine and 320-callback checks, the strengthened metamethod gate, exact
source/object/archive `ISNEXT` checks, a deliberately executed
`ISNEXT+ITERC` intermediate state, and a separate authentic failure followed
by exact-word verification, full GC, and generic rerun.

This closes the known JIT-disabled interpreter publication gate. ARM
safepoint polling and acknowledgement are next. The JIT-enabled
`ISNEXT`/`JLOOP` path is preserved but remains explicitly deferred with the
rest of the ARM64 JIT port.

### 13. Apple ARM64 interpreter safepoints

The ARM64 VM now acquire-loads `TG.poll` and `TG.profile_request` as separate
32-bit words and routes nonzero requests through a common
`lj_safepoint_ack_check` boundary. Taken iterator and numeric-for paths,
`ILOOP`, `JMP`, and fixed/vararg interpreter entry provide bounded progress.
The numeric `IFORL` check is on the taken integer or floating-point backedge,
so a pure loop cannot postpone a remotely published STOPREQ indefinitely.

Leave-to-C polls before restoring the previous C frame. Unwind-to-C rebuilds
TG dispatch, polls before publishing VM state C, and preserves the incoming
error status across the helper. Ordinary `IFORL`, `IFUNCF`, and `IFUNCV` polls
are compiled unconditionally so their semantics remain present when a future
JIT-capable ARM64 runtime executes interpreter bytecode with JIT disabled; the
separate J bytecodes and native trace paths remain deferred.

The assert bootstrap build passed the strengthened VM fixture, including fixed
and vararg entry, iterator and return boundaries, exact unwind behavior, and an
asynchronously interrupted allocation-free numeric loop. Generated-object
inspection found nine distinct 32-bit acquire poll/request pairs and exactly
three helper relocations for common redispatch, leave, and unwind. The prior
root-publication, metamethod, and ISNEXT runtime fixtures also remained green.

ARM64 TG-local SIGPROF publication is explicitly not enabled here.
`LJ_THR_TG_SIGNAL_CACHE` still covers only x64 Linux/macOS, so
`LJ_PROFILE_TGLOCAL` remains off on ARM64 and the profiler uses its legacy
signal path. JIT-capable ARM64 runtime validation, trace polling/XPOLL, and the
exact ARM64 signal-cache port remain later checkpoints.
