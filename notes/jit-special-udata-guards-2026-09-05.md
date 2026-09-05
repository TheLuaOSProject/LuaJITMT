# Special userdata method guards, 2026-09-05

Native code could ignore replacement methods on C-library namespaces, files,
and buffers. The exact `e3428257` baseline warms and executes a real native
`ffi.C.abs` lookup loop, then replaces the namespace's `__index` with a function
returning false. Interpreter execution calls the replacement 80 times and
returns zero; native execution calls it zero times and returns 80.

`lj_record_mm_lookup` now sends special userdata through the existing guarded
metatable and method lookup. The namespace identity specialization and other
userdata subtype guards remain. The removed early return treated the method
or index table as immutable, although Lua table writes and
`debug.setmetatable` can replace them. The common path checks the metatable,
table storage/key, value type, and called function identity. It handles missing
methods and table-valued methods through the ordinary recorder behavior.

This retains the full shared-MT refusal before receiver/metatable sampling.
It does not enable shared-MT metamethod recording. The pure-cdata optimizer
exception does not admit userdata metatable loads; no optimizer or runtime
lookup helper changes accompany this fix. Recorded callable constants retain
their existing GC roots, and all guards precede the guarded operation.

The permanent fixture covers 11 modes for each of namespace, file, buffer,
and ordinary userdata, with JIT off and on in fresh processes. It checks
function/table/missing/nonfunction `__index`, metatable replacement/removal,
hash growth plus collection, old method lifetime, an index-table entry change,
and function/table-valued `__newindex`. Every native case requires an actual
warm exit and a post-mutation exit from an original retained trace; an
interpreter-only fallback or trace count cannot satisfy the test.

The exact baseline has 30 native failures across the three special kinds and
58 passing controls. All 88 cases pass with the fix in default, assertion, and
Clang ASan builds. There are 326 passing runtime processes including the
initial reducer and relevant existing tests: stock 387 JIT-off/509 JIT-on per
build, cdata method guards, pure-loop/side/exclusion/profile checks, callback
stack relocation and unwind, first attachment, generated remote CALLXS
pointer/bool/sret collection and flush, and the full XSAVE fixture in helper
builds. Runtime ASan/LSan uses `detect_leaks=1:abort_on_error=1` without
suppressions; host generators are checked uninstrumented.

Three earlier driver invocations omitted the exclusion fixture's required
mode and failed its input assertion. Those commands and failures remain in
the archive; all seven correctly specified modes subsequently pass. They are
not counted as passing processes. The shared canonical
`m6_jit_special_udata_guards` registration passes all 88 cases in 44.608 seconds
including default build preparation.

All 224 tracked runtime/generator inputs match across the three fixed builds.
Only `lj_record.c` differs from baseline; only `lj_record.o` and
`lj_record_dyn.o` differ between the paired normal object sets. Final source
SHA-256 is `07116bc933781976c91453a9ca89a46aef4a81d80bf71ab2f6e5269383fcce87`.
[Validation identities](evidence/jit-special-udata-guards-2026-09-05/root/final-validation.json)
and [independent source review](evidence/jit-special-udata-guards-2026-09-05/independent/method-guard-review.md)
record the exact boundaries.

Seven alternating fresh-process pairs per workload use CPU 31, normal builds,
enabled GC, and five timed rounds on a shared host. Every sample is retained.
The custom workloads prove prior native execution; the foreign-call workload
also requires actual CALLXS IR. The existing `ffi_struct` harness is unchanged.

| Workload | Baseline median ns/iteration | Fixed median | Median paired change |
| --- | ---: | ---: | ---: |
| Namespace symbol lookup | 1.003 | 2.051 | +104.53% |
| File method lookup | 2.051 | 2.508 | +22.29% |
| Namespace lookup plus foreign `abs` call | 83.734 | 84.242 | +0.40% |
| Existing `ffi_struct` | 0.687 | 0.687 | 0.00% |

File lookup has substantial variation: four fixed processes are about 2.51 ns,
one 5.65 ns, and two 9.57 ns, despite equal aggregate IR/machine-code sizes.
Its geometric paired ratio is 2.018, so the median alone understates this
sample's cost. These checks remain necessary; any coalescing requires its own
complete-body alias, phase, callback and side-exit proof. The results describe
these particular lookups, not full-suite or stock parity.
[All 56 cost processes](../bench/jit-special-udata-guards-2026-09-05/cost-results.json)
and [their summary](../bench/jit-special-udata-guards-2026-09-05/cost-summary.json)
retain commands, input identities, all rounds and generated-code counts.

The independent review also reproduces separate baseline defects in the
CLibrary builtin recorder: a directly called captured `__index` can accept
the wrong userdata, and existing native namespace lookups can ignore debug
cache changes or semantic close. Those controls are preserved and remain
follow-up work. This method-guard commit does not repair those other lookup
contracts or establish release readiness. Validation is Linux x64 only.
