# JIT Threaded Tmpbuf Format

## 2026-06-20

Bug:
- A four-worker `string.format()` loop crashed with JIT enabled and passed with
  `-joff`.
- The failing trace entered worker 2 with its own `lua_State`, but the compiled
  `lj_buf_tostr()` path called `lj_str_new()` through worker 1's `SBuf.L` and
  worker 1's tmpbuf storage.

Cause:
- `recff_bufhdr()` and the concat recorder emitted
  `lj_ir_kptr(J, &J2TG(J)->tmpbuf)`.
- That bakes the recording TG's temporary SBuf into shared trace IR, so another
  thread can run the trace against the wrong tmpbuf and stale `SBuf.L`.

Fix:
- Added `lj_buf_tmp_reset(L)` as a small JIT-callable wrapper around
  `lj_buf_tmp_(L)`.
- Recorded format/concat buffer headers now call the helper from runtime
  `IR_LREF` and feed the returned SBuf into `IR_BUFHDR_RESET`.
- This keeps trace code using the running TG's tmpbuf without adding a new IR
  addressing mode.

Regression:
- Added `m6_jit_tmpbuf_thread_format`, a JIT-on four-worker `string.format`
  smoke that checks first/last formatted strings per worker and keeps the hot
  format loop traced.
