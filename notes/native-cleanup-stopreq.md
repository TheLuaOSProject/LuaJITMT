# Native cleanup after STOPREQ

Date: 2026-06-20

## Problem

Several native wrappers correctly acknowledged STOPREQ while performing a
blocking operation, but then cleaned up successful resources with raw blocking
calls before throwing the shutdown interruption:

- `io_fopen_checkstop()` used raw `fclose()` on an opened file.
- `io_tmpfile()` used raw `fclose()` on the temporary file handle.
- `os_native_mkstemp()` used raw `remove()` for the just-created temp path.

These paths are short, but they are still filesystem/stdio operations and can
block on unusual filesystems or descriptors.

## Fix

- Moved `io_native_fclose()` before `io_fopen_checkstop()` and reused it for
  STOPREQ cleanup closes, OR-ing close-time actions into the pending action set.
- Split `os_native_remove()` into an action-returning
  `os_native_remove_action()` plus the public checkstop wrapper, and reused the
  action-returning form for `os.tmpname()` cleanup.

Follow-up consistency slice:

- `io.tmpfile()` and POSIX `os.tmpname()` now snapshot the pre-existing sticky
  `TGF_STOPREQ` bit before entering native state and only run temp-resource
  cleanup plus throw on a freshly observed STOPREQ. This matches the existing
  `io.open`, `io.popen`, `fwrite`, and close semantics: a caught/sticky
  shutdown flag does not interrupt later cleanup calls, but a native leave that
  acknowledges a new shutdown still closes/removes the just-created temporary
  resource before throwing.
- The safepoint handshake harness now has explicit sticky-temp coverage and
  post-clear assertions that each expected STOPREQ case leaves no pending
  shutdown flag/poll state behind.

## Verification

Passed:

- `tools/ci/lua_test.sh m3_safepoint_handshake`
- Direct rerun of `/tmp/lj_t_safepoint_handshake` with the loadlib STOPREQ
  fixture.
