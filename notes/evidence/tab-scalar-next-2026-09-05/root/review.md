# Scalar-next combination review

Base HEAD is 997c0044, including the accepted worker SWEEP/native teardown
change and constructor deferral/between-TG fairness. The exact owner v1 patch
fe8b4e4598f56e420c302ccd4545fd90c2707d59688565943b69044e7fb482f8
changes only lj_tab.c and a helper-only declaration in lj_tab.h among 225
runtime/generator inputs. All four candidate configurations share those
inputs. No lj_api.c capture patch is included.

## Authority and progress

The new path runs only after global SMR refusal. It independently admits the
small table allocation before any header read, then rereads the actual table
and scalar-key source cells and exact state owner. A separate array is admitted
at its allocation header before reading vector metadata; colocated capacity
must fit the retained table span. The exact nil hash node, paired array/node
snapshot, allocation extent, nonretiring vector and source/owner confirmations
bound the semantic attempt. Numeric/boolean results have no new GC lifetime
to discover. Any other tag is an opaque refusal, never a skipped value.

FOUND and END share final confirmations after copying the result. All source
reads precede any aliased output store. END and RETRY preserve both outputs and
the cursor. Success retains ordinary stack/root publication. Every acquired
lease is released on every path; the old wait sees no remaining independent
authority. Output stack offsets are restored on each outer attempt. The
existing allocating retry hook is left to its original released-authority
window; new stage hooks cannot allocate, move stacks, throw or call Lua.

The helper does not change successful SMR reads, GC/hash traversal, physical
writer gates or the registry/arena metadata admission loops. A paused plain
arena writer, Huge allocation or unsupported semantic shape can still refuse.
No wait-free or general iterator completion claim follows.

## Exact validation generations

Owner manifest 2f526f18874696539c19965fe8db862dee4afbbce42a956afe08d894c216cd85
has 376 verified artifacts (470930218 bytes including hash-only binaries).
It records 123 final runtime passes on exact793, plus immutable development
failures, older baseline alarms and the public-C receiver-capture alarm.
The ROOT package archives current tracked code/tests/tools without notes;
setup.json owns all before/after runtime inputs and fixture hashes.

ROOT's current-source configurations and positive runtime counts are:

| Configuration | Passes | Scope |
| --- | ---: | --- |
| Default, no helper macros | 4 | Stock and generational off/on |
| Optimized, GC2 helper only | 27 | Stock off/on, original IDLE-entry, 24 paused progress cases |
| Assertion/APICHECK plus ten recorded defines | 48 | Above plus 11 authority, six moving-stack retry, two lifetime and two general-reader controls |
| Target-only Clang O1 ASan, same ten defines, LSan enabled | 48 | Same complete set |

All 127 pass. Compilation records are separate. Host build tools have no ASan
references; lj_tab.o, lj_gc.o and lj_gc2.o do. Builds and runs verify exact
source sets before/after. Default is not inferred from an optimized helper
configuration. The baseline negative pair links the accepted worker+fair
strict archive (58ade6fefc9225c1442e54166ab39bff0edb621ed1e29a83c7456e38113b3b64)
without the iterator patch. Unchanged next/dense and rooted/empty both terminate
with their original SIGALRM (exit -14), while candidate counterparts pass.

Shared promotion applies the exact two-file patch. The permanent authority
filename becomes t-tab-scalar-next-authority.c; the dependent lifetime file
changes only its include and introductory comment. All other fixture bytes
are exact owner versions. Canonical compilation checks the renamed dependency
through the normal harness rather than a private source copy.

The registered m6_jit_alloc_account passes all five runtime components in
15.916115089 seconds. Its previously stalled unchanged IDLE fixture and both
formerly unrun cooperation cases complete. The new m5_tab_scalar_next passes
44 components in 28.439942948 seconds with GC2/TAB/ARENA/assert/APICHECK defines
and restores the default build. Both complete records retain source hashes,
commands, runner identity, archive/ELF hashes and full output. These 49 plus
127 isolated passes give 176 ROOT positive processes, distinct from owner's
123 older-source passes and the two current-baseline negative controls.

## Explicit limitations

The cost suite uses unchanged default archives for both the accepted worker
baseline and candidate, CPU 30, a fixed 32-number array, full collection after
warmup, five CPU-time samples per process and seven alternating pairs for each
of ordinary next and ITERN with JIT off/on. Interpreted runs visit 320000 values
per sample; JIT-enabled runs visit 6400000. Checksums are exact, with no measured
allocation churn. JIT-enabled processes report seven/five traces respectively;
the engine-off controls report zero. This records engine-enabled cost and
trace presence, not an instruction-by-instruction native coverage proof.

All 56 final processes (280 samples) and the separate eight-process pilot pass.
Median paired changes are +0.43833% next/off, +0.16764% ITERN/off, +1.59453%
next/on and +0.10529% ITERN/on. Next/on process-median ranges are 39.559–39.638
ns baseline and 40.063–40.848 ns candidate, with medians 39.615625 and 40.20375.
That consistently higher narrow-loop cost is explicit; it is not called noise
or unchanged performance. The other three ranges overlap. The stability
benefit is accepted with this measured cost, leaving normal-path tuning and
broader application/MT throughput as separate performance work. No throughput
gain or stock-parity claim is made.

The public C lua_next capture still blocks before this iterator helper. The
separate owned-stack capture candidate has no accepted final validation after
its automatic review interruption; no part of that candidate or interrupted
validation is retried here. The earlier VM/ITERN implementation and its complete
final evidence were already frozen independently. General table helpability,
hash/GC-valued next, Huge/custom allocator support, synchronous GC ownership,
same-TG arena fairness and the baseline JIT automatic assistance defect remain.
