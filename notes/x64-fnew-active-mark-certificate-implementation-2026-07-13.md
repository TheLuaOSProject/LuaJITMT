# x64 active-MARK FNEW certificate implementation (2026-07-13)

This implements the low-risk C-seeded certificate design audited in
`x64-fnew-active-mark-certificate-audit-2026-07-13.md`.  The implementation was
developed and measured against exact base
`b5b7a88db14e076be7fd2ec48114962eff70a169`.

The optimization applies only to the existing x64 one-immutable-numeric-
upvalue FNEW inline template.  It does not broaden the template's pre-MT,
zero-GC-worker, arena-allocator, or descriptor eligibility.  Outside active
MARK its semantics and admission rules are unchanged, although moving the
single environment load earlier and keeping its register live changes code
scheduling and register pressure.  During cooperative active MARK, the first
allocation for an exact `(cycle, prototype, environment)` tuple calls C; later
allocations may use the existing rootless typed inline constructor.

## Safety model

The cache is comparison authority only.  It is never a root, mark bit, or
replacement for collector work.

Each `TGState` has a one-entry non-root cache:

- `fnew_cert_pt`;
- `fnew_cert_env`; and
- `fnew_cert_cycle`, where zero is always invalid.

The traced C constructor performs its normal function/environment,
function/upvalue, and upvalue-payload barriers first.  It then attempts an
allocation-free seed with `lj_gc2_fnew_certify_pair_nodrain()`.  A seed is
accepted only when all of these are simultaneously true:

- the supplied TG is the current TG for `g` and owns its active SSB node;
- phase is exactly `LJ_GC2_MARK`;
- the TG barrier mirror is active and allocations are black;
- the JIT phase gate is open;
- the current cycle is nonzero and equals `jit_mark_resume`; and
- the current owner SSB has no remembered suffix, its published base/end pair
  is exactly the active node's full slot array, its cursor is bounded by that
  pair, and it has two adjacent slots available.

On success the seed publishes the exact prototype and environment as two raw
SSB traversal requests.  Runtime publication order is:

1. store the prototype slot;
2. store the environment slot;
3. release-advance the owner cursor once by two slots;
4. invalidate the cache cycle;
5. update the comparison pointers; and
6. release-publish the current nonzero cycle.

A malformed owner tuple, remembered suffix, full SSB, or one-slot SSB does not
rotate, drain, allocate, enter recovery, advance the cursor, or publish any
cache authority.  The already completed ordinary C barriers protect that
allocation and the next trace iteration calls C again.  The cache may remain
usable after ordinary SSB rotation/drain because the durable traversal request,
rather than the cache entry, is the liveness edge.

Every MARK activation invalidates `fnew_cert_cycle` before enabling the TG
barrier mirror, while native entry is closed.  Generated code also compares the
cache cycle with the current nonzero cycle and `jit_mark_resume`.  Consequently
stale pointer fields, cycle wrap, aborted cycles, or remotely reset authority
cannot authorize construction.

## Generated-code predicate

The active-MARK inline arm requires, in runtime order:

1. `mark_active != 0` (inactive marking skips the certificate checks);
2. phase exactly MARK;
3. black allocation enabled;
4. JIT phase gate open;
5. current cycle nonzero;
6. `jit_mark_resume == cycle`;
7. cached cycle equal to that same cycle;
8. cached prototype equal to the trace constant; and
9. cached environment equal to the parent environment captured at entry.

Every mismatch branches to the generic constructor before any typed lifetime
claim or partial inline object initialization.  No object mark, NEEDSCAN bit,
or approximate FFI/C-call shape carries authority.

The parent environment is loaded once before the predicate.  The exact same
register is compared with the certificate and installed in the new closure.
If a permitted racy `setfenv` changes the parent environment before capture,
the old certificate misses; if it changes after capture, the closure still
uses the exact value that was validated.  There is no second parent-environment
load after validation.

Adding this live environment register exposed a real assertion-build sparse
mcode-limit overflow in the pre-existing eight-transform packed pair transition
(193 bytes between checks).  The helper now checks the red-zone between its two
exact packed-lane transforms.  This changes no emitted runtime instruction or
control-flow semantics; it only keeps the assembler's limit proof bounded.

## Deterministic coverage

`t-jit-fnew-bump.c` now checks both publication and executable code:

- a correctly shaped foreign TG cannot append to an owner SSB;
- production-valid zero- and one-slot owner boundaries leave the cursor, slots,
  and cache untouched, while exactly two remaining slots publish adjacent
  prototype/environment entries and advance the cursor once;
- the mcode decoder proves the full predicate order, one common fallback, one
  environment capture, and reuse of its register for the closure store;
- a real cooperative MARK cycle reaches a completed persistent root snapshot
  (`mark_root_scanned == 1`), drains the old snapshot work without running a
  second fixpoint, reopens the gate in MARK, and has exactly one C miss for a
  new exact pair followed by zero helper calls for hits;
- the first post-snapshot pair contributes real barrier work that prevents a
  one-item close attempt, proving the certificate's SSB entries remain part of
  collector progress rather than mere comparison metadata;
- a test-only no-call mcode pause after the environment comparison lets a
  pthread release-publish a new parent environment before the closure store;
  the paused closure retains the already validated environment, and the next
  allocation has exactly one C miss and reseeds for the new environment;
- a changed environment and a changed prototype each force exactly one miss
  and reseed;
- a deliberately stale prior-cycle cache forces exactly one miss in the next
  cycle; and
- after ordinary roots are removed and the cycle completes, the surviving
  closure is invoked successfully, still identifies the exact certified
  prototype, and reaches the expected custom-environment token.

The SSB boundary fixture preserves the production base/end tuple and fills
preceding positions through legitimate one-entry cursor publications.  It does
not shrink the node end pointer or jump the cursor over stale storage, so its
zero/one/two-slot cases exercise only valid queue states.

The capture-to-store pause machinery is compiled only with
`LJ_FUNC_TEST_HELPERS`.  Release builds contain neither the probe nor its test
state, so it adds no production mcode branch or synchronization traffic.

Final focused gates passed:

- `m6_jit_fnew_bump`;
- `m6_jit_gc2_readiness`;
- `m3_gc2_recovery`, normal and assertion/paranoia builds;
- `m3_gc2_worker_scheduler`;
- `m6_jit_gcworkers_activation_flush`;
- direct assertion/paranoia FNEW runs in default, publication-only, and
  `LJ_TEST_TRACED_FNEW=1` modes;
- focused ASAN FNEW runs in all three modes; and
- focused UBSAN FNEW runs in all three modes.

The broader `m3_gc2_paranoia` stock-JIT, no-JIT, and default-restore matrix had
also passed before the final owner/boundary tightening.  The tightening was
then covered directly by the final assertion/paranoia and recovery runs.

## Performance

The final comparison used exact-base and candidate default builds, pinned to
CPU 8, with three alternating pairs per condition and
`BENCH_SCALE=.5 closures_upval`.  Each reported value is the harness best of
five internal runs.

| condition | exact base (ns/op) | candidate (ns/op) | median change |
| --- | --- | --- | ---: |
| GC active | 298.56, 296.30, 297.93 | 295.89, 296.66, 296.87 | -0.43% |
| GC stopped | 86.59, 87.93, 88.70 | 87.64, 89.54, 89.75 | +1.83% |

The active-GC candidate median is 296.66 ns/op versus 297.93 ns/op at the
exact base.  That sub-1% difference is effectively neutral, not evidence of a
throughput win.  The correctly stopped-GC candidate median is 89.54 ns/op
versus 87.93 ns/op.  The active-MARK capability therefore adds no material
ordinary workload regression; the stopped delta is low single digits and
consistent with the preliminary shorter alternating run (which measured a
sub-1% delta).  This is a capability/safety tranche, not a claimed throughput
optimization.

This tranche changes no plan file and introduces no platform-harness complaint.
Linux/x86-64 is the tested release scope here; macOS and Windows remain deferred
under the current b1.2.0 release direction.
