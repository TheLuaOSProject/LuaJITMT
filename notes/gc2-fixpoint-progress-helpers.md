# GC2 Fixpoint Progress Helper Slice

This slice routes the GC2 mark-fixpoint progress detector through helpers:

- `gc2_marks_this_round_store_rlx()` initializes and mark-begin resets the
  progress word.
- `gc2_marks_this_round_add()` records first-time arena/HugeTab marks.
- `gc2_marks_this_round_xchg_acqrel()` resets the detector at the start of a
  bounded fixpoint round.
- `gc2_marks_this_round_acq()` reads the post-round zero-mark predicate.

The field remains a relaxed progress counter for successful first-time marks,
paired with acquire-release exchange at round start and acquire read at the
fixpoint predicate. This preserves the existing 05 section 5.7.1 protocol while
making the raw access boundary explicit.

Coverage:
- `m3_gc2_paranoia` and `m3_gc2_worker_scheduler` own the observable mark
  fixpoint behavior.
- Production access to `GC2State.marks_this_round` in `lj_gc2.c` must stay
  behind the documented helper surface instead of source-text matching.
- `m3_gc2_worker_scheduler` keeps the repeated bounded fixpoint
  driver private to `lj_gc2.c`; public mark completion enters through
  `lj_gc2_mark_complete()`.
- C tests still inspect the field directly for fixture assertions.

Validation:
- `tools/ci/lua_test.sh m3_gc2_paranoia` passed.
- `tools/ci/lua_test.sh m3_safepoint_handshake` passed.
- `tools/ci/lua_test.sh m3_gc2_worker_scheduler` passed.
- `tools/ci/lua_test.sh m10_generational` passed.
- `git diff --check` passed.
