2026-06-26

- Replaced Lua-visible runtime `strerror()` formatting with
  `lj_err_strerrno()`, which uses `strerror_r()` on POSIX targets and a
  caller-owned scratch buffer before falling back to the legacy non-POSIX
  path.
- Converted `luaL_fileresult()`, `luaL_loadfilex()` open/read errors,
  `io.open()` argument errors, and pthread creation errors. These paths now
  snapshot the failing errno/status before safepoint checks or Lua stack work.
- Added `tests/t-libc-error-reentrant.lua` and
  `tools/ci/m5_libc_error_reentrant.sh`; the notes document why raw production
  `strerror()` use outside the helper and stresses missing-file errors from
  overlapped child TGs.
