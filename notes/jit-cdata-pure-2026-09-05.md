# Guarded pure-cdata loop reuse, 2026-09-05

Pure pre-MT cdata loops now reuse the method checks made at root entry. The
entry base-table, node, key, type and function guards remain, so replacing
`__index` or `__newindex` still changes subsequent execution. This recovers the
focused field-loop cost introduced by the stale-method correctness repair.
Shared-MT method recording remains refused under its existing policy.

The compiler classifies the whole original root body before copying it during
protected unrolling. Only direct loads, selected inline arithmetic/comparisons,
two int32/number conversions, and direct numeric/int32 cdata payload stores
qualify. A store must start at a typed cdata SLOAD/KGC plus a constant payload
offset in `[sizeof(GCcdata), INT32_MAX]`. This assumes supported in-bounds FFI
accesses; it is not a bounds check for arbitrary foreign memory.

Allocations, Lua-object stores, indirect pointer stores, helpers, foreign calls,
XSAVE, profiling operations and every unclassified IR operation reject the
whole body. The exception recognizes only the cdata base-root FLOAD and its
node-vector FLOAD. Existing XBAR, store aliasing, phase and TG polling remain.
The full shared-runtime predicate is checked again at the actual fold, after
fallible compiler allocation. Side traces reenter through root entry guards.

A token-private byte limits this permission to protected root unrolling. It
uses existing padding and is cleared immediately after the protected call,
before error, free or retry work. No runtime lock, wait, version counter,
publication protocol or new lifetime authority is added.

The measured original patch is `508e8012…`; the final implementation patch is
`6eff86feefc6299c35fa74d3b03562fdf4476ffbbae2e9c7a6be5f9d7a537853`.
Their only difference is a corrected comment about global GC-worker activation.
Matching filename/line-number preprocessing verifies identical compiler input.
Only `lj_jit.h`, `lj_opt_loop.c` and `lj_opt_mem.c` change. The final source
identities and [independent scope/lowering review](evidence/jit-cdata-pure-2026-09-05/isolated/independent-review/final-review.md)
are retained in the [isolated handoff](evidence/jit-cdata-pure-2026-09-05/isolated/review.md).

Six permanent fixtures and two M6 entries require actual native execution:

- Seven mutation/error/resize/lifetime modes retain exact current-method calls
  and entry guards while confirming removal of the copied lookup chain.
- Installed side-to-root reentry executes before and after method replacement.
- Seven excluded bodies keep their repeated checks and exact results.
- A real profiler sample changes a method after the policy flush.
- Real MARK gate closure and global worker activation force early native exits;
  method mutation occurs on the owner after exit, with exact continuation calls.
- Injected protected allocation failure returns `LUA_ERRMEM`, clears the private
  flag, and permits subsequent correct native execution.

Final integration uses `f43a9f24` with the optimizer, permanent tests and the
separate XSAVE fixture correction subsequently committed as `3750bc03`. All
799 runtime/generator/test inputs match in normal, assertion and Clang ASan
trees. All 115 commands and 90 executed test processes pass: 28 normal and 31
each in assertion/ASan. Stock reports 387 JIT-off and 509 JIT-on in each tree.
Existing cdata guards/capture, both first-attachment modes, callback stack
relocation/unwind, and matching helper-enabled XSAVE/nested-callback/post-call
fixtures pass. Both new canonical entries also pass in the shared default build.
ASan/LSan uses `detect_leaks=1:abort_on_error=1` without suppressions, with runtime
instrumentation and uninstrumented host generators checked explicitly.

The combined performance check compares normal default mixed runtimes with the
poll/callback repairs in both; all 224 runtime/generator inputs match except
the three optimizer files. Seven fresh alternating pairs on CPU 31 use the
unchanged 30-million-iteration `ffi_struct` harness, which reports the best of
five CPU times to four decimal places. Median time changes from 0.0684 s to
0.0206 s, a 69.9% reduction. Every paired value matches its respective median
at that precision. The earlier isolated study agrees. This is a shared-host
field-loop result, not constructor, full-suite, concurrent-MT or stock parity.
[Exact commands and samples](../bench/jit-cdata-pure-2026-09-05/field-cost-results.json)
and [the cost summary](../bench/jit-cdata-pure-2026-09-05/field-cost-summary.json)
preserve that measurement boundary.

[The evidence manifest](evidence/jit-cdata-pure-2026-09-05/artifact-manifest.json)
archives the 409 hash-verified isolated artifacts, 41 permanent-test artifacts,
and final combined results. Earlier no-op/compile trials, invalid fixture
premises, classifier/flag negative controls and the independently repaired
callback defect remain labeled. The old first-attachment gap in the isolated
review is repaired separately by `8d342cd6`; global-worker safety still depends
on phase/entry exclusion. Collector root borrowing, synchronous handshakes and
general MT recording remain open. Linux x64 only; Windows/macOS remain deferred.
