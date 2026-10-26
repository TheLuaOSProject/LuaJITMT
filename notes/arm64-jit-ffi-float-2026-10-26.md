# Fixed scalar float CALLXS

Experimental ARM64 CALLXS admission now accepts exact plain FLOAT conversions
from NUM/INT and plain NUM widening from FLOAT. This enables fixed float
arguments, results and call chains through the existing ABI classifier and
emitter. The semantic and allocated-state validators each check the conversion
source, mode, type and ABI width independently. Unproved PHI/guard/conversion
flags remain rejected.

The CALLXS grammar also admits exact plain NUM subtraction and guarded NUM
ULE/UGE with backward, typed operands. The range checks generated for float
results need these unordered-aware comparisons. This does not widen ordinary
scalar, rooted-table or side-trace admission. No emitter, native-frame or
runtime-publication protocol changed.

## Native evidence

The production Lua fixture requires twenty XSAVE and twenty CALLXS operations
across integer, double and float cases. It covers NUM rounding, an odd INT
above 2^24, float-result to double-argument widening, and a direct FLOAT result
feeding another FLOAT argument after the optimizer folds the intermediate
widen/narrow pair. Source calls use explicit locals between calls: nested
`fd(ff(x))` still has a CALLM source site and is intentionally not admitted.

The native ABI observer exercises float(float,float), nine FLOAT arguments,
and alternating nine INT/nine FLOAT arguments. Each 300-call case requires at
least 200 calls observed from an ACTIVE generated frame. The ninth float uses
a four-byte stack slot at offset zero; the mixed case places its ninth INT at
offset zero and ninth FLOAT at offset four, with an eight-byte outgoing extent.
Machine-code checks require the matching S-register stack stores, alongside
the existing integer/stack/spill checks.

Result widening precedes the leave helper and appears in the exact leave
snapshot. NaN exercises the unordered range path. Positive and negative float
overflow produce the correct upper/lower exit after exactly four source calls,
including at least two generated calls, without replaying a completed call.
Warm calls then re-enter native code. Independent cloned semantic/post-RA
graphs reject wrong conversion sources, modes, types, SEXT/ISPHI flags,
pointer inputs, register classes, and malformed SUB/ULE/UGE operands/guards.

Existing lifecycle checks also pass: register and stack C++ unwind each
observe 1,195 generated calls, then recover after collection and flush; nested
callbacks, callback errors, errno, result roots, remote GC/flush, STOPREQ and
trace lifecycle checks remain enabled.

## Frozen-source validation

Base: `e2f6b7fd742b46e7620b1870d32b13ffc64e0575`.
SHA-256 of `git diff --binary HEAD` before documentation changes:
`8e31507dfa0c263662132f078e1fe6cb9d6d1b47a4cd78e5d2d0f1e04b31fffe`.
The five changed runtime/test files were frozen throughout validation.

The focused final M7 run passed before the combined run. The combined
experimental gate was interrupted during the VM-next contract's second build.
On resumption, its process and lock owner were verified dead and only the stale
lock was removed. The completed prefix was retained, the VM-next contract was
restarted, and the exact remaining gate suffix ran on the same base/diff hash.
This is combined prefix/suffix evidence, not an uninterrupted gate invocation.

The completed sequence covers:

- Every experimental contract, including all four M7 profiles: ARM64 and
  arm64e/BTI, with assertion and release runtimes.
- ARM64 stock source and original-v2 import corpora: 509 passed each.
- Signed x86-64/Rosetta rooted-store, nil-reader, bounded-reader, raw nil-store
  and segment-map race fixtures; stock source and import: 509 passed each.
  Runtime/object undefined symbols contain no atomic runtime fallback.
- Complete bootstrap gate, including all 387 applicable stock cases,
  interpreter/GC2/TG contracts, callbacks, aggregate/variadic interpreter FFI,
  and the ARM64/arm64e/x64 errno unwind contract.
- Restoration to experimental ARM64 with assertions and a final unchanged
  base/diff hash check.

Commands were driven by `/tmp/ljmt-f32-full.sh`, followed by
`/tmp/ljmt-f32-full-resume.sh`; the latter resumes the experimental suffix
through `/tmp/ljmt-f32-experimental-resume.sh`. They use the Apple SDK compiler,
macOS deployment target 13.0, two build jobs and ad-hoc link signing. Logs:

- `/tmp/ljmt-f32-m7-focused-final.log`
- `/tmp/ljmt-f32-experimental-f32-e2f6b7fd-working.log` (completed prefix)
- `/tmp/ljmt-f32-experimental-resumed.log` (VM-next and remaining contracts)
- `/tmp/ljmt-f32-stock-source-f32-e2f6b7fd-working.log`
- `/tmp/ljmt-f32-stock-import-f32-e2f6b7fd-working.log`
- `/tmp/ljmt-f32-x64-regression-f32-e2f6b7fd-working.log`
- `/tmp/ljmt-f32-bootstrap-f32-e2f6b7fd-working.log`
- `/tmp/ljmt-f32-development-profile-f32-e2f6b7fd-working.log`
- `/tmp/ljmt-f32-full-resume.log` (sequence completion and hash confirmation)

Final development profile identity:
`5287d5b1a390fe5fb3ec756fc83553fee2f0930a1d295239472089e4f5fa1c0e`.
Flags: `-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL
-DLUA_USE_ASSERT`.

This checkpoint does not establish general ARM64 JIT parity or add a new
sanitizer, native Intel performance or sustained-concurrency result. CALLM,
aggregate/sret, variadic/vector/complex native calls, unsupported boxed-64-bit
graphs and general heap/side-trace capabilities remain open work.
