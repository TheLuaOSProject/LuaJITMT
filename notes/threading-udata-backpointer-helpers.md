# Threading userdata backpointer helpers

## Summary

Routed threading userdata backpointers through helper accessors:

- `lj_thread_udata_*()` owns the `LJThread.ud` payload backpointer.
- `lj_tg_*_thread_ud()` owns the `TGState.thread_ud` per-TG cache.

Thread creation, worker attach/cleanup, `threading.current()`, detach, current
thread checks, legacy userdata validation, and the focused live-root test now
use the helper surface instead of direct pointer loads/stores.

## Coverage

`m4_threading_live_root`, `m4_threading_api`,
`m4_threading_shutdown`, and `m4_threading_join_gcscan` cover the observable
behavior for these backpointers.

Verification: clean build plus the four focused suites above passed.
