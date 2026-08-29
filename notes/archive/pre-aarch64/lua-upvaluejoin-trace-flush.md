# Lua Upvalue Join Trace Flush

`lua_upvaluejoin()` and `debug.upvaluejoin()` replace the first closure's
upvalue cell pointer with another closure's cell. Existing traces over the
first closure may already have loaded from the old cell, so the pointer swap
must publish the same full trace-flush boundary used for other API-visible
function mutations.

The flush happens only when the cell pointer actually changes and runs before
the release-store that publishes the replacement upvalue. This keeps
interpreter behavior stock while preventing old traces from continuing to read a
stale closed-upvalue cell after `debug.upvaluejoin()`.

`m6_jit_cclosure_upvalue_flush` now also heats a Lua-closure upvalue load,
joins that upvalue to another closure's cell through `debug.upvaluejoin()`,
verifies the trace table was flushed, and checks the next call observes the
joined value. `tests/t-lua-upvaluejoin-trace.c` covers the public C API
`lua_upvaluejoin()` path directly.
