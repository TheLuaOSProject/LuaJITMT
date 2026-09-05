# Independent review: special-userdata method guards

Reviewed source: `lj_record.c` SHA-256 `07116bc933781976c91453a9ca89a46aef4a81d80bf71ab2f6e5269383fcce87`, patch `c67caffd432a8e460a8e5feac5d265e664910a5cd5f2bc9cd0ee991b346ea084`, against base `e34282576c7df0180e8113a4cfba07fd637a36b3`.

Verdict: no source blocker found in the lj_record.c-only change. It removes an invalid immutable-method assumption and reuses the existing ordinary-userdata lookup protocol without relaxing shared-MT admission.

- The full `lj_record_mt_runtime_shared` check still rejects before sampling the receiver-to-metatable edge. This change does not claim shared-MT metatable recording or lifetime authority.
- CLibrary receiver identity specialization and every other special userdata subtype guard remain before method lookup. Builtin recorders still receive their existing specialization through ordinary metamethod dispatch.
- Non-NULL metatables now traverse the common guarded lookup: metatable FLOAD/non-NULL guard, table storage/key lookup, value type, and function identity when calling the method. A replacement root with an equivalent method may continue only through a valid new table lookup. No root identity assumption is required in that case.
- NULL metatables take the guarded absent path instead of calling `lj_tab_getstr` on NULL. Missing entries and explicit nil use the existing stable nil/miss checks. Function methods use ordinary call specialization; index tables go through another guarded indexed lookup. Other metamethod values follow ordinary indexing/call semantics or cause the existing recorder abort/interpreter error.
- `lua_setmetatable` does not flush userdata traces. The new dynamic metatable/node/method chain is therefore necessary for `debug.setmetatable`, and the patch does not rely on a flush that is absent for these receivers.
- Called monomorphic functions become KGC constants through `rec_call_specialize`; GC2 trace traversal marks these constants. Dynamic index tables and method results keep the existing common lookup/snapshot machinery. The special branch no longer fabricates an unguarded constant method.
- The recent pure-cdata loop exception is restricted to the exact cdata GG base-root/node FLOAD pair. It does not classify `IRFL_UDATA_META` as invariant across XPOLL. The generic table-store/call/XBAR/XPOLL alias checks remain in effect. No optimizer change accompanies this patch.
- The leftover buffer-index constant shortcut requires `ix->mt == TREF_NIL`. A found ordinary buffer metatable produces the non-nil metatable TRef; this proposed change does not newly admit that shortcut.

Independent nearby counterexamples are baseline bugs, not regressions or blockers in this method-guard repair:

1. A directly called captured builtin CLibrary `__index` omits runtime receiver specialization in `recff_clib_index`, so a trace warmed on ffi.C also accepts io.stdout.
2. Native ordinary namespace lookup ignores later writes to the debug environment cache.
3. Native ordinary namespace lookup ignores semantic close performed by the exposed builtin CLibrary `__gc`.

All three reproduce with baseline and proposed method-guard-only binaries. The interpreter provides the expected behavior. Exact inputs, binary/source hashes, stdout/stderr, and exits are recorded in `captured-clib-index-type-results.json` and `additional-results.json`. The direct receiver fix is being developed separately; runtime cache/lifecycle authority needs its own follow-up. These findings do not justify bypassing shared-MT recorder refusal.

This verdict is source review. ROOT owns broad normal/assert/ASan mutation validation of the exact lj_record.c change; the independent runs here are the distinct baseline counterexamples listed above.
