TG stack-dirty helper surface
=============================

Status: implemented and guarded for C-side production users.

Changes:

- Added `lj_tg_stack_dirty_epoch_*` helpers around
  `TGState.stack_dirty_epoch`.
- Preserved the existing ordering: relaxed owner-side increments when stack
  ownership changes and acquire reads for GC2 thread-root scan decisions.
- Routed the C-side owner increment and GC2 scan read through the helper layer.
- Documented the invariant formerly checked by `m3_gc2_scaffold`: raw C-side production
  `stack_dirty_epoch` access outside `src/lj_tg.h`.

Note:

- The x64 VM still has intentional inline TG memory operands for this field;
  this helper slice only guards C-side access.

Validation:

- `make -C src -j$(nproc)`
- `tools/ci/m3_gc2_scaffold.sh`
