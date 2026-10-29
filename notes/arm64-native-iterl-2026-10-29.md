# Native scalar ITERL roots (2026-10-29)

The custom iterator loop with a scalar Lua helper and an explicit body break
now records, assembles, publishes and enters natively on Darwin ARM64. This
checkpoint covers integer and floating-point keys. It preserves the ordinary
lockless trace publication/lifetime protocol and introduces no recorder or
admission bypass. Iterator-parent side traces remain closed. The separate
nil-terminated source shape still has no native runner root: its reachable
RET0 leaf branch is outside the existing bounded leaf grammar.

The recorder binds the exact setup JMP, ITERC and ITERL generation to a
two-argument, one-result non-vararg Lua call. Semantic and post-RA admission
independently reconstruct the hidden function/state/control triple. An absent
snapshot slot must have a unique guarded root SLOAD; explicit mapped values
take precedence. The second unrolled call uses the LOOP map and a source-body
check that excludes writes to the hidden triple. Complete hot-leaf replay
binds the returned value to both the visible key and the copied control value,
including the loop PHI. Child exits retain the existing frame and cold-prefix
proofs. Bounds are checked before inspecting a later LOOP snapshot.

JITERL performs the stock nil test and key-to-control copy before requesting
entry. Entry requires the exact whole consumed bytecode word, the immutable
root certificate and generation, and equal non-nil live key/control values.
The existing jit_base lease protects the native lifetime. A failed entry
restores the consumed word and takes the existing branch-only JITERL recovery
path, without repeating the call or control copy. Both metadata reads and the
final source recheck retain the common root-entry checks.

## Validation

`tools/ci/arm64_jit_iterl_contract.sh` passes ARM64 and ARM64e assertion and
release profiles. Each profile compares 88 native calls with a separate
interpreter-only oracle while an MT peer remains attached. Two generations
of both integer and numeric key traces survive full GC, flush/recompile and
changed-callable fallback. Each profile rejects 48 private malformed semantic
views and 48 malformed post-RA views, covering source words, argument slots,
result/control mappings, PHIs, snapshot bounds/footers and leaf return shape.
The entry fixture injects a profile request after metadata capture and changes
the whole JITERL instruction before its final recheck; both cases release the
lease and reject native entry. A mismatched live key/control pair also rejects.

The fixture targets the intended ITERL hot edge after a flush. During initial
testing, the numeric runner's ITERL and leaf FUNCF shared hot-count bucket 21;
a failed leaf compilation left that bucket at 50077, preventing reheat in the
bounded warmup. The fixture now sets the owner-local hot counters so one leaf
call followed by the loop decrement heats ITERL. This changes only test
warmup, with no production admission relaxation. The unmodified ordinary
topology probe below independently demonstrates native publication.

Ordinary assertion binaries on both targets pass the complete topology probe
without test helpers, forced hot-count writes or a TRACE observer. The scalar
iterator runner has a selected native root on ARM64 (20 IR, eight exits), with
81 selected-root exits in each normal, type-error, zero, one and longer-loop
phase. x86_64 also has its own selected iterator root. The nil-terminated
runner still reports no ARM64 root, so semantic comparisons are not claimed
as full compilation parity. Strict `-Wall -Wextra -Werror` Darwin x86_64
compilation passes for lj_asm.c, lj_trace.c, lj_record.c and the fixture.

Evidence is under `/Users/frityet/Scratch/ljmt-iterator-record-map-20261029/`:
`native-contract-four-profiles.log`, `native-validation.log`, and immutable
runtime/archive/module bundles and topology logs in `native-ordinary/`.
Source copies and SHA-256 manifests accompany those bundles. Earlier failed
diagnostic logs remain separate. The umbrella gate now runs both the source
tuple and native iterator contracts; it has not yet been rerun for this
checkpoint.
