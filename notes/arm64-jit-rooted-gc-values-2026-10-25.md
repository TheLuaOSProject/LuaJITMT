# GC-valued rooted table operations on ARM64

This tranche extends the existing operation-based root certificates to visible
GC TValues: string, table, function, thread, userdata and opaque cdata. Root
LOOP/FORL traces can load these values through rooted table reads, carry them
across later helpers, restore them from snapshots, and store them to existing
keys. Numeric and boolean results can also be read through GC identity keys.
This is not general ARM64 JIT parity.

## Lifetime mechanism

Registers and native spills hold untagged GC pointers and are not scanned as
TValue roots. The proof uses two existing runtime mechanisms together:

1. `tab_gettv_rooted_try_impl` retains and confirms the exact table, key and
   result, publishes the complete result TValue, and calls `lj_gc_pubroot`
   before releasing the result lease. GC2 marks/rescues that object in the
   current MARK/WEAK/SWEEP phase; generational IDLE uses its remembered set.
2. `lj_tg_any_jit_active` checks every registered live TG, including blocked
   executors. GC2 closes the entry gate with the matching SC fence before
   sampling this state. MARK root snapshot/fixpoint/closure, WEAK transitions,
   IDLE cycle/reclaim admission and SWEEP physical reclamation defer while
   native state remains published. Ordinary table helpers and their retry
   waits keep this lease active. `lj_snap_restore_exit` restores tagged Lua
   stack roots before the deferred-GC or VM exit path clears `jit_base`.

Thus a later helper can overwrite the scratch result while the typed pointer
remains live in a register or spill. Publication protects the current cycle;
the native phase gate prevents a later phase/reclamation from outrunning its
exit restoration. No new per-value persistent root allocator or stack map is
introduced. FFI/native calls that change the phase lease retain their separate
frame/root contracts and are not opened by this certificate.

The source authorities are `lj_gc.c:lj_gc_pubroot`,
`lj_gc2.c:gc2_jit_phase_gate_close`, `gc2_mark_root_snapshot`,
`gc2_mark_close_help`, the SWEEP reclaim gates, `lj_tg.c:lj_tg_any_jit_active`,
and `lj_trace.c:trace_exit_cp`/`lj_trace_exit`.

## Runtime and compiler changes

- GC-valued reads use bounded `lj_tab_gettv_rooted_hit_try`, an immediate
  success guard, a post-helper snapshot and a typed guarded VLOAD. Numeric
  and boolean reads keep the optimized scalar helper for string, numeric and
  boolean keys. Other visible GC keys use the generic bounded reader, whose
  exact key lease authorizes hashing. Both admission passes separately reject
  pairing such a key with the scalar-only helper.
- `lj_tab_storegc_rooted` stages table, key and value in three distinct
  TG-local TMPREF slots. New input-only IN3 addresses `tmptv3`; ARM64 and x64
  emitters materialize the complete TValue, with nil initialization, GC2 root
  enumeration and recorder event scratch preservation.
- The store reserves three empty anchor slots before acquiring SMR/leases,
  confirms all input cells and owner state, publishes three anchor copies,
  ends the temporary retaining interval, and uses the existing forwarding/CAS
  store and write-barrier protocol. Reserve failures pop prior slots; protected
  errors roll back the owner anchor checkpoint. GC identity keys retain exact
  leases and are accepted by both scalar and GC store wrappers.
- Semantic and immutable post-RA passes independently check helper IDs, CARG
  order, latest scratch definitions, IN1/IN2/IN3 modes, typed value provenance,
  guards and post-effect snapshots. Allocated checks also require distinct
  scratch registers and complete aligned GC spill spans with nonoverlapping
  ownership. GC PHI inputs and visible GC constants retain normal LuaJIT
  representations; raw pointer/prototype TValue impostors are excluded.
- Side traces retain their previous scalar boundary. CALLXS, raw heap IR and
  arbitrary calls are not admitted through this root operation grammar.

## Validation

The focused reproducible check is:

```sh
JOBS=2 MAKE_JOBS=2 sh tools/ci/arm64_jit_rooted_gc_contract.sh
```

It builds assertion and release profiles for ARM64 and arm64e/BTI. It includes
direct helper handoff/GC/resize/errno/three-reserve-failure/error rollback;
native six-type read/store loops; GC PHI returns and identity keys; linked
table traversal; 18 simultaneous mixed GC values and 32 GC parameter sources;
independent authentic trace mutation fixtures; and a deterministic native
collector handoff with the source edge removed after scratch overwrite.

The complete focused contract passed all four profiles. The pressure traces
contained 70 and 31 GC spill locations and 66 and 36 rooted GC stores; each
matrix completed 20 repeated native loop exits. Mutation tests independently
rejected helper/root/mode/order/type/guard/snapshot corruption, aliased scratch
registers, unaligned GC spans and a 32-bit status overlapping a GC spill's
upper half. The lifecycle hook counted actual generated calls for every GC
type. Its peer removed the source edge after a subsequent read overwrote the
result scratch, requested a major cycle and proved IDLE entry was deferred
with the request retained and the native gate closed. The resumed trace
stored the same object, exited, survived collection/reentry, and the weak
witness cleared after the last strong edges were removed.

Log: `/tmp/ljmt-gc-contract-development.log`. The ordinary scalar store matrix
also passed during development. The first full regression at `5bf113a4`
passed the rooted GC, table allocation, side-entry, numeric, exit and live-flush
contracts, then stopped in the first CALLXS admission fixture. The shared
recorder had also changed the cdata `__call` FUNC lookup to the bounded helper,
breaking its separately certified FFI graph. An explicit recorder mode now
keeps that lookup on its existing full helper and snapshot sequence; ordinary
table reads retain the new bounded helper. The CALLXS admission rules are
unchanged. The failed run is `/tmp/ljmt-gc-experimental-5bf113a4.log`.

The repaired ARM64 assertion profile passed the complete
`m7_ffi_callxs_arm64_scalar` suite, including independent int/double/pointer
admission, stack ABI, C++ callback unwind and integer/double/pointer lifecycle
checks. Log: `/tmp/ljmt-gc-ffi-lookup-fixed.log`.

The scalar-result/GC-key extension passed the full rooted GC contract in all
four profiles, alongside the earlier GC lifetime, spill, mutation and raw nil
deletion regressions. Its Lua matrix covers all six GC key classes with INT,
NUM, true and false results, same-type numeric replacement, collection and
native reentry, plus a key supplied by an earlier rooted VLOAD. Each warmup
requires one published root, no aborts and 19--21 native loop exits across
20 calls. Separate semantic and post-RA mutations reject the wrong helper,
root modes/CARG ordering, wrong status guard/constant, unsupported result
types, missing type guard and broken result snapshot. Log:
`/tmp/ljmt-scalar-keys-contract.log`.

Full regression validation remains pending after this repair. These focused
results do not establish overall port completion or unrestricted FFI/side-trace
support.

## Remaining boundary

New-key insertion, nil/lightuserdata result/value classes, general metamethod
paths, raw table/upvalue heap IR, sunk initialization, heap-effect side traces
and general FFI combinations still need their own proofs. The optimized scalar
read helper retains its numeric/string/boolean key domain; generic bounded
reads cover the other visible GC identity keys. The full port also
retains the broader parity, stress and performance work listed in README.
