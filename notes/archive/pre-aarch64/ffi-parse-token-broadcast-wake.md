# FFI Parse-Token Broadcast Wake

Date: 2026-07-02

`lj_ctype_parse_unlock()` now wakes every waiter on `CTState.parse_token` after
publishing the next even parser sequence. The wait set contains both parser
token acquirers and stable readers that only need to re-check the sequence. A
single wake can unblock one reader while leaving the rest parked until their
STOPREQ polling timeout, even though the parser mutation is already visible.

The cdef parser remains serialized as planned. This only removes the
post-publication waiter tail for FFI readers and follow-on parser calls. The
test fixture helper mirrors the production broadcast so parser-token snapshot
fixtures do not bake in single-wake behavior.

Coverage: `m7_ffi_cdef_token` now starts multiple waiters on the odd parser
sequence, unlocks through `lj_ctype_parse_unlock()`, and requires every waiter
to return without relying on the timeout fallback.
