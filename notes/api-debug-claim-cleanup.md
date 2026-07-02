# API/debug state-claim cleanup

This slice fixed owner-claim leaks and crashes around debug/API operations that
inspect a foreign `lua_State`.

The main invariant is: do not enter `lj_vm_cpcall()` on an arbitrary suspended
coroutine stack. In practice that stack may be ownerless and TG-neutral, and the
protected-call entry path is not a safe way to wrap simple inspection/copy work.

Permanent shape:

- `lib_debug.c` uses claimed-state helpers for `debug.getinfo`,
  `debug.getlocal`, `debug.setlocal`, and `luaL_traceback`.
- Values copied from a target state are rooted on the caller stack before the
  target claim is dropped.
- Names derived from prototype metadata keep the owning function rooted until
  the caller has interned/copied the string.
- Public `lua_getinfo()` uses a resume claim. Ownerless foreign-state metadata,
  `f`, and `L` requests use the claimed helper path instead of a foreign
  protected call.
- `lua_xmove()` directly copies and release-publishes stack slots while both
  states are claimed; the copy path itself does not allocate and must not be
  wrapped in `lj_vm_cpcall()` on the source coroutine.

Regression coverage:

- `m5_api_debug_claim_cleanup` source guards enforce the helper/root/drop
  ordering.
- `m5_state_owner` covers public C API `lua_getinfo()` on an unowned yielded
  coroutine, including `S`, `f`, and `L`.
