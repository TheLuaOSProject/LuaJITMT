# Captured CALLM and ancestry integration — 2026-10-29

The unpromoted `codex/arm64-side-ancestry-integration` branch combines deeper
ancestry, fixed CALLM arity normalization, zero-formal constant consumers,
current-iteration captured-consumer replay, middle-ancestor scoped retirement,
and the safepoint-suite capability correction. Main `codex/arm64-parity` remains
at the earlier production checkpoint with the harness correction (`e5fe6acdd`).

At `25eeb67e4`, the complete open-CALLM exit contract passes ARM64/arm64e assertion
and release profiles: eight ordinary consumer cases plus three captured
consumer cases per profile. It proves native selected exits, unchanged code
bytes/address/generation after GC/reentry, live peer, and independent source,
IR and map refusals. The merge preserved both cross-arity and captured-case
fixtures. Log: `captured-normalized-consumer-four-profile.log`.

At `dae24b46a`, strict assertion x86_64 builds pass with `-Werror`. The helper
build passes `t-jit-trace-retire` (including SMR reader balance). The restored
ordinary build passes both 509-case stock corpora and the ordinary depth-three
and topology probes. The depth probe observes 1504 deepest-side native exits
in its final reentry phase with settled code bytes unchanged. Logs are
`captured-ancestry-x64.log`, `captured-ancestry-x64-stock-source.log`,
`captured-ancestry-x64-stock-bytecode.log`, `captured-ancestry-x64-depth.log`,
and `captured-ancestry-x64-topology.log`.

The combined ARM64 matrix did **not** pass. Its first assertion profile at
`dae24b46a` failed the first capacity graph fixture: expected 64 children,
observed 3, with 4097 trace slots. The earlier middle-ancestor check had passed,
but subsequent retirement/leaf/consumer fixtures in this matrix were not run.
`combined-captured-ancestry-matrix.log` preserves the failure; the outer command
returned 134 and restored the ordinary runtime. Separate investigation of this
repeatable publication/topology interaction is active. Main promotion is held.

All named logs above are under
`/Users/frityet/Scratch/ljmt-side-ancestry-20261029/`. Full parity remains
incomplete. The independent indirect-FFI-result and zero-formal captured-
producer branches are not included in this integration point.

## Subsequent component integration at 45fc76a31

`bbde96c5f` integrates indirect 17–32-byte FFI aggregate returns through
`c3c77156a`; its aggregate contract passes all four profiles in
`combined-indirect-ffi-four-profile.log`. `45fc76a31` then integrates
`693b161ff` for captured zero-formal producers with one or two results. Both
the complete Lua leaf and open-CALLM consumer contracts pass four profiles in
`combined-ffi-zero-producer-leaf-four-profile.log` and
`combined-zero-producer-consumer-four-profile.log`.

Ordinary assertion ARM64 and strict assertion x86_64 builds at `45fc76a31`
pass both 509-case stock corpora. The x64 helper build also passes
`t-jit-trace-retire`. Logs use the `combined-ffi-zero-` prefix in the same
evidence directory. The ordinary ARM64 depth-three probe preserves the settled
code bytes across GC/reentry and observes 3520 deepest-side exits. The topology
probe matches its interpreted results, but does not establish native topology
parity: for example, the fractional while-loop phase three still uses one ARM64
side and repeated exits where the matched x64 probe formed two sides.

The wide-graph failure remains unresolved. Isolated repeated runs at the older
combined source `dae24b46a` show count3 before and after GC, with an MCODEAL abort
that flushes all traces; successful runs instead observe a recoverable MCODELM.
The failing cases have only 256KiB total MCode with a 2MiB configured limit, so
raising `maxmcode` is not yet justified. Investigation is separating range
mapping failure from retirement-node allocation and the configured-size limit.
Evidence is retained in `/Users/frityet/Scratch/ljmt-cap-investigation-20261029/`.
The current component passes do not supersede this blocker or constitute a
complete combined gate. Main promotion remains held.

## Pointer results and allocation diagnosis

`c35f0f38f` integrates `e1509da33`: pointer fields in direct small aggregate
results. The combined aggregate contract passes ARM64/arm64e assertion and
release profiles (`combined-pointer-four-profile.log`) and restores the
ordinary ARM64 build. This does not widen pointer-containing argument or
indirect-result admission.

The allocation failure is now localized to `mcode_alloc` range placement, not
retirement-node allocation or `maxmcode`. On original fixture execution,
16 advisory MAP_JIT hints all returned the same non-null mapping
`0x107a10000`, outside the active window `0xfeac0000..0x106ab0000`; it was
freed each time. With only 192KiB allocated, exhaustion of those probes caused
MCODEAL and a full trace flush. The window is approximately 128MiB
(`LJ_TARGET_JUMPRANGE=27`), not 32MiB. A reported errno after successful mmap
is stale and is not evidence of an mmap syscall failure. Exact evidence is
`baseline-allocator-sites.log` in the capacity-investigation directory,
lines 83–101. A range-preserving allocator correction is in progress; this
component pass does not release the main promotion hold.
