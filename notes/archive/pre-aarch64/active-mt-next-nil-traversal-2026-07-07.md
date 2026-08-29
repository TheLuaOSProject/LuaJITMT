Active-MT `next(t, nil)` traversal boundary, 2026-07-07:

- A direct reopening of non-trace-local shared `next(t, nil)` under active MT
  recorded `m6_jit_token`, but immediately crashed `m5_tab_resize_jit_stress`.
- A helper-backed variant that routed the nil-start lookup through
  caller-owned `TMPREF` outputs and published GC roots still crashed the same
  stress. The observed failure was during GC2 stack scanning:
  `gc2_registered_obj_valid -> gc2_frame_func_valid -> gc2_frame_prev_safe ->
  gc2_scan_thread_stack -> gc2_scan_one_tg_roots -> gc2_scan_tg_roots ->
  lj_gc2_fixpoint_round -> lj_gc2_mark_complete -> lj_gc2_collect_active`.
- Conclusion: nil-start shared `next()` is still part of the active-MT shared
  traversal boundary. Do not reopen it independently of the broader traversal
  result/snapshot/frame contract used for optimized `pairs()`/BC_ITERN and
  shared `ipairs_aux`.
- `tests/t-jit-secondary.lua` now pins this rule by asserting that hot
  `next(shared, nil)` remains interpreted under active MT, while trace-local
  `next(t, nil)` continues to record.
