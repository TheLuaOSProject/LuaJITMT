# Rooted non-nil insertion

Experimental ARM64 LOOP/FORL traces now admit ordinary non-nil assignments
whose recorded key is absent and whose table has a positive rooted
NULL-metatable certificate. The three-input `lj_tab_storenew_rooted` helper
handles visible numeric, boolean, GC and lightuserdata values. Raw assignments
and other targets retain their previous recording paths. Existing-key helpers
remain in use when the recorded key is present.

The runtime operation reserves nil anchors for the table, key and value before
capturing or leasing the inputs. It confirms their exact source values,
publishes the anchors and drops temporary SMR/leases before allocation or
cooperative waits. Input stack addresses are restored after reservation; later
retries use anchors. GC/weak-table barriers use the existing table protocol.

Each retry resolves the current table generation. A nil-valued retained hash
node counts as semantic absence. Before a missing-key insertion, a separately
anchored `__newindex` lookup must return nil; otherwise the status guard exits
to ordinary Lua semantics. A non-NULL metatable without `__newindex` is valid
at runtime, though the recorder currently requires NULL. Present keys remain
raw assignments. This follows the interpreter's ordered raw lookup, method
decision and raw store; it does not introduce a new atomic transaction joining
the mutable metatable decision to the value CAS.

Stable FORWARD storage is repaired without inserting before the method
decision. Structural insertion is followed by current-generation re-resolution
and the existing keyed CAS. A committed old-generation CAS followed by STALE
is not treated as immediate success: the helper confirms the exact value in
the successor or reapplies the already-authorized assignment there. It never
replays `__newindex` after commit. Cleanup preserves errno and rolls back
anchors on normal failure or protected-call unwinding.

The independent semantic and allocated-state passes check distinct IN1/IN2/IN3
inputs, visible value provenance, argument chain, immediate status guard and
effect snapshots. Existing emitters and table publication machinery are used;
no raw ASTORE/HSTORE/FSTORE admission is added.

## Validation

All runs used base `d8cf5a451f4e1c7c3f2cfd0d8996e91174a40091`.
The first frozen full focused regression used binary diff SHA-256
`a017d544b5bfd07fd582153aee52aa8990f6ea43bafbb8ee71dc30e76b653b66`.
It passed:

- The complete rooted-GC contract on ARM64 and arm64e/BTI, assertion and
  release. The new matrix has 30 admission cases per profile: both LOOP and
  FORL, numeric/true/false/string/table/function/thread/userdata/cdata/
  lightuserdata values, plus array-number, hash-number, boolean, table and
  lightuserdata keys. A native observer checks exact table/key/value inputs
  under a published JIT frame. Its counts include helper refusals as well as
  completed stores; they are not all successful insertion counts.
- Independent malformed argument/root/type/status/snapshot and allocated
  register tests. Primitive booleans correctly use constant references rather
  than an assumed dynamic value producer.
- Native reentry after collection and repeated metatable changes. Each
  `__newindex` phase produces exactly 640 metamethod calls with native exits,
  so the interpreter neither skips nor replays an assignment.
- Direct fresh-key and retained-nil-node behavior, existing keys under
  `__newindex`, empty metatables, nil/NaN keys and nil-value refusal, errno,
  all four anchor reservation failures, and source-root removal followed by
  stack relocation, collection, resize and a thrown error. Weak witnesses
  become collectable after the operation/unwind releases its anchors.
- A deterministic post-CAS successor publication missing the new value and
  simultaneously installing `__newindex`. The successor receives the value
  and the metamethod is not invoked. Synthetic stable FORWARD cases decline
  before inserting when a method exists. These are controlled interleavings,
  not sustained concurrent mutation or repair-OOM evidence.
- ARM64 CAS publication/forwarding fixtures, and signed x86-64/Rosetta rooted
  store/read/nil/raw-store/CAS fixtures. Stock source and original-v2 import
  each pass 509 cases on each architecture. The restored ARM64 production FFI
  fixture still publishes all 24 XSAVE/CALLXS pairs.

A subsequent test-only addition checks growing fresh tables, rather than
repeatedly reusing retained nil slots. Its frozen complete diff SHA-256 is
`6a6eb8f52f53fc3449f90cfebf54206bed96e1f1749e2b7a73371a499cb946a0`.
The production-source diff SHA-256 remained
`89b3b4cf9b6b1a12cf9ee53a136b975286543540d4ecd4d6b4ff02c35d9b083b`
across both runs. The growth test passed separately in four production
profiles, without test-helper defines. Eight cases per profile combine
LOOP/FORL, dense array/sparse hash keys and number/table values. Each inserts
5,120 fresh source keys into 20 tables, checks every stored value before and
after collection, and observes 20 loop-completion exits. That exit bound
excludes interpreting each insertion through a per-key helper refusal.
The growth test is now included in the rooted-GC contract.

Drivers and logs:

- `/tmp/ljmt-insertion-regression-final.sh`
- `/tmp/ljmt-insertion-regression-final.log`
- `/tmp/ljmt-insertion-rooted-gc-contract-final.log`
- `/tmp/ljmt-insertion-arm64-stock-source-final.log`
- `/tmp/ljmt-insertion-arm64-stock-import-final.log`
- `/tmp/ljmt-insertion-shared-regression-final.log`
- `/tmp/ljmt-insertion-growth-profiles.sh`
- `/tmp/ljmt-insertion-growth-profiles.log`

The Apple SDK compiler, deployment target 13.0, two build jobs and ad-hoc link
signing were used. Both drivers finished successfully and the experimental
ARM64 assertion runtime was restored. Documentation was added afterward.

Uncommitted drafts exposed a recorder path blocked by the existing active-MT
metamethod veto, a lost-write case on committed/stale handoff, and test
assumptions about shared counters, valid helper substitutions and boolean IR
constants. These were corrected before the final runs. This focused checkpoint
does not replace the latest combined experimental/bootstrap validation or add
sanitizer, sustained concurrency, performance or general x64 parity evidence.
General metamethod traces, upvalue/traversal effects and unrestricted ARM64
admission remain incomplete.
