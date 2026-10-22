# Upstream refresh at 489bb089

This merge integrates LuaJITMT
`origin/v2.1` at `489bb089` into the ARM64 branch at `8893ffc4`, from common
ancestor `a649f737`. It retains the generic scalar capability, root-entry
snapshot tests, shared source/import capture corrections, numeric-spill
admission and current gate registration. The focused candidate evidence below
does not replace a complete gate run on the merged checkpoint.

The first complete-gate attempts after integration are not passing results.
Concurrent bootstrap and experimental runs on `cac092fd` exhausted the
120-second legacy-bytecode fixture deadline. An instrumented standalone run
and the unchanged serial bootstrap run both completed that fixture within the
original limit. The serial bootstrap then failed threaded tmpbuf stress at
`lj_gc2_sweep_to_idle`: `table NEEDSCAN leaked to normal sweep close`.
That collector race is under investigation; neither the assertion nor the
gate deadline has been weakened. Logs are
`/tmp/luajitmt-upstream-{bootstrap,jit}-gate.log` and
`/tmp/luajitmt-upstream-bootstrap-gate-serial.log`.

## Scope and history

Upstream's new code and tests are retained. Its bulk `bench/`, `notes/` and
`plan/` additions are deliberately excluded: those three trees retain the
ARM64 branch's versions. That preserves the existing archived-notes layout
instead of restoring about 17,000 historical or generated evidence paths as
an incidental code refresh. This is a tree-level merge decision, not evidence
that the omitted performance reports or historical claims were revalidated.

The upstream runtime changes include automatic-GC request/control and
finalizer ownership, constructor retention and sweep fairness, arena reuse
and statistics publication, scalar table readers and array iteration,
rooted cdata/metatable capture, native FFI caller stack publication, bounded
trace-stop admission and deferred root-abort retirement. The shared C changes
apply to ARM64. The x64-only pure-cdata loop optimization remains x64-only.

## ARM64 merge decisions

- Upstream `TRACE_RETIRE_PENDING` uses common trace flag bit `0x10`. ARM64's
  admission certificate formerly occupied the same byte. The certificate now
  uses the existing 32-bit `GCtrace.unused_gc64` field through target-local
  acquire/release accessors. Common lifecycle flags and their upstream values
  remain unchanged. Trace size, alignment and all field offsets are unchanged
  (native GC64: size 160, alignment 8, admission offset 16, lifecycle byte 133).
- Private trace initialization zeros the certificate; assembly sets it only
  after the existing semantic and register-allocation checks. Compact trace
  publication copies the source certificate instead of clearing its padding.
  Metadata views independently acquire and revalidate the complete 32-bit
  certificate and common lifecycle byte. Unknown certificate bits reject
  entry. Published certificates remain immutable. Tests distinguish common
  pending-retirement bit `0x10` from a loop certificate and reject upper bits.
  A compiler probe emits a 32-bit acquire load and release store with no
  atomic helper import. Extra metadata-load cost has not been benchmarked.
- ARM64's shared interpreter allocation predicate now recognizes upstream's
  pending automatic request while phase is IDLE: nonzero cycle leader and
  clear automatic-control flags. The leader/phase loads are 32-bit acquire;
  the packed automatic flags use an 8-bit acquire. Existing threshold and
  local-debt cadence remain intact.
- ARM64 already requires the full XPOLL path before first attachment, so the
  upstream x64 first-attach emission fix needs no duplicate target code.
- The custom ARM64 first-side stop/publication path remains separate. Its
  existing GDB metadata prepare/commit operations cover the upstream move of
  generic GDB registration out of trace-save. Remote owner-root completion
  retains ARM64 STOPREQ and owner-only hotcount-reset checks.
- Rooted ARM64 comparisons request only receiver/key capture from the updated
  common metadata helper. A null optional method-result pointer avoids adding
  an unused speculative cdata lookup; their existing rooted lookup semantics
  remain unchanged.
- The ARM64 scalar remote-flush fixture's identity function now passes its
  caller address to the upstream helper's expanded signature, preserving the
  new proof that the blocked call came from published generated code.

## Native evidence so far

On the final `348dee94` forward candidate:

- The experimental stock source and genuine-v2 import corpora each pass all
  509 tests. Safe-profile source and import corpora each pass all 387 tests.
- The complete root-entry contract passes on ARM64 and ARM64e/BTI, including
  upper certificate-bit rejection and independence from lifecycle flags.
- Numeric spills pass on both ARM64 and ARM64e: actual FMA/PHI/copy/reload
  machine code, independent layout corruptions, guard snapshots, profiling,
  STOPREQ unwind, resumed entry, full collection and flush/retirement.
- The interpreter GC2 contract again passes on both targets with emitted
  32/32/8-bit acquire checks and pending-IDLE/STOP/FINPAUSE runtime cases.

The refresh also supplies a native Darwin runner for upstream huge-tail
allocation/lifetime tests and all six remote owner-root-completion modes. It
uses fixture-only translation units around the actual arena/safepoint sources;
the remote-wait witness retains the real kernel wait. Both positive runs and
single-change negative controls are recorded in
`notes/darwin-upstream-gc2-runtime.md`. Production source is unchanged by those
test adaptations.
The registered case also passes on the final `8893ffc4` parent and restores
the safe ARM64 profile, verified by disabled JIT status and absent `jit.opt`.

On the `25275a13` forward candidate, with assertions and experimental ARM64
enabled:

- A clean build passes both the stock source suite (509 tests) and its genuine
  original-v2 import corpus (509 tests).
- The upstream pending-root-abort fixture passes all four abort modes,
  terminal-slot cleanup, capacity and eventual drain checks.
- Scalar table hits pass protected-page, replacement, resize, retirement and
  paused-reclaimer tests. Scalar iteration passes all eleven authority modes.
- Table sweep coalescing passes ordinary/huge and narrow/wide cases, stale
  scanner refusals, rollover collections and private reservation failure.
  Constructor publication/cancellation/automatic deferral, fairness with
  multiple workers and detached owners, and mixed eligible/deferred arena
  progress also pass.
- The migrated root-entry, scalar-loop, numeric-loop and mcode-retirement C
  fixtures pass. Source capture identity, recursive frame/return captures,
  source/import iterator and conditional captures, and C debug-local mutation
  pass with the merged runtime.
- The complete bytecode C fixture and true-legacy open-upvalue admission pass.
  ARM64 scalar CALLXS production and remote-flush tests pass, including the
  generated-entry check, as does interpreter-mode threaded upvalue behavior.

Earlier checks on the original `01b8c560` review candidate:

- Clean safe ARM64 and arm64e/BTI builds and the complete interpreter GC2
  contract pass. Its emitted-object checks verify all acquire widths. Runtime
  tests cover pending IDLE requests and STOP/FINPAUSE negatives through TNEW,
  TDUP and `string.char`, together with existing hard-debt cadence tests.
  Removing only the new pending-request block in a disposable copy makes the
  first pending-IDLE case fail as intended.
- Safe ARM64 stock suite: 387 passed. Original-v2 bytecode import/roundtrip,
  true-legacy open-upvalue admission, rooted metamethod chains, source capture
  identity, interpreter threading upvalues and cdata capture pass.
- Upstream restart-at-maximum-threshold and all 16 nested-finalizer control
  combinations pass natively, as does the finalizer spawn/query Lua fixture.

## Existing x64 target

A fresh assertion/helper x64 build from the `348dee94` merge candidate passes
under Rosetta:

- Stock source and genuine-v2 import corpora: 509 tests each, plus the complete
  bytecode C fixture with forced reader collections and worker sharing.
- Full FNEW default and optional traced modes, including the upstream wide
  slot-reuse check; source/import capture identity, recursive frame/return,
  iterator and conditional cases under default, `-O0`, and `-O-cse`.
- Pending root-abort retirement; real first attachment with optimized and
  optimizer-disabled polling; bounded side trace-stop admission.
- Scalar CALLXS, remote-flush boxed/bool/aggregate paths, and generated
  callback stack normal/growth/unwind cases.

These are focused checks, not full upstream-suite or complete-port
conformance. The upstream harness still marks several new fixtures Linux/x64
only, and some require GNU linker wrapping or Linux signal/syscall behavior.
Direct execution of the generic callback-stack and threaded closed-upvalue
JIT fixtures reaches assertions requiring native traces outside ARM64's
admitted operations; their assertions were not weakened. The full FNEW C
fixture also references x64 registers and condition codes unconditionally and
does not compile as an ARM64 test. Target-specific callback and interpreter
coverage must be distinguished from those exact generic native-code proofs.
Primary-branch validation does not automatically validate the merged runtime.
