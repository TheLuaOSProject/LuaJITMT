# TG null fallback and finalizer guard

Clean standard and amalgamated builds exposed GCC `-Wstringop-overflow`
diagnostics where inlining could see a null `global_State *` reaching TG
fallback helpers or GC2 finalizer hook state.

`lj_tg_cur_L()` and `lj_tg_jit_base()` now keep the intended TLS/TG-first
behavior for `g == NULL`, but stop before touching the transitional global
mirrors if no matching TG exists. The store helpers also update the TG when
available and skip the mirror when there is no global state.

`gc2_call_finalizer()` now owns its input contract locally and returns before
claiming state or reading hooks if any required object is missing.

Verification:

* clean default build
* clean amalgamated build, with the prior null/provenance warnings gone
* `m4_thr_substrate`
* `m3_gc2_worker_scheduler`
* `m3_gc_active_thread_roots`
* `m8_weak`
* `m8_finalizer_error_native_stdio`
* `run_stock_tests -- --quiet`
