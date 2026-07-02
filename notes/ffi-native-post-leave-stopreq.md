# FFI native post-leave STOPREQ poll

FFI native helpers now re-poll a pending STOPREQ before testing freshness when
`lj_native_leave()` returns no action bits. This closes the window where a
request can be published after the native leave poll and before the helper's
post-native check.

The fix mirrors the threading native-check pattern for both FFI C calls and
FFI library memory helpers. It checks the TG request mask first, then the
poll/global-action pair, and calls `lj_safepoint_poll()` only when the leave
actions did not already include STOPREQ.

Focused guard:

- `tools/ci/lua_test.sh m7_ffi_ccall_native`

Broader verification:

- `tools/ci/lua_test.sh m7_ffi m8_weak`
