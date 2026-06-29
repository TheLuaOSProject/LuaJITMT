# FFI Shared Cdata Hammer

The M7 FFI target includes user-racy shared cdata field access. LuaJIT cannot
make C payload races deterministic, but the VM must stay safe: field loads and
stores must not corrupt ctype metadata, produce internal sentinels, crash during
GC, or make the cdata unusable after racing workers detach.

`t-ffi-cdata-shared-hammer.lua` shares one struct cdata object across spawned
Lua threads and hammers aligned integer fields, array elements, `_Bool`, boolean
bitfields, small integer bitfields, and a nested struct. Its assertions are
domain checks rather than last-writer checks because the user payload writes are
intentionally racy. The same script runs under interpreter and default-JIT modes.

This is runtime coverage only; it does not search repository source for helper
names or implementation snippets.
