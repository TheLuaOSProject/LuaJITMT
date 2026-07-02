# Fresh STOPREQ pending poll

`lj_safepoint_fresh_stopreq()` now consumes a pending STOPREQ request before
testing freshness when the caller's action bits do not already include
`LJ_GC2_HS_STOPREQ`.

This makes the post-native STOPREQ window consistently covered for every local
native wrapper that delegates to the shared freshness predicate, including
clib, package/load, ctype waits, PRNG seeding, profiler paths, debug/print I/O,
and FFI callbacks.

The predicate only polls when the TG request mask contains STOPREQ, or when the
TG poll word is set and the current handshake action mask contains STOPREQ. The
normal sticky STOPREQ behavior still depends on the caller's `had_stopreq`
snapshot.

Focused guards:

- `tools/ci/lua_test.sh m3_safepoint_handshake`
- `tools/ci/lua_test.sh m7_ffi_ccall_native`
- `tools/ci/lua_test.sh m8_weak`
