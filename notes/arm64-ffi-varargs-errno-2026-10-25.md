# Apple ARM64 variadic ABI and internal errno preservation (2026-10-25)

The refreshed bootstrap gate reached the aggregate FFI fixture but intermittently
failed its large-struct result assertion. The returned fields were correct; a
native diagnostic showed `errno` changing from 73 immediately after the foreign
return to 2 while Lua boxed the three 64-bit fields. The metadata wait and
subsequent automatic collector progress can issue Darwin kernel waits/wakes.
Those internal calls must not replace the foreign call's OS error state.

`lj_ctype_parse_wait()` and `lj_ctype_parse_unlock()` now preserve the existing
`errno`/Windows LastError pair through the shared `ERRNO_SAVE` macros. Automatic
GC entry points preserve the same state, including interpreter-top assistance
and trace-side progress. The save/restore is outside the GC protocol and adds no
lock or shared authority. It is compiled out when FFI is disabled. Fresh-stop
handling in the parser wait sees the restored state before any unwind.

A separate native variadic probe exposed incorrect Apple stack packing: typed
integers 11, 22, 33 supplied to `snprintf("%d %d %d", ...)` appeared as 11, 33,
and an unrelated stack value. The ARM64 stack alignment code replaced the
variadic flag with the aggregate flag. Keeping both flags (`isva |= ...`) retains
8-byte unnamed argument slots while fixed narrow arguments stay packed. This
is confined to the existing Apple ARM64 branch. Struct cdata still decays to a
pointer for variadic calls, matching stock LuaJIT's existing limitation.

The aggregate fixture now checks promoted narrow integers, alternating
integer/floating/pointer variadics, unnamed arguments after packed named stack
arguments, and fixed narrow stack arguments against native C ABI oracles. It
retains the errno assertion after all returned fields have been boxed. Direct
checks cover real parser timeouts with and without a Lua owner, wake with no
waiters, and forced automatic collector progress.

Validation used a clean private native bootstrap/assertion build of `8a2bd465`
plus this source/test change. The complete aggregate fixture passed. Three
independent negative controls substituted the prior object for only one changed
source file while retaining the other fixes:

- Prior `lj_ccall.c`: failed the new variadic integer oracle.
- Prior `lj_ctype.c`: failed the actual parser-wait errno assertion.
- Prior `lj_gc.c`: failed the forced automatic-GC errno assertion.

These are targeted runtime results; the complete bootstrap/experimental gates
and x86-64 compatibility checks are recorded separately once rerun.

References: Apple's [ARM64 calling convention](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms)
and LuaJIT's [ffi.errno contract](https://luajit.org/ext_ffi_api.html#ffi_errno).
