# Executed CALLXS remote-flush fixture, 2026-09-05

The authentic CALLXS aggregate now reaches and passes its remote GC/flush,
post-call and nested-callback checks through the canonical runner. No production
runtime source changes. The remote fixture captures its foreign functions and
the actual cdata-call builtin on the worker before recording, and verifies that
each blocked foreign call really came from published generated code.

Previously the worker failed its scalar XSAVE assertion before sending the
first ready message. The parent then timed out after ten seconds and hid that
error. The retained diagnostic joins the failed worker and exposes the original
reason. Verbose traces abort at the library lookup; `lj_record_mm_lookup` still
refuses shared-runtime metatable lookup before sampling its receiver edge.
This remains a runtime recording limitation requiring rooted lookup authority.
Capturing dispatch does not fix general MT metatable/library lookup recording.

For this lifecycle fixture, calling the real captured builtin with its cdata
function preserves the production generic CALLXS lowering, without requiring
the unrelated dynamic lookup to record. Original XSAVE/CALLXS, pointer/boolean/
aggregate results, blocked-call counts, GC/flush bounds and no-replay assertions
remain. Failure to receive the first ready message now reports the worker's
join result instead of a bare assertion.

The additional native-execution witness uses `jit.util.tracemc` to publish
scalar code ranges after warming. At the blocked callee, the C fixture records
whether its actual return PC lies in one of those ranges, before publishing
the entered flag. The pointer, boolean and struct-result calls must each have
exactly one such entry. The channel protocol keeps the range list stable until
the call returns. No code bytes are read through these retained addresses;
the ranges provide test observations, never runtime lifetime authority.

Disabling JIT for the pointer loop after warming still executes the foreign
function but fails the new native-caller assertion. The failure path releases
the callee before reporting the error, allowing close to join it. This negative
control prevents correct interpreter results or mere trace existence from
satisfying the generated-call lifecycle test.

The final behavior passes on the exact `b4e26564` assertion baseline and current
normal, assertion and Clang ASan/LSan runtimes. ASan uses
`detect_leaks=1:abort_on_error=1` with no suppressions; the small C test library
is compiled normally. All 224 current runtime/generator inputs match
`35cfaaa2`. The canonical `m7_ffi_callxs_authentic` entry passes in the shared
workspace using a private temporary directory and restores the default build.
It now runs through the later post-call and nested-callback C fixtures as part
of that aggregate. Final Lua differs from the directly tested witness only in
removed trailing whitespace; canonical validation uses those exact final bytes.

[The evidence manifest](evidence/ffi-remote-flush-fixture-2026-09-05/artifact-manifest.json)
preserves original failures, worker error/trace diagnostics, the intermediate
dispatch-only experiment, final native-PC checks, the interpreted negative,
exact source/binary identities, and complete canonical output. Functional
completion times are not benchmark comparisons. Linux x64 validation only;
Windows/macOS and general lockless GC/MT recording remain release work.
