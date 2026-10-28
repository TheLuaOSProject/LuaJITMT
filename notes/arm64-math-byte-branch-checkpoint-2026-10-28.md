# ARM64 scalar math, bounded byte reads and branch checkpoint

The continuation branch combines native ABS/min/max/numeric NEG at `89f22dc2`,
bounded string-byte reads at `ca96ec0c`, stronger signed-zero tests at
`ed7e1832`, and bounded Lua leaf branches at `a4acb620`. These extend the
existing ARM64 operation and snapshot certificates; they add no runtime lock,
GC allocation protocol, executable-memory mechanism or emitter change.
The production sources were frozen at `a4acb620` for integration validation.
They are integrated and pushed on `v2.1` through `8e2679b0`, which adds only
comparison-boundary tests and the nested-call audit after that source commit.

## Capability boundaries

- Scalar ABS, min/max and numeric NEG use existing ARM64 instructions. The
  independent post-RA check requires the exact ABS/NEG mask provenance and
  ensures those mask loads are not allocated as real values. Corrected max
  signed-zero cases produce zero and compare reciprocal signs; both numeric
  NEG zero signs require native entry in each profile.
- `string.byte` accepts an exact two-argument, one-result builtin call with a
  positive constant index. Semantic and post-RA checks independently bind
  readonly U8 loads to a STRREF, its immutable string, and an earlier exact
  length bound. Generic pointers, dynamic/negative indices and pointer/U8
  snapshot escapes remain closed. The original literal-byte inventory row
  still lacks a native-root witness at this checkpoint.
- Leaf branches support one ordered numeric comparison, two nonempty acyclic
  arms and a common join. Each independent snapshot proof follows the selected
  path, including NaN cold exits and live-parameter restoration. Nested tests,
  early returns and empty arms remain outside this slice; multi-operation
  arithmetic arms have not yet been demonstrated to publish.

The merge preserves the distinction between NUM unordered comparisons and
unsigned INT comparisons: the latter require the exact string-byte bound
proof. The older malformed INT guard test still rejects its input, with TYPE
instead of OPCODE as its diagnostic. The first combined full-gate attempt
stopped only because that test expected the older diagnostic; its rejection
assertion was retained. Follow-up INT post-RA negatives and NUM positives are
tested separately against the same production source.

## Evidence and current validation state

The combined math/string-length/string-byte focused run passed all four
ARM64/arm64e assertion/release profiles before branch integration. The branch
slice independently passed the four-profile leaf contract: each profile
reported 95 return cases, 392 native runs, 95 terminal exits, 297 same-trace
reuses and 95 GC reuses. Independent static review of the combined math, byte
and branch comparison checks found no concrete admission defect; that review
does not substitute for runtime validation.

- The complete experimental gate passes on the frozen combined source,
  including four-profile leaf/string/GC contracts and all four combined M7
  FFI profiles. The independent comparison-boundary follow-up also passes its
  ordinary ARM64 IR admission contract against this production source.
- Ordinary ARM64 assertions pass thirteen Lua companion contracts and both
  509-case stock source/import corpora. Signed x64 assertions pass both corpora
  and the return, compound, math, string-length and string-byte companions.
- Both architectures pass all 260 inventory result checks. ARM64 has selected-
  root exit witnesses in thirteen of twenty rows, versus eighteen on x64;
  dynamic string byte is the added ARM64 witness. The original early-return
  branch row is outside this branch slice and still has no witness. These
  observations establish some selected-root execution, not full-body
  compilation, throughput or parity.
- Main is independently rebuilt as ordinary ARM64 assertions and passes the
  same thirteen companions, both corpora and the inventory. Its signed
  executable has no undefined libatomic or checked test-hook symbols.
  SHA-256 is `959076a89c5586ef4af6e19f9bcc2ae161752a8bffe8aeced0e309a8d8326f7a`.
  Main and the worktree were built at different documentation/test commits;
  their generated rolling-release timestamps differ (`1793216388` versus
  `1793215248`). The assembler and trace objects match; executable byte identity
  is not claimed.

Evidence is retained in
`/Users/frityet/Scratch/ljmt-math-byte-20261028/`: `focused.log`, the original
`experimental.log` diagnostic mismatch, `experimental-branches.log` for the
frozen combined source, and the ordinary build/corpus/companion/inventory logs.
Branch-specific evidence is in
`/Users/frityet/Scratch/ljmt-arm64-branch-20261028/`.
The independent comparison-boundary test evidence is
`/Users/frityet/Scratch/ljmt-parity-audit-20261028/arm64-jit-ir-admission-followup.log`.

The bootstrap-only gate and separate historical table/FFI fixture matrix were
not repeated for this checkpoint; the experimental gate includes the combined
FFI suite and its runtime lifecycle contracts.

This is not x86_64 parity. General Lua calls, nested frames, varargs, multiple
results, broader trace topology, heap effects and FFI representations remain
open, as do broader concurrency and performance comparisons. A passing run
does not explain or fix the earlier intermittent rooted-length native-witness
failure or callback auto-unwind timeout.
