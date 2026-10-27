# LuaJITMT ARM64 port status

This directory is intentionally small. It contains the current ARM64 status
and the durable lockless architecture constraints. The 938 historical notes
and reports that are no longer authoritative are retained in
[`archive/`](archive/README.md).

## Status at a glance

- Current branch: `v2.1`.
- Integrated upstream comparison point: LuaJITMT `v2.1` at `489bb089`.
  See
  [`upstream-refresh-2026-10-22.md`](upstream-refresh-2026-10-22.md) for merge
  decisions, retained artifact exclusions, tests and remaining limits.
- ARM64 remains opt-in with `LUAJIT_MT_ARM64_BOOTSTRAP`.
- Native ARM64 JIT work additionally requires
  `LUAJIT_MT_ARM64_JIT_EXPERIMENTAL`.
- Current review implementation replaces whole-trace templates with operation
  proofs for scalar LOOP/FORL/FUNCF bodies, first scalar side traces, rooted
  scalar reads and existing-key stores, unsunk table construction, and fixed
  scalar FFI calls, including bounded promoted variadic suffixes. Root and child publication certificates remove immutable
  release-entry scans; native coverage and progress counters expose successful
  entry, exits and refusals. Shared build profiles bind runtime, archive,
  fixture and helper-library identity. All review work streams are integrated.
  See [`review-implementation-2026-10-25.md`](review-implementation-2026-10-25.md).
- Fixed owned I64/U64 CALLXS arguments pass all four ARM64/arm64e
  assertion/release profiles, including nine arguments, exact CData roots,
  prior boxed-call inputs, fixed-prefix varargs and independent malformed
  semantic/post-RA views. Each profile records 344 native observations and
  independent 4-positive/26-negative admission checks.
  See [`arm64-jit-i64-arguments-2026-10-26.md`](arm64-jit-i64-arguments-2026-10-26.md).
- Latest combined runtime validation: the fixed I64/U64 argument checkpoint
  on 2026-10-26, frozen at `f0c4f574`.
  The complete experimental and bootstrap gates passed, including all four
  M7 scalar CALLXS profiles. ARM64 and signed x86-64 each passed both 509-case
  stock source/import corpora. The ordinary main ARM64 rebuild passes both
  corpora, six fixed-argument and nine boxed-result smoke cases without test
  helpers. See the [fixed-argument evidence](arm64-jit-i64-arguments-2026-10-26.md).
  The preceding boxed-result and shared constructor/recovery checkpoint is
  documented in the [boxed-result evidence](arm64-jit-boxed64-results-2026-10-26.md)
  and [eight-profile recovery matrix](gc2-constructor-recovery-2026-10-26.md).
  The earlier scalar bit-operation and NUM fusion-boundary work is in
  [`arm64-jit-scalar-bitops-2026-10-26.md`](arm64-jit-scalar-bitops-2026-10-26.md)
  with its initial evidence and limits. Earlier shared-helper and
  segment-map race evidence remains in the
  [rooted local-cell checkpoint](arm64-jit-rooted-local-cells-2026-10-26.md).
  Earlier callback recording and the repaired x64 test-only unwind are in
  [`arm64-jit-callback-recording-2026-10-26.md`](arm64-jit-callback-recording-2026-10-26.md)
  and [`x64-vmevent-test-unwind-2026-10-26.md`](x64-vmevent-test-unwind-2026-10-26.md).
  The architecture-review checkpoint and its sanitizer/performance scope remain in
  [`review-validation-2026-10-25.md`](review-validation-2026-10-25.md).
- Earlier focused FFI validation: boxed I64/U64 result checkpoint `2ba1e91b`
  passes its four-profile ARM64/arm64e assertion/release contract, with 1,722
  native boxed results and independent admission mutations per profile. The
  earlier combined run at `9651f937` failed in native TNEW on a shared
  constructor root-membership assertion. With the
  [constructor/recovery repair](gc2-constructor-recovery-2026-10-26.md), the
  eight-profile ARM64/arm64e/x64 recovery and pending-root matrix and renewed
  combined validation now pass. The original failure remains documented in the
  [boxed-result note](arm64-jit-boxed64-results-2026-10-26.md).
  The preceding scalar variadic checkpoint `328fcb23`
  passed the complete M7 FFI suite in all four ARM64/arm64e assertion/release
  profiles. ARM64 and signed x64 each passed both 509-case stock corpora. The
  ordinary ARM64 runtime also passes all eight variadic smoke cases without
  test helpers. See
  [`arm64-jit-scalar-varargs-2026-10-26.md`](arm64-jit-scalar-varargs-2026-10-26.md).
- The scalar bit-operation tranche now admits 32-bit bit.* operations in
  LOOP/FORL roots, including stock integer narrowing, instruction fusions and
  exact snapshot-only INT-to-NUM restoration. All four ARM64/arm64e
  assertion/release profiles pass 52 native cases, malformed-view checks,
  deterministic profile/STOPREQ exits and independent INT/NUM cross-loop
  fusion rejections. See
  [`arm64-jit-scalar-bitops-2026-10-26.md`](arm64-jit-scalar-bitops-2026-10-26.md).
- Fixed scalar FFI stack arguments pass the four-profile M7 suite,
  including compact Darwin stack slots, spill separation, nested callbacks
  and C++ unwind/recovery, both separately and within the combined gate.
- The experimental gate includes generic roots, rooted effects, scalar-side
  publication/entry/retirement, native numeric adaptation, and M7 scalar CALLXS
  on ARM64 and arm64e/BTI with both assertion and release runtimes.
- Native ARM64 source and original-v2 import stock corpora each pass all 509
  cases at the combined `f0c4f574` checkpoint and its ordinary main rebuild.
- The complete bootstrap gate passes, including 387 applicable stock cases,
  threaded tmpbuf, Darwin native roots/GC2, ARM64/arm64e callbacks,
  variadic/aggregate interpreter FFI and x86-64 errno unwind.
- The signed x86-64/Rosetta runtime at `f0c4f574` passed both 509-case corpora.
  The combined gates also ran x86-64 callback-result lifetime and errno unwind.
  Shared rooted-store, raw nil-deletion, upvalue/cell and segment-map race
  fixtures were validated at the earlier local-cell checkpoint. Earlier
  authentic CALLXS/remote-flush/callback and callback-stack relocation/unwind
  evidence is at `9b0c6e18`. These are targeted regressions, not a complete
  x86 umbrella or native Intel performance result. See
  [`x64-rosetta-final-regression-9b0c6e187-2026-10-25.md`](x64-rosetta-final-regression-9b0c6e187-2026-10-25.md).
- Earlier investigations and checkpoints remain in
  [`checkpoint-2026-10-25.md`](checkpoint-2026-10-25.md),
  [`checkpoint-2026-10-22.md`](checkpoint-2026-10-22.md) and the implementation
  ledger. Passing later runs do not retrospectively explain earlier stalls.
- The development checkout was restored to the ARM64 experimental assertion
  profile after final validation. Select and rebuild the intended profile
  explicitly when switching configurations.

This is a working Apple Silicon port with substantial native execution
evidence. It is not yet a completed or general ARM64 LuaJIT backend. Native
JIT recording and entry outside the admitted capabilities and exact ABI
certificates below are intentionally rejected; ordinary interpreted Lua remains
available.

## Implemented and exercised

### Interpreter and lockless runtime

- Lockless interpreter bootstrap, TG-local state, safepoints, frame and root
  publication, protected calls, metamethods, selected table operations, and
  interpreter FFI/callback lifecycle.
- Explicit ARM64 acquire/release ordering for the audited mutable VM roots and
  edges. The target-local VM acquire primitive now covers C-closure upvalues,
  base-metatable roots, Lua 5.2 iterator metatables, Lua-closure `uvptr[]`,
  `L->openupval`, and `GL->wrapf`.
- Forwarding-safe `lj_vm_next` array/hash traversal on ARM64 and arm64e/BTI,
  including active mutation, generation forwarding, retirement retry,
  separated arrays, hidden keys, and caller-owned result snapshots.
- Callback-result lifetime through post-detach TG reclamation.
- Lightuserdata segment creation uses a fixed map of atomic, immutable prefix
  slots shared by the universe. A concurrent test reproduced corruption of
  old tokens with the previous movable map; the replacement passes capacity,
  concurrent decoding and forced same-slot CAS races in all four ARM64 profiles.
  See [`lightuserdata-segment-map-2026-10-25.md`](lightuserdata-segment-map-2026-10-25.md).
- GC2 hard-assist parity at interpreter allocation checks, including the fast
  function helper and the `TNEW`/`TDUP` bytecode paths. Shared pacing fields
  are acquired explicitly and the exact local-debt batch boundary is covered.
- Fresh-object construction preserves recovery's borrowed lifetime while
  committing or abandoning root membership. Deterministic sampled-state,
  claimed-drainer and release-repair cases pass with the full recovery and
  pending-root fixtures in eight ARM64/arm64e/x64 profiles. The repaired
  combined gate also passes native TNEW on both ARM64 targets. See the
  [constructor/recovery note](gc2-constructor-recovery-2026-10-26.md).
- ARM64 FFI aggregate classification copies CType metadata through the existing
  writer-token-aware snapshot helpers. Nested homogeneous aggregates, mixed
  aggregates, indirect results, register overflow, errno, and concurrent cdef
  publication have native ABI coverage.
- An exact lock-free admission/count word prevents the first secondary VM
  admission while any legacy open upvalue still points into an owner's stack.
  Rejection preserves ordinary owner semantics; admission succeeds after the
  captures close. See the remaining legacy compatibility limitation below.
- Source captures share their closed-cell identity even when the captured
  local is read-only. Lua and C debug-local writes update that cell and use
  the existing trace-flush handshake when its value changes. Current-frame
  addressing, fresh invocation cells, and `RETF` boundaries have targeted
  regressions; see [`source-upvalue-conformance.md`](source-upvalue-conformance.md)
  and [`jit-cell-retf-2026-10-22.md`](jit-cell-retf-2026-10-22.md).
- Legacy VM-event and profiler callbacks on ARM64/x64 reserve dispatch
  suppression on their own TG, preserving peer recorder continuity. Global
  ACTIVE retains ordinary-hook/callback exclusion. Profiler stop also discards
  already-pending SIGPROF before restoring the saved disposition, with explicit
  failure/restart ownership. The seven-build callback contract covers both
  target/runtime modes and no-JIT profiling; see
  [`arm64-jit-callback-recording-2026-10-26.md`](arm64-jit-callback-recording-2026-10-26.md).

### Bounded native JIT

- ARM64 and arm64e/BTI trace publication, authenticated entry and exits,
  retirement, flush/reuse, GDB-JIT preparation, and bounded root/first-side
  entry paths.
- Root and first-child publication seal exact body/allocation generations,
  IR/snapshot geometry, source identity and machine code after independent
  semantic and allocated-layout checks. Release entry validates the seal and
  reacquires every mutable lifetime, topology, bytecode, request and PAC
  authority. Assertion entry also rescans immutable content. Both target and
  release corruption/lifecycle tests and measured entry costs are recorded in
  [`review-implementation-2026-10-25.md`](review-implementation-2026-10-25.md).
- Scalar self-linked `LOOP` roots use independent semantic and allocated-state
  capabilities for checked INT arithmetic, NUM arithmetic/conversions,
  comparisons, PHI copies/shuffles, snapshots and INT/NUM spill frames. NUM
  spans have explicit width, alignment, ownership and snapshot checks, with
  native mixed-pressure and fused-arithmetic regressions. The old whole-trace
  NUM arrays are removed. Heap operations outside the rooted helper profiles
  below remain closed; see
  [`arm64-jit-scalar-capability.md`](arm64-jit-scalar-capability.md) and
  [`arm64-jit-numeric-spills.md`](arm64-jit-numeric-spills.md).
- Scalar 32-bit bit.* fast functions in LOOP/FORL roots use the stock
  recorder and emitter. Independent operation/allocation proofs cover bitwise
  logic, byte swap, shifts/rotations, TOBIT's exact numeric bias, integer
  negation, wrapping ADD/SUB and bounded target fusions. Snapshot-only
  NUM<-INT conversions require a separately restorable integer source.
  FUNCF/side bitops, BROL, string coercion and cdata/64-bit operations remain
  excluded. See the [bit-operation note](arm64-jit-scalar-bitops-2026-10-26.md).
- Rooted table length and scalar reads are validated per helper operation,
  scratch root and immediate retry/type guard. Numeric, string and boolean
  keys, numeric/boolean results, multiple tables and mixed read/length effects
  have native and malformed-graph coverage. Table spills retain explicit
  width, alignment and ownership checks. Resize, retirement, GC, STOPREQ and
  type-change exits keep the existing runtime authorities.
- Existing-key scalar stores and unsunk TNEW use their own publication and
  rooted-effect contracts; raw heap effects remain excluded.
- GC-valued rooted reads and existing-key stores cover strings, tables,
  functions, threads, userdata and opaque cdata. Their phase-gate lifetime
  proof permits values across later helpers, full-width spills, PHIs and exit
  restoration. Three distinct TG-local input slots feed the anchored store
  helper. Numeric/boolean results through GC identity keys use the generic
  bounded reader; string/number/boolean keys keep the optimized scalar helper.
  Native GC identity keys, mixed-value pressure and a deterministic
  source-edge-removal/collector handoff pass on ARM64 and arm64e/BTI in both
  assertion and release profiles; see
  [`arm64-jit-rooted-gc-values-2026-10-25.md`](arm64-jit-rooted-gc-values-2026-10-25.md).
- Generic bounded reads also accept exact lightuserdata keys and results.
  Guarded loads, loop-carried values, full-width spills and snapshot restoration
  preserve the segmented token without treating it as a GC pointer. The full
  rooted-value contract passes in all four profiles; see
  [`arm64-jit-rooted-lightuserdata-2026-10-25.md`](arm64-jit-rooted-lightuserdata-2026-10-25.md).
- Rooted nil reads in LOOP/FORL use a bounded raw-absence and NULL-metatable
  proof, including nil and NaN keys. Insertion or a metatable causes an exit
  into ordinary lookup. Native key/mutation matrices and independent admission
  checks pass in all four profiles; ARM64 and signed x64 stock source/import
  each pass 509 cases. See
  [`arm64-jit-rooted-nil-reads-2026-10-25.md`](arm64-jit-rooted-nil-reads-2026-10-25.md).
- Existing-key rooted stores also support lightuserdata keys and values in
  LOOP/FORL. Internal table markers remain excluded despite sharing the tag.
  All four native profiles and signed x64 helper/stock regressions pass; see
  [`arm64-jit-rooted-lightuserdata-stores-2026-10-25.md`](arm64-jit-rooted-lightuserdata-stores-2026-10-25.md).
- Rooted nil stores delete present keys and perform absent-key no-ops when
  the current table has no metatable. Retained nil-valued hash nodes count as
  absence; `__newindex` cases fall back to the interpreter. Native LOOP/FORL
  key matrices, deterministic resize/delete interleavings and independent
  admission negatives pass in all four profiles, with signed x64 helper and
  stock regressions. See
  [`arm64-jit-rooted-nil-stores-2026-10-26.md`](arm64-jit-rooted-nil-stores-2026-10-26.md).
- Rooted non-nil insertion supports ordinary LOOP/FORL assignments recorded
  under an absent-key/NULL-metatable certificate. Runtime `__newindex` checks,
  generation repair, GC roots and keyed CAS preserve current Lua assignment
  behavior. Native value/key matrices, fresh array/hash growth, forced stale
  commit repair and independent admission negatives pass in all four profiles,
  with shared signed x64 helpers and both stock corpora. See
  [`arm64-jit-rooted-insertion-2026-10-26.md`](arm64-jit-rooted-insertion-2026-10-26.md).
- Exact three-argument `rawset` CALLs support zero or one result in bounded
  LOOP/FORL roots. Rooted insertion bypasses `__newindex`, while exact caller
  snapshots preserve refusal, unwind and completed-store effects. Independent
  admission, native no-replay/growth tests and the complete rooted-GC and M7
  FFI suites pass in all four profiles, with ARM64/x64 stock regressions. See
  [`arm64-jit-rooted-rawset-2026-10-26.md`](arm64-jit-rooted-rawset-2026-10-26.md).
- Closed closure-upvalue reads and writes in LOOP/FORL use exact function,
  cell and value leases with acquire/release publication. Independent source,
  semantic and post-RA checks preserve literal representation, current-function
  spills and completed-store continuations. All visible value classes,
  no-replay exits, cell joining, GC retention and concurrent native readers and
  writers pass on ARM64/arm64e, assertion/release. Shared helpers and stock
  regressions pass on signed x64 as well. See
  [`arm64-jit-rooted-upvalues-2026-10-26.md`](arm64-jit-rooted-upvalues-2026-10-26.md).
- Captured-local CGET/CSET in root LOOP/FORL use owned-stack cell helpers,
  leased payload sampling and independent admission. Cell identity loads stay
  guard-only and never enter registers, spills or exit snapshots. Visible
  values, GC, peer sharing, exact post-store exits and independent malformed
  snapshot/IR rejection pass on ARM64/arm64e with assertions and in release.
  Shared x64 helpers and both architectures' stock corpora also pass.
  See [`arm64-jit-rooted-local-cells-2026-10-26.md`](arm64-jit-rooted-local-cells-2026-10-26.md).
- Scalar `FORL` supports constant and variable integer bounds/steps through
  def-use and source-control-flow checks. Scalar fixed-arity `FUNCF` supports
  bounded arithmetic, argument/cell loads and one scalar return.
- First scalar side traces inherit the selected parent's exact snapshot and
  locations, then close as a self-loop or through its certified root tail.
  Successful native handoff and bounded exits are required; ROOT and LOOP
  children are retired through GC-claim, scoped flush and full flush on both
  targets. Side-of-side traces, heap-effect children and stitches remain closed.
- TG-local XSAVE staging and native-frame lifecycle support fixed scalar FFI
  signatures classified from stable CType snapshots, separate GPR/FPR argument
  banks, naturally packed outgoing stack arguments and independently validated
  allocated operands/results. Multiple calls per loop and rooted boxed
  pointer/I64/U64 results replace the three exact templates. Stack layouts retain separate
  semantic and allocated-state checks, including frame and spill separation.
- Fixed scalar float arguments/results support exact NUM/INT-to-FLOAT and
  FLOAT-to-NUM conversions, float-to-float call chains, ninth-argument stack
  overflow and mixed compact integer/float stack slots. Native rounding,
  NaN/infinity exits, no-replay and independent malformed-graph checks pass
  across all four M7 profiles. See
  [`arm64-jit-ffi-float-2026-10-26.md`](arm64-jit-ffi-float-2026-10-26.md).
- Shared-table callable reads compose the bounded rooted-hit operation with
  scalar CALLXS in FORL roots. One-level native-width function pointers retain
  exact outer CType guards and normalized variadic signatures. The focused
  contract and broader M7 suite pass all four ARM64/arm64e assertion/release
  profiles at `0761fb79`, covering replacement, active invalidation, pre-frame
  GC lifetime, zero arguments, string/integer/boolean keys and independent
  malformed views. ARM64 and signed x64 each pass both 509-case stock corpora. See
  [`arm64-jit-table-callables-2026-10-27.md`](arm64-jit-table-callables-2026-10-27.md).
- Direct scalar producer `CALL` to consumer `CALLM` chains support nested
  integer, float and double calls with register and stack arguments. Source
  geometry, final-argument identity and continuation snapshots have independent
  semantic/post-RA checks. Six forced producer/consumer exits preserve exact
  effects without replay in all four M7 profiles; both ARM64 stock corpora
  pass 509 cases. See
  [`arm64-jit-ffi-callm-2026-10-26.md`](arm64-jit-ffi-callm-2026-10-26.md).
- Variadic scalar calls validate the named prefix, exact CARG separator and
  promoted number/boolean/string/nil suffix against the original Lua snapshot.
  Independent post-RA checks include eight-byte unnamed stack slots and spill
  separation. Native ABI, no-replay and malformed-clone tests pass in all four
  M7 profiles, with both stock corpora passing on ARM64 and signed x64. See
  [`arm64-jit-scalar-varargs-2026-10-26.md`](arm64-jit-scalar-varargs-2026-10-26.md).
- Exact FFI lifecycle evidence covers errno, normal and forced exits, STOPREQ,
  one-level and depth-two callbacks, callback errors, cleanup/reuse, remote GC
  and flush, mutable cdata `__call`, double ABI/results, boxed-pointer identity,
  boxed I64/U64 payloads and result-root survival.
- Boxed signed/unsigned 64-bit results compose with existing fixed and
  promoted-variadic arguments, including a scalar CALL producer feeding a
  boxed CALLM consumer. Both admission passes bind the exact hidden and caller
  result snapshots to the CNEW root. Full-width patterns, typedef/const
  identities, discarded results and forced POSTCALL exits without replay pass
  in all four ARM64 profiles. Boxed CALLM producers remain closed. See
  [boxed-result implementation and evidence](arm64-jit-boxed64-results-2026-10-26.md).
- Fixed owned I64/U64 arguments bind exact guarded CData types or completed
  ordinary boxed-call results to their payload loads and caller argument roots.
  Independent admission checks constrain every payload use to an exact fixed
  ABI argument. Eight GPRs plus a ninth stack argument, full-width spills,
  typedef/const identities, fixed-prefix varargs and no-replay exits pass in
  all four ARM64 profiles. See the
  [fixed-argument evidence](arm64-jit-i64-arguments-2026-10-26.md).
- Lockless OS-error restoration for C, fast-function and admitted JIT unwind
  paths. A native observer proves the C++ callback-error fixture enters from
  an ACTIVE generated frame, catches/rethrows through C++, runs destructors,
  clears pins/staging and executes native calls again after GC and flush.
- Trace-head GC checks acquire both color-GC fields and both GC2 cadence fields,
  matching x64's `total >= threshold || alloc_since > hard_check` behavior.
  The admitted boxed-pointer `CALLXS` trace proves both equality-skip and
  strict-overdue paths without flushing sub-batch debt or starting stopped GC.
- ARM64 generated table barriers call the existing GC2 runtime helper, matching
  x64. A dedicated machine-code fixture covers key roots and argument-register
  overlap on ARM64 and arm64e/BTI. General table-effect admission remains closed.
- The common assembler marks the loop-entry boundary before emitting PHI spill
  copies, preventing ARM64 store pairing across that branch target. The native
  generated-code regression covers both ARM64 and arm64e/BTI and is required
  by the admitted NUM pressure cases.

## Deliberately closed or incomplete

- General ARM64 JIT IR admission for arbitrary Lua programs, unrestricted
  spill or register layouts, and general root geometries.
- General table and upvalue JIT reads, writes and traversal, plus sunk/duplicate
  allocation and raw initialization effects. Rooted scalar/GC reads,
  existing-key scalar/GC stores, bounded ordinary/raw insertion, closed closure
  upvalues, root-frame closed-cell CGET/CSET and unsunk TNEW have operation proofs.
  Other local-cell geometries, open upvalues, closure/cell creation, other insertion/call geometries
  and general metamethod
  paths remain closed. Bounded nil
  deletion now emits a rooted effect even when the recording sample is absent,
  replacing the earlier raw-store recording veto described in the
  [raw nil-store regression](arm64-raw-nil-store-2026-10-25.md). Forwarding-safe
  interpreter `lj_vm_next` does not by itself open other JIT heap paths.
  The historical
  [`GC-value rooting audit`](arm64-gc-value-rooting-audit-2026-10-25.md)
  links to the implemented rooted-value lifetime proof and its limits.
- General side traces, side-of-side traces, stitches, and production JIT
  side-exit unwind resolution.
- ARM64 CALLXS aggregates/sret, unproven variadic cdata forms, vectors, complex results, general
  multires/tail calls, CALLM outside the direct scalar producer shape, and
  unsupported result/IR representations. The admitted scalar
  float conversions do not certify arbitrary float graphs. Boxed I64/U64
  results and fixed owned arguments have narrow certificates with passing
  four-profile and combined validation at `f0c4f574`. General 64-bit cdata
  arithmetic, references, PHI arguments and variadic suffixes remain excluded.
  See the [boxed-result note](arm64-jit-boxed64-results-2026-10-26.md) and
  [fixed-argument note](arm64-jit-i64-arguments-2026-10-26.md).
- Full parity with every x86_64 lockless VM, JIT, FFI, profiler, unwind, and
  stress path.
- Concurrent Darwin profiler start/stop can still terminate on a fresh late
  kernel SIGPROF after the saved disposition is restored. Discarding signals
  already pending does not close that producer race. This reproduces on the
  preceding clean checkpoint and is excluded explicitly from the callback
  contract's claimed coverage; see the callback notes.
- The normal development build's repeated upvalue regression passed 110
  processes, then one process timed out after 120 seconds. Subsequent unchanged
  runs passed 300 unbuffered and 1,000 buffered processes without reproducing
  the original recorder assertion or the timeout. The timeout's stage and cause
  remain unknown; these later passes do not explain it.
- Complete sanitizer, weak-memory, sustained-concurrency, and performance
  recovery matrices.
- Legacy bytecode imports now normalize to shared cells before publication.
  Native interpreter compatibility covers genuine stock v2 imports, debug
  metadata, worker sharing, and v4 round trips. Conversion retains explicit
  frame/jump/window limits. Both 509-case stock corpora and targeted native
  closure/import tests now pass on x86-64; unrestricted original-bytecode
  compatibility is not yet established. See
  [`legacy-bytecode-cells.md`](legacy-bytecode-cells.md).
  True internal legacy
  open cells still require the admission veto.
- Concurrent tmpbuf stress showed one unchanged-baseline timeout. A rejected
  inline dirty-epoch experiment also hit a GC2 assertion; no common cause or
  specific upstream fix has been established.

## Minimal-divergence review

Across the earlier and current minimal-divergence checkpoints, touched code was
compared with the `v2.1` control flow and style. The reviews removed only
divergence that was not part of the lockless ARM64 contract:

- restored unchanged build options and non-ARM behavior where earlier work had
  widened a target guard;
- kept the large ARM64 admission policy in `src/lj_asm_arm64_admit.h` instead
  of growing shared assembler control flow;
- kept semantic and post-register-allocation certificates independent even
  where merging them would reduce line count;
- hid test-stage constants behind `LJ_TRACE_TEST_HELPERS` and removed a
  redundant exported side-validator test wrapper;
- normalized DynASM indentation, switch layout, comments, stale names, and
  redundant nested ARM64 conditions; and
- added small target-local address-plus-`LDAR` primitives for VM and JIT
  publications rather than duplicating acquire sequences or changing common
  code. Source and generated-object contracts pin every converted consumer.

The large ARM64 lifecycle sections in `src/lj_trace.c` were reviewed but not
moved. They span recording, publication, retirement, and unwind lifecycle, and
many existing contracts inspect that file directly. Extracting only part would
split one subsystem and create more maintenance divergence than it removes.
Any future extraction should be a separate mechanical change that moves the
whole lifecycle and updates the source contracts without changing behavior.

## Verification checkpoints

- Complete safe ARM64 bootstrap and experimental fail-closed gates passed on
  frozen `709583ee` source on 2026-10-22. This includes the new aggregate ABI
  and generated table-barrier contracts. It does not certify subsequent edits.
- Legacy admission changes passed 387 safe ARM64 stock tests and 509 tests each
  on experimental ARM64 and x86-64/Rosetta, plus the focused lifetime, admission
  race, and threading fixtures described in the corresponding note.
- `tools/ci/arm64_meta_publication_contract.sh`: source and generated-object
  checks pass at `3f109fa2`, including every new unconditional VM acquire.
- Safe ARM64 runtime smoke at `f1fe1f73`: `jit.arch == "arm64"`,
  `jit.os == "OSX"`, `jit.status() == false`, and 64-bit FFI pointers.
- `tools/ci/arm64_jit_fail_closed_gate.sh`: complete pass at `f1fe1f73` in
  about 164 seconds, including ordinary ARM64 and arm64e/BTI publication,
  entry, exits, retirement, flush/reuse, first-side paths, safepoints,
  forwarding-safe `lj_vm_next`, callback-result lifetime, interpreter and JIT
  GC2 contracts, 3,072 compiler combinations, 37 admitted modes per process,
  and 222 default runtime mode executions.
- Exact ARM64 `CALLXS` scalar/lifecycle suite: focused pass at `f1fe1f73` for
  the integer, double, and boxed-pointer certificates, including the native
  GC2 equality/overdue cadence proof. This suite is not part of the
  fail-closed umbrella above.
- Native experimental ARM64 vendored suite: 509 passed at `4d1b6126`.
- Disposable x86_64/Rosetta vendored suite: 509 passed at `4d1b6126`.
- Safe ARM64 no-JIT stock suite: 387 passed at `e2c8778d`.
- ARM64 bootstrap gate: passed at `9044cac5`, including the stock suite,
  TG/root/safepoint/protected-call contracts, threading/coroutines, and 320
  FFI callback rounds across four threads, plus ordinary ARM64 and arm64e/BTI
  interpreter GC2 hard-assist proof.

Known diagnostics observed during validation include the diagnostic `topofs`
warning and the x86_64
builder's expected preliminary non-GC64 rejection before its configured GC64
build.

## Review implementation and future capabilities

The review's concrete changes and evidence are tracked in
[`review-implementation-2026-10-25.md`](review-implementation-2026-10-25.md):
operation/effect admission, root/child entry certificates, numeric adaptation,
native coverage, shared build profiles and measured progress contracts.
Further ABI classes, raw heap effects and arbitrary child topology need their
own semantic, allocated-layout, execution and lifecycle proofs. Keep common
changes narrow and retain stock LuaJIT control flow wherever the lockless
protocol permits it.

## Notes layout

- [`README.md`](README.md): current implemented, verified, and fail-closed
  status.
- [`architecture-constraints.md`](architecture-constraints.md): durable
  correctness and minimal-divergence rules.
- [`legacy-upvalue-admission.md`](legacy-upvalue-admission.md): exact open-cell
  admission authority and its compatibility boundary.
- [`arm64-ffi-aggregate-snapshots.md`](arm64-ffi-aggregate-snapshots.md): aggregate
  classification publication and native ABI evidence.
- [`arm64-jit-tbar.md`](arm64-jit-tbar.md): generated GC2 table barriers.
- [`arm64-jit-scalar-capability.md`](arm64-jit-scalar-capability.md): independent
  scalar certificates and native arithmetic/PHI/lifecycle evidence.
- [`arm64-vm-epoch-audit-2026-10-22.md`](arm64-vm-epoch-audit-2026-10-22.md):
  rejected performance experiment and unresolved stress observation.
- [`gc2-idle-table-rescan-close-2026-10-25.md`](gc2-idle-table-rescan-close-2026-10-25.md):
  intermittent SWEEP close assertion from IDLE table-store reservations and
  the targeted next-cycle SSB handoff repair.
- [`archive/aarch64-checkpoints/`](archive/aarch64-checkpoints/): 76 historical
  ARM64 investigation and checkpoint notes.
- [`archive/pre-aarch64/`](archive/pre-aarch64/): 847 earlier LuaJITMT notes.
- [`archive/complaints/`](archive/complaints/): 15 historical environment and
  test-runner reports.

## Primary checks

```sh
tools/ci/arm64_meta_publication_contract.sh
env MACOSX_DEPLOYMENT_TARGET=13.0 tools/ci/arm64_jit_fail_closed_gate.sh
env MACOSX_DEPLOYMENT_TARGET=13.0 \
  LUA_PATH='tests/lib/?.lua;src/?.lua;src/jit/?.lua;;' \
  src/luajit tools/test.lua m7_ffi_callxs_arm64_scalar
env LUA=src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet
```

Use focused contracts while developing and the complete fail-closed gate
before making a checkpoint claim. Keep source, proof, and documentation in
separate validated commits, and push each completed slice.

Read [`architecture-constraints.md`](architecture-constraints.md) before
changing VM, JIT, FFI, GC2, publication, or cleanup paths.
