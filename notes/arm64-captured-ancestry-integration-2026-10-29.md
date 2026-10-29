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
