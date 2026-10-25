# Rooted nil results without a table metatable

Ordinary experimental ARM64 `LOOP` and `FORL` roots now admit nil table-read
results when a bounded helper proves raw absence and then acquire-loads a
NULL table metatable. This includes nil and NaN keys, which are ordinary read
misses but may still invoke `__index` when a metatable exists.

`lj_tab_getnil_nometa_rooted_try` reuses the exact table/key leases, SMR
interval, paired-generation resolver and root/owner confirmation of the
bounded generic reader. It does not allocate or wait. It publishes nil only
on success, including aliased input/output roots. A raw hit, non-table,
non-NULL metatable, forwarding, retiring generation, KEYLOCK or admission
refusal returns zero without changing any cell. In particular, it does not
reuse the legacy public resolver's FORWARD-to-ABSENT mapping. Raw absence
followed by the metatable acquire follows interpreter lookup ordering; this
does not invent a combined table/metatable epoch.

The ARM64 recorder probes that same helper while its original table/key
anchors remain valid. Only a successful nil/no-metatable sample bypasses
metamethod sampling. Generated execution uses distinct TG-local IN1/OUT1
and IN2 roots, a helper-status guard, a post-helper snapshot and an exact
guarded NIL VLOAD. No emitter changes are needed. Other targets retain their
existing recorder path, and the full/hit reader modes keep their previous
contracts.

Semantic and allocated admission independently validate the helper geometry,
status/result guards and snapshots. Nil values are canonical `REF_NIL`
constants with no register or spill location; dynamic nil keys retain their
own guarded SLOAD before canonicalization. Nil PHIs and nil side-trace
capabilities remain closed. Scalar/generic hit helpers and stores still reject
nil keys. CALLXS and fixed-function capabilities are unchanged.

## Evidence

The final focused run on the implementation based on `0391f47c` passed:

- `tools/ci/arm64_jit_rooted_gc_contract.sh`: ARM64 and arm64e/BTI, assertion
  and release. This includes the new direct helper and native LOOP/FORL key
  matrix, independent semantic/post-RA mutations, and all existing rooted
  scalar/GC/lightuserdata, store, lifetime and segment-map cases.
- ARM64 stock source and original-v2 bytecode import: 509 cases each.
- Signed x86-64/Rosetta: direct nil helper, full bounded rooted reader, raw
  nil-deletion regression, and both 509-case stock corpora.
- The rooted scalar-read and general IR-admission contracts also passed on
  the same runtime sources before the final fixture-only refinements.

The native matrix requires one root, no aborts, 19--21 native exits over
twenty calls, a nil helper and guarded NIL result, and no raw heap IR or nil
PHI. It covers scalar, lightuserdata and all six visible GC key classes,
nil/NaN keys, insertion, function/table `__index`, empty metatables and full
collection. Cloned traces cover LOOP table keys, FORL table keys, constant
nil keys and nil arguments. The immediate result snapshot precedes recorder
destination assignment; a later snapshot must explicitly restore the
returned local as `REF_NIL`.

Logs are `/tmp/ljmt-nil-regression-verified.log`,
`/tmp/ljmt-nil-rooted-gc-contract-verified.log`,
`/tmp/ljmt-nil-arm64-stock-{source,import}-verified.log`,
`/tmp/ljmt-nil-x64-regression-verified.log`,
`/tmp/ljmt-nil-rooted-read-contract.log` and
`/tmp/ljmt-nil-ir-admission-contract.log`.

Two preliminary runs exposed fixture issues. The direct fallback assertion
assumed dual-number integer tagging on x64; it now checks the numeric value.
An existing lightuserdata rejection case missed recording after another
case used the same prototype. Each negative case now receives fresh bytecode
and explicitly resets its hotloop settings, retaining the required positive
abort count. The failed logs remain at `/tmp/ljmt-nil-x64-regression.log` and
`/tmp/ljmt-nil-rooted-gc-contract-final.log`. No runtime-source change or
timeout increase was needed for either correction.

The build helper restored the ARM64 experimental assertion development
profile. This is focused coverage, not another combined bootstrap/experimental
gate run. General metamethod traces, new-key stores, nil deletion traces,
heap-effect side traces and unrestricted ARM64 JIT admission remain open work.
