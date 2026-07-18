# Debian Wine 10 aborts when `TMPDIR` is explicitly set

Status: open environment issue with a complete local workaround; not a LuaJIT
runtime failure.

The first b1.2.0 Windows artifact preflight used private `HOME`, `WINEPREFIX`,
and `TMPDIR` directories. Its Wine smoke aborted with glibc
`free(): invalid pointer` before a useful Lua-level diagnostic. Repeating the
artifact build reproduced the abort, while split and later exact LuaJIT probes
without the private `TMPDIR` passed.

The minimal reducer does not involve LuaJIT:

```text
TMPDIR=<existing-private-directory> WINEPREFIX=<prefix> wine cmd /c echo OK
```

In this Debian Wine 10.0~repack-6 build, GDB resolves the abort to
`server_tmpdir()`. That function retains the pointer returned by
`secure_getenv("TMPDIR")` and later passes it to `free()`, although the pointer
belongs to the process environment/initial stack. The failure happens during
Wine server initialization, before the Windows payload enters `main`. It is
selected here because the container has no usable `/run/user/$UID` directory.

With `TMPDIR` unset, the exact combined fork smoke passed, including
`threading`, FFI `kernel32`, `GetCurrentProcessId`, and `package.loadlib`.
The common FFI/loadlib reducer passed 25/25 for both this fork and pinned
upstream LuaJIT under the same normal Wine environment. Setting the private
`TMPDIR` made even an upstream `print("STARTED")` process abort before printing.
Explicit package-before-FFI, FFI-before-package, and duplicate finalizer probes
also passed, excluding a loader-handle double-free.

Local Windows validation must use `env -u TMPDIR` (or simply avoid overriding
it). Private `HOME` and `WINEPREFIX` are safe. The rolling-release workflow does
not export `TMPDIR`, and the corrected complete artifact build, staged-binary
smoke, ZIP extraction, and extracted-binary smoke all pass.
