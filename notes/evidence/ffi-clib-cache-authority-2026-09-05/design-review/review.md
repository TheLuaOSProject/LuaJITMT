# CLibrary cache and semantic-close guard: source-only review

This is a review of the proposed design, not an implementation or runtime validation. The source basis is the committed method-guard repair plus the immutable five-line receiver patch: lj_record.c SHA-256 07116bc933781976c91453a9ca89a46aef4a81d80bf71ab2f6e5269383fcce87 and lj_crecord.c SHA-256 d9117ff3214f258cc84648725effcd498f3d1e93a774a94efd65e81ed11f2bff. reviewed-inputs.json freezes the relevant source identities in the existing private receiver tree. No shared source or old evidence was changed.

## Verdict and smallest sound form

No additional lifecycle, cache-refill, or extern-ordering blocker was found. The proposed lookup/value/lifecycle sequence is sound in principle with two corrections: emit a real guard from the actual returned TRef rather than comparing two stale recorder snapshots, and preserve the actual loaded numeric result instead of synthesizing a constant that loses signed zero. These are implementation requirements, not optional optimizations.

A minimal sequence is:

1. Keep the builtin-local typed KGC receiver identity guard and the symbol-name guard. Retain the immutable original cache_env table as a KGC constant; create the key and expected cache-value constants while the existing anchors retain their source values.
2. Record a raw indexed read of that table with `ix.val=0` and `ix.idxchain=0`. This must not consult the cache table's own __index method. Preserve the existing current-snapshot/cache admission checks as recorder eligibility checks.
3. From the returned TRef, force the desired equality/type guard against the retained cache value. Cdata uses raw object identity against its KGC constant. Numeric values need valid number/int normalization or a conservative recorder abort on incompatible representation. Known unequal constants must abort through the normal false-guard path. Do not interpret a helper's unguarded refusal output as a value.
4. Emit a signed 32-bit volatile lifecycle read and a signed >=0 guard after the table read and its result guard, before exposing the specialized result or recording the extern access.
5. For a constant-value symbol, return the actual guarded numeric TRef. For functions/externs, retain the existing exact cached-cdata identity and CType conversion paths. The namespace and relevant cdata constants remain trace roots.

This does not exempt ordinary shared-MT metamethod lookup from its existing refusal. Direct captured builtin calls can retain the existing shared-table helper path, with the waiting limitation described below.

## TRef guards and numeric semantics

`lj_record_objcmp` derives its desired comparison from the supplied TValue snapshots. It returns 2 on incompatible IR types without emitting an equality guard. When both operands are constants, it trusts those snapshots and skips IR emission altogether. `lj_record_idx` can independently resample, forward a known store, or return a constant. Passing two earlier equal environment/cache snapshots after that operation does not establish that those snapshots describe the returned TRef.

Force the guard from the actual IR operands and explicitly handle type mismatches. This avoids the both-constant fast-path problem and lets the normal IR constant folder reject unequal constant operands. If objcmp is retained, its inputs must accurately describe both TRefs, including reconstructing actual constant TValues, and its nonzero return cannot be ignored. Earlier equality is only permission to attempt recording; it is not the runtime proof.

`lj_obj_equal` uses numeric equality, not bitwise TValue identity. A legitimate original-cache override of enum zero with -0 compares equal to the cached +0. If generated code then returns a newly synthesized +0, `1 / lib.ZERO` has the wrong sign. Returning the guarded loaded numeric TRef preserves -0 without inventing bitwise floating-point comparison machinery. Alternatively, a true bit-identity guard may reject that override and side-exit. NaN overrides must not satisfy an equality guard to a finite cached constant. Large unsigned 32-bit constants also need their actual numeric representation preserved. No cdata __eq metamethod should be involved in cache identity.

A recorder-time mutation between eligibility sampling and the new indexed read can yield a different type or known constant. That must fail closed as a recorder abort or a real guarded runtime mismatch, never as a comparison skipped because earlier snapshots happened to match. This requires a deterministic recording-race control in the eventual implementation, especially with assertions enabled.

## Original cache environment and recorder lifetime

The immutable edge is the cache_env pointer, not the table's contents. `lj_clib_cache_env_rel` is called only by clib_new, before publication of UDTYPE_FFI_CLIB. `lua_setfenv` modifies GCudata.env instead. Both interpreter namespace lookup and the recorder must continue using the original cache_env after debug.setfenv replaces the visible userdata environment. Semantic close detaches cache_head and handle but does not clear or redirect cache_env.

The receiver KGC retains the namespace, whose GC traversal follows cache_env. A KGC for cache_env gives generated lookup an explicit stable root. The symbol string remains a Lua argument root until it becomes an IR constant. Existing envtv/tv anchors retain copied cache values across fallible recording; anchor storage uses linked blocks and does not relocate existing slots when another block is allocated. Create cdata/number constants while those anchors remain live and pop them only after all new fallible operations and result construction are finished. The current trace is an enumerated KGC root.

For the widened anchor scope, use L2TG(J->L), matching rec_idx_mt_shared_sample and lj_vm_cpcall's rollback checkpoint. The old recff_clib_index J2TG macro resolves through the physical fallback; extending that dependency is unnecessary when the protected-call owner TG is explicit. Normal returns pop in reverse order. Recorder errors and STOPREQ must restore the same anchor stack. Do not retain raw table-vector/cache-entry slot pointers or acquire a new lease from an unrooted old snapshot.

The proposed table lookup does not invoke Lua, but its shared helper can retry and may unwind. New reads of stack-backed arguments after a potentially relocating operation should use current J->L->base or restored offsets instead of assuming an old rd.argv address remains current. This is separate from the stable anchor pointers. The generated guard operates on TRefs, so it does not need to export a stale C pointer to the latest table slot.

## Lifecycle proof and extern ordering

CLOSING is bit 31 of a uint32 lifecycle field; the other 31 bits count admitted readers. It is sticky: close ORs the bit and no path reopens the namespace. A signed INT load guarded >=0 accepts every legitimate reader count and rejects every closing value. An unsigned >=0 test would be vacuous.

Use VOLATILE alone on a mutable address derived from the retained userdata. Do not combine it with READONLY, whose forwarding path takes precedence, and do not use KKPTR, whose contents can be folded at recording time. Adding an offset to a userdata KGC normally yields mutable KPTR, which is appropriate here. In lj_opt_fwd_xload, a volatile load bypasses load forwarding/CSE; a real dependent guard keeps it live under DCE and on copied loop iterations. Inspect final IR and machine code to confirm the actual 32-bit read remains after the helper and before result use.

After a successful environment read, a later open lifecycle observation implies the namespace was also open at the earlier read, since closing is irreversible. A close that happens while the helper waits is rejected by the following lifecycle guard. A close that happens after that guard may race the already-admitted lookup. This matches the existing interpreter split: lj_clib_index releases its reader and checks closing before ffi_clib___index/newindex performs the extern conversion or read/store.

Physical handles already remain retired until joined-world trace teardown, so a close after the lifecycle sample cannot unload the embedded extern/function address. The new guard does not need a reader count merely to access the retained environment/result constants; it does not traverse mutable side-cache links in native code. Retaining the namespace alone does not mean it is semantically open, hence the per-operation volatile guard remains necessary.

Place the lifecycle guard before the extern store and preserve existing needsnap handling. A failed cache/lifecycle guard must not perform the old extern write. A nonvolatile extern load may be reused by normal optimization, but the fresh lifecycle guard must still prevent exposing a result from an already-closed namespace. The existing volatile-CType rules remain separate from the lifecycle field's volatility. Guards must not replace them.

## Nil refill and overrides

A nil or missing original-cache entry must side-exit rather than silently reuse the retained old symbol. Interpreter fallback can then enter the namespace reader, reuse the side-cache entry, and republish it into the original environment. If another writer supplies an override during refill, the existing clib_env_publish protocol selects the winning current value. False, function, table, and different-cdata overrides fail the old native value guard and follow normal interpreter behavior. The same-value case may continue if its exact relevant semantics are preserved, including signed zero for numeric results.

Replacing the userdata environment remains irrelevant to namespace lookup. Adding a metatable to the original cache table must not add metamethod dispatch, because clib_env_get is raw. Setting cache_env entries to nil is not itself semantic close; only lifecycle determines that state.

## Shared helper, snapshots, and nonwaiting choices

`lj_record_idx` in shared MT calls the existing lj_tab_gettv_rooted helper through its CCI_L|CCI_T side-effecting IR entry. That helper retains exact source/key/result objects, resolves the current generation, releases leases before retry, may wait at lj_tab_wait_l, and can throw STOPREQ. The new use adds a waiting generated call site even though it adds no new wait primitive or lock. This is a valid stability-first option if stated explicitly; it is not a fully nonwaiting lookup.

rec_idx_mt_shared_load creates a post-helper snapshot before VLOAD. That established rawget-style path is a relevant template: at this point the recorder and interpreter still describe the fast-function frame, unlike the separate post-CALLXS case after lj_record_ret has already switched IR state to the Lua caller. The cache identity and lifecycle guards should use that post-helper state. The lookup has not yet executed a user callback or extern store, so restarting the builtin lookup after guard failure is acceptable; preceding Lua effects must not replay. Tests must also cover direct captured __newindex because its later extern store is observable.

An existing bounded admission option is lj_tab_gettv_rooted_hit_try. It attempts owner/SMR/leases once, leaves cells unchanged on miss/refusal, and returns 1 only after publishing a retained hit. It is currently used by interpreter metamethod fast paths and has no IR-call registration. Reusing it requires explicit recorder/IR-call plumbing, authoritative enumerated TMPREF inputs/output, a success-status guard before loading the result, and the same cache/lifecycle guards. Its successful GC-result publication may grow GC queue storage after SMR closes; one-shot admission is not by itself a proof that the entire publication path is allocation-free or universally nonblocking. The native ABI, failure/throw behavior, and snapshot placement need a separate review if this option is chosen.

The simplest narrower nonwaiting variant would refuse direct CLibrary recording once the runtime is shared and use raw pre-MT guards only, but that removes existing direct-call functionality. It is not required for the correctness of the existing rooted-helper option. Do not instead bypass shared-table admission with raw HREF/HLOAD reads merely because cache_env itself is immutable.

## Required implementation evidence

The design is not approved as tested until the eventual source is available. High-value controls are: original environment override/deletion/refill/resize and replacement-env irrelevance; enum signed-zero/NaN/large unsigned values; different cdata and callable overrides; direct and ordinary extern reads/writes with exact no-replay counters; actual old root and side execution after semantic close; close and STOPREQ while a real shared-table helper waits; recorder type/value changes between samples; anchor rollback and full GC of replaced environment/cache values; and actual volatile lifecycle reads in root and side machine code. Normal, assertion, and ASan runs should use the final source identities. No such new runtime runs or source edits were performed for this source-only handoff.
