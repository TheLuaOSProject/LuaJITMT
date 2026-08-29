# Windows package.loadlib native STOPREQ handling

The POSIX `package.loadlib()` path already marks `dlopen()`, `dlsym()`, and
`dlclose()` as native time and checks fresh STOPREQ actions before returning to
Lua. The Windows path still called `LoadLibraryA()` and `GetProcAddress()`
directly from VM state.

The Windows package loader now uses the same native-boundary shape:

- `LoadLibraryA()` runs under native enter/leave and preserves the failing
  `GetLastError()` value for stock-compatible error strings;
- a successfully loaded library is closed before delivering a fresh STOPREQ;
- regular C-symbol lookup and bytecode-symbol lookup both enter native state
  around `GetProcAddress()` and default-module probing;
- explicit unload already used native `FreeLibrary()` and now remains the
  common cleanup path for load-abort handling.

This mirrors the Windows FFI CLibrary loader without changing stock
`package.loadlib()` return values or registry ownership semantics.

Coverage:

- Windows platform/release binary smoke resolves
  `package.loadlib("kernel32.dll", "GetCurrentProcessId")` without calling the
  foreign function as a Lua C function.
