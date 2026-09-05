# Narrow owner-deferral candidate: first review checkpoint

This isolated candidate is based on the frozen pristine 597b control at `/tmp/lj-gc-auto-stop-overlap-20260905-y4h4cc8a/controlextrahelpers`, with all 807 source inputs recorded. It contains only `src/lj_gc.c`, `src/lj_gc.h`, and `src/lj_gc2.c` changes in `candidate.patch`. No shared source/build/fixture edits and no automatic-control prototype are included.

The richer reclaimer returns an optional exact LINKING/UNLINKING observation; its old wrapper, original scan, cursor, EOF, pending, finish, and ownership behavior remain. The production owner call publishes one existing deferred_epoch event after the existing unseal and physical-writer cleanup and ends that owner quantum. The original step/count branches still account executed work. The worker TG loop ends on that event. The automatic outer batch also stops after an inner step changes the event.

Both fixture executables were compiled from the byte-identical untouched original `t-func-construction-anchor.c` with the same six helper macros and original 60-second run bound. Candidate build succeeded (8.47s), fixture passed (0.0155s). The unchanged frozen-control fixture reached its original 60-second timeout (exit recorded as 124 by the runner). Exact argv/CWD/LUA_PATH/times/flags and raw stdout/stderr are saved in `*-focused-results.json`; source/archive/ELF hashes are in `*-focused-identities.json`. The fixture timeout is an actual timeout, distinct from a diagnostic debugger exit. These results establish the focused before/after behavior, not all retained-work or fairness requirements.

## JIT caller review

`lj_gc_step_jit` has the finite `while (steps-- > 0 && gc2_step_auto(..., 1) == 0)` loop. An active-phase deferral still returns -1 from the unchanged automatic epilogue and stops this loop. A concurrent phase-to-IDLE observation after the event can take the IDLE epilogue with `done == 0 && drove == 1`, return 0, and permit another iteration. This is a real source possibility, not a measured occurrence.

That path has observed an independently closed phase under the existing gates, publishes the idle threshold, and the JIT loop recomputes threshold_step from current bytes before its next finite iteration. Without fresh debt/admission, a subsequent no-work IDLE call returns -1. If new debt/another cycle exists, a later iteration can perform bounded work on it. This does not reproduce waiting indefinitely on the still-active blocked sweep; the JIT steps counter also remains finite. The candidate does not change this separate IDLE return contract or broaden the JIT loop. Focused validation should still watch repeated admission if the later combined automatic-control candidate changes these conditions.

## Remaining acceptance evidence

This is a review checkpoint, not a ready-to-integrate runtime patch. Next work must prove actual nested deferral with the same retained allocation, successful publication and cancellation followed by eventual collection, bounded automatic and background-worker retries, and useful progress for another eligible owner while one publisher is held. Repeated global events can yield other work in the same global_State; if mixed-owner work starves, this narrow candidate is not ready. Candidate3 automatic control may be combined only in a later separately identified variant.
