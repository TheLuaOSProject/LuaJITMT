# C parser local publish and istype snapshot-wait slice

This slice reduces in-place shared `CType` mutation during parser rollback
windows and removes the non-string `ffi.istype()` parser-lock fallback without
changing public FFI behavior.

Changed:

- `cp_ctype_setsib()` and `cp_ctype_setname()` now copy the current shared
  `CType` into a local temporary, modify that temporary, then publish it with
  `lj_ctype_publish()`.
- `cp_ctype_abandon()`, `cp_ctype_abandon_id()`, and rollback restore publish
  local snapshots instead of first writing the shared slot directly.
- `lj_ctype_parse_wait()` exposes the existing parser-token park path without
  acquiring the parser token. `lj_ctype_parse_lock()` now uses that helper.
- `ffi.istype()` now retries the sequence-checked snapshot comparison when a
  parser mutation is active instead of taking the parser token after it clears.
  String declarations still parse under the parser lock before comparison.
- Removed the stale exact-source-text check in `m7_ffi_typeinfo_snapshot.sh` that
  looked for an old pointer auto-deref spelling already covered by behavior
  fixtures and broader raw-access guards.
- Removed the now-dead `ffi_istype_raw()` fallback and its source-text check.
- Extended `t-ffi-istype-snapshot.c` with an active-token behavior test. It
  holds the parser token, releases it from a helper thread, asserts the normal
  `ffi.istype()` boolean result, and checks the parser sequence advanced only
  from that helper release.

Why this matters:

- Interpreter-side FFI fallback waits still exist because the cparser can mutate
  shared CType records and roll them back while readers need normal Lua/FFI
  semantics.
- Publishing from local snapshots shrinks those mutation windows and moves the
  parser toward copy-then-publish behavior, which is a prerequisite for safely
  deleting more reader waits.
- `ffi.istype()` is the narrow reader whose snapshot path already preserves
  true/false semantics across parser-token contention, so it can be made more
  lockless now.

Not done:

- The larger struct/enum completion paths still use `cp_ctype_mut()` and need a
  separate copy-publish pass with focused rollback/reader coverage.
- Layout APIs, `ffi.new`, `ffi.cast`, cdata indexing, enum conversion,
  arithmetic, and `ffi.C` still keep their fallback waits until their snapshot
  helpers distinguish transient parser state from real misses everywhere.

Verification:

- `make -C src -j`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback m7_ffi`
- `tools/ci/lua_test.sh m7_ffi_ctype_name_claim m7_ffi_ctype_ticket_intern`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/m7_ffi_cdata_get_l.sh`
- `git diff --check`
