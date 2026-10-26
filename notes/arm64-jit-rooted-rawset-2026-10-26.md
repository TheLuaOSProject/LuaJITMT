# Rooted rawset CALLs

Experimental ARM64 LOOP/FORL roots now admit exact builtin `rawset` calls
with three arguments and zero or one result. The recorder requires a fixed
`CALL` and the existing Lua caller-frame geometry. Direct C-called roots,
multires and tail-call forms remain interpreted. Function identity is guarded
when the callee comes from a native load; replacing it exits before the call.
Other targets retain their previous rawset recorder veto.

Non-nil raw assignments use `lj_tab_storeraw_rooted`, sharing the ordinary
insertion helper's three anchors, exact input validation, GC barriers,
current-generation resolution and keyed CAS. Its raw mode skips `__newindex`
for both present and absent keys. Allocation, relocation, forwarding, committed
stale-generation repair, failure and protected unwind retain the existing
rooted protocol. No lock or raw table-vector pointer escape is added. Nil
rawset uses the existing rooted deletion helper; absent/metatable combinations
may still refuse to the interpreter, which preserves rawset semantics.

A snapshot before the CALL retains the exact callee and three arguments for a
refused helper or unwind. A forced snapshot immediately after the helper/status
guard records the caller continuation and, when consumed, the original table
result. Semantic and post-register-allocation admission independently verify
both boundaries, source counts, exact rawset identity, input/result provenance
and the `SNAPCOUNT_DONE` marker excluding this continuation from side traces.
Recorder and LOOP copying preserve these rawset snapshots even when the next
call's argument setup occupies the same IR position. The retention predicate
is limited to completed rooted raw/nil-store pairs; FFI snapshot merging keeps
its existing behavior. Independent admission supplies the full call-site proof.

## Validation

The complete focused regression ran from base
`73cfc836b699f1b7230fa30c15b786ecce450729`, with binary diff SHA-256
`44e35f4b0fd764402c4364a425e2b24a97cedc92c7a97b2b0eef18384027b137`
and production-source diff SHA-256
`d97f7649df2b67a9b4c6416287694316fc3db70df2784f9c38af601d1908d496`.
The driver verified the frozen source identity after all runs. It passed:

- Complete rooted-GC contracts on ARM64 and arm64e/BTI, assertion and release.
  Each profile includes 60 ordinary/raw LOOP/FORL admission cases: ten value
  classes and five additional key classes per operation/mode. Native observers
  check the exact table/key/value inputs while JIT activity is published;
  observer counts include refused attempts. Reentry after collection and
  function/table-valued `__newindex` cases preserve the expected effects.
- Independent malformed callee, CALL opcode/count, input map, return-table,
  snapshot PC, continuation marker, root/status and allocated-location tests.
  Both original and LOOP-copied rawset boundaries survive adjacent snapshots
  at the same IR position. Nil rawset's third argument is independently checked.
- Forced native reserve refusal followed by exactly one interpreter write;
  a changed function target exiting before its effect; a throwing helper with
  no write; and a post-CAS value change forcing a later numeric-read exit/error
  without replaying rawset. Exact effect/entry/exit counters and restored root
  anchors, SMR readers, root descriptor and JIT activity are checked.
- Direct raw helper behavior for fresh, present and retained-nil keys,
  metamethod bypass, GC/lightuserdata values, invalid keys/values, errno, all
  three anchor reservation failures, and source-root removal followed by
  relocation, collection, resize and unwind. Ordinary insertion's four-anchor
  method path and stale-commit repair tests remain included.
- Sixteen growth cases in each of four production profiles, without helper
  defines: ordinary/raw, LOOP/FORL, dense array/sparse hash and number/table
  values. Each inserts 5,120 fresh source keys into 20 tables, verifies every
  value before and after collection, and observes 20 loop-completion exits.
- All four complete M7 scalar FFI suites, including independent admission,
  native CALLM chains, callbacks, errno, GC/STOPREQ, flush, POSTCALL no-replay
  and C++ unwind/recovery. The production fixture retains 24 native
  XSAVE/CALLXS pairs.
- ARM64 CAS fixtures and signed x86-64/Rosetta rooted store/read/nil/CAS and
  production FFI regressions. Stock source and original-v2 bytecode import
  each pass all 509 cases on each architecture.

The final cleanup only const-qualified the read-only `snap_pc_acq` input and
corrected a test comment's bytecode field name. Its complete diff SHA-256 is
`de987f0cbee8f795430bd205de601f59aae8aabf112f5eb9bf4cf84909219e2e`,
with production-source diff SHA-256
`10972ba5277d3ab420f223e4100fdec7f650726d4c9bf60a04867a94388af1c4`.
The accessor body is unchanged. ARM64 and x86-64 were rebuilt; ARM64 FFI and
rawset admission/exits/growth, x86-64 rooted helpers/FFI/509 source cases and
the restored ARM64 production FFI fixture passed. The new qualifier warnings
are gone. Documentation was added after validation.

Drivers and logs:

- `/tmp/ljmt-rawset-regression-final.sh`
- `/tmp/ljmt-rawset-regression-final.log`
- `/tmp/ljmt-rawset-rooted-gc-contract-final.log`
- `/tmp/ljmt-rawset-m7-{arm64,arm64e,arm64-release,arm64e-release}-final.log`
- `/tmp/ljmt-rawset-arm64-stock-source-final.log`
- `/tmp/ljmt-rawset-arm64-stock-import-final.log`
- `/tmp/ljmt-rawset-shared-regression-final.log`
- `/tmp/ljmt-rawset-const-validation.sh`
- `/tmp/ljmt-rawset-const-validation.log`

The Apple SDK compiler, deployment target 13.0, two build jobs and ad-hoc link
signing were used. The experimental ARM64 assertion runtime was restored.

An earlier draft preserved every DONE snapshot and changed an existing FFI
trace from 15 snapshots/97 map entries to 16/107. The full M7 fixture caught
that mismatch. Restricting retention to the rooted rawset pair restored the
existing certificate before the successful regression. Review also caught the
missing continuation-marker rejection; both passes now enforce it and the
private mutation test covers it.

This focused checkpoint does not replace the latest combined experimental/
bootstrap gate or establish sustained concurrent mutation, sanitizer,
performance or unrestricted x64 parity. General root/side geometries,
metamethod traces, upvalues, traversal and remaining FFI ABI classes stay
incomplete. The full ARM64 port is not complete.
