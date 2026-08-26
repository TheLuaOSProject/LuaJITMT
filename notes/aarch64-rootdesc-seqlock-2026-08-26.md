# Apple ARM64 root-descriptor seqlock repair

The architecture-aware standalone harness exposed a real ARM weak-memory
failure in `LJGC2RootDesc`: two native runs observed an ACTIVE descriptor whose
`old_root` and `aux_root` came from different owner publications.  The former
protocol wrote a reused payload while control was IDLE and relied on x86 TSO
plus a control/payload/control sample.  An ARM reader could retain the previous
ACTIVE control observation while seeing stores from the next payload.

The repaired state machine uses the previously unused control state 3:

```text
IDLE(n) -> PUBLISHING(n+1) -> ACTIVE(n+1) -> IDLE(n+1)
                    \-> NO_RECLAIM(n+1)
ACTIVE(n+1) ----------------------> NO_RECLAIM(n+1)
```

The owner's acquire-release claim of PUBLISHING and a following release fence
precede every payload store; the acquire-release activation publishes the
complete payload.  Readers never inspect PUBLISHING.  An ACTIVE reader performs
an acquire fence after its relaxed payload loads and before rechecking the exact
control word.  If it observes any payload store from the new generation, the
paired fences make PUBLISHING happen-before that final control load, so atomic
coherence forbids accepting the older ACTIVE word.  A pin may still replace
PUBLISHING or ACTIVE with sticky NO_RECLAIM, so delayed activation cannot
overwrite it.

Validation on Apple clang 21, targeting macOS 11:

- the native markword/activation/root-descriptor fixture passed 100 consecutive
  optimized runs (the old protocol failed twice in 51 observed runs);
- the optimized ARM64 artifact contains the expected `dmb ishld` reader fence
  and imports no atomic runtime helper;
- ASAN+UBSAN passed the complete fixture;
- the native ThreadSanitizer build passed the complete fixture;
- the exhaustive root-gate/store good model still has zero failures and all
  five intentionally broken variants are detected;
- the x86-64 cross-built fixture runs under Rosetta, retains inline
  `cmpxchg16b`, and imports no atomic runtime helper.

This repairs the descriptor substrate only.  It is not evidence that the ARM
assembler VM already has all acquire/release loads, safepoints, or TG-local
state required for lockless execution.
