FFI parse token helper surface

- Added `ctype_parse_token_acq()`, `ctype_parse_token_rel()`,
  `ctype_parse_token_cas()`, `ctype_parse_token_wait()`, and
  `ctype_parse_token_wake()` for the accepted cparse mutation-token bridge.
- Routed `lj_ctype_parse_lock()`, `lj_ctype_parse_unlock()`, ctype snapshot
  readers, and FFI type/layout snapshot readers through the helper API.
- Documented the invariant formerly checked by `m7_ffi_cdef_token`: raw implementation-side
  `cts->parse_token` access.
- Follow-up safety cleanup: the non-Linux `lj_ctype_parse_lock()` fallback now
  sleeps through `lj_thr_sleep_ns(L, ...)` and processes STOPREQ instead of
  spinning on `la_cpu_pause()`. The token guard also documents why raw pause waits in
  `src/lj_ctype.c` / `src/lib_ffi.c`, giving the FFI ctype/parser surface the
  same no-busy-spin invariant as the Linux futex path.
- Follow-up native STOPREQ cleanup: contended `ffi.cdef()` parser-token waits
  now use fresh STOPREQ semantics. A pre-existing sticky `TGF_STOPREQ` no
  longer aborts an otherwise successful cdef wait, while a STOPREQ flag that
  appears during the native wait still interrupts before the parser lock is
  acquired. `tests/t-ffi-cdef-token-stopreq.c` covers sticky-only success,
  fresh STOPREQ failure, and post-interruption recovery.

Verification:

- tools/ci/m7_ffi_cdef_token.sh
- tools/ci/m7_ffi_cparse_rollback.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- git diff --check
