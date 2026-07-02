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
- Public load/call entry points that can enter VM protected frames
  (`lua_loadx`, `lua_call`, `lua_pcall`, `lua_cpcall`) hold resume claims for
  ownerless coroutine states. `lua_call` uses a protected VM call only for a
  claim it must release before rethrowing; already-owned current-state calls
  keep the direct VM call path.
- Public metamethod-facing APIs (`lua_equal`, `lua_lessthan`, `lua_concat`,
  `lua_gettable`, `lua_getfield`, `lua_settable`, `lua_setfield`, and
  `luaL_callmeta`) hold resume claims around stack inspection/mutation and route
  VM calls through `api_vm_call_claimed`.
- Public stack APIs in the first stack-manipulation group (`lua_gettop`,
  `lua_checkstack`, `lua_settop`, `lua_remove`, `lua_insert`, `lua_pushvalue`)
  acquire state claims before stack access. Stack-growth paths use resume claims
  and protected growth; stack mutation paths release-publish adjusted slots
  before dropping ownerless claims.
- Public read-only stack getter/conversion APIs (`lua_type`,
  `lua_iscfunction`, `lua_isnumber`, `lua_isstring`, `lua_isuserdata`,
  `lua_rawequal`, `lua_tonumber`, `lua_tonumberx`, `lua_tointeger`,
  `lua_tointegerx`, `lua_toboolean`, `lua_tocfunction`, `lua_touserdata`,
  `lua_tothread`, and `lua_topointer`) acquire state claims before reading stack
  slots. The claimed region is read-only and preserves stock conversion results;
  a genuinely busy foreign state reports `thread busy`.
- Auxiliary check/conversion APIs that only inspect existing stack values
  (`luaL_checkany`, `luaL_checknumber`, `luaL_optnumber`,
  `luaL_checkinteger`, and `luaL_optinteger`) use the same read-only claim
  shape and drop the claim before raising argument errors.
- Raw object getter APIs that read and/or publish result slots (`lua_rawget`,
  `lua_rawgeti`, `lua_getmetatable`, `lua_getfenv`, and `lua_next`) claim the
  target state before stack access and release-publish produced stack slots
  before dropping ownerless claims. Push-style raw getters use protected
  one-slot growth through `api_checkstack1_claimed()`, which drops the resume
  claim before reporting stack allocation errors.
- `lua_xmove()` directly copies and release-publishes stack slots while both
  states are claimed; the copy path itself does not allocate and must not be
  wrapped in `lj_vm_cpcall()` on the source coroutine.

Regression coverage:

- `m5_api_debug_claim_cleanup` source guards enforce the helper/root/drop
  ordering and the stack/getter/load/call/metamethod resume-claim boundaries.
- `m5_state_owner` covers public C API `lua_getinfo()` on an unowned yielded
  coroutine, including `S`, `f`, and `L`, plus unowned `lua_loadx`,
  `lua_call`, `lua_pcall`, `lua_cpcall`, public metamethod APIs, the first
  stack-manipulation group, the read-only stack getter/conversion group, and
  the read-only auxiliary check/conversion group, and the raw object getter
  group.
