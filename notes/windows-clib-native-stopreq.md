# Windows CLibrary native STOPREQ handling

The POSIX FFI C-library path already entered native time around `dlopen()`,
`dlsym()`, and `dlclose()`, then checked fresh STOPREQ actions after returning
to the VM. The Windows path used `LoadLibraryA()` and `GetProcAddress()`
directly from the mutator.

That was a platform-specific nonblocking gap: a thread stopped in the Windows
loader or symbol lookup could remain in VM state instead of native state, unlike
the Linux/macOS path.

The Windows branch now:

- wraps `LoadLibraryA()` in native enter/leave and preserves the failing
  `GetLastError()` value for stock-compatible FFI errors;
- closes a successfully loaded handle before delivering a fresh STOPREQ;
- uses the same native enter/leave protocol around default-library resolution
  and `GetProcAddress()`;
- keeps `FreeLibrary()` on the native path through the same helper used by
  unload and load-abort handling.

Coverage:

- `LJ_M0_PLATFORM_ENABLE=1 tools/ci/lua_test.sh m0_platform_cross_smoke`
  cross-builds Windows UCRT, runs under Wine, and now exercises
  `ffi.load("kernel32")` plus `GetCurrentProcessId()` symbol resolution.
- The same Windows FFI-load smoke is also used by release binary checks and
  platform CI smoke code.
