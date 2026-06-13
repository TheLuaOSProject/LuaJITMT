M6 x64 dispatch localization inventory:
- `DISPATCH` now starts from the running `L->tg_hint` plus
  `offsetof(TGState, dispatch)` at x64 VM entry points. `TGPOLL` reads the
  current TG's `poll` word, so interpreter safepoint checks are TG-local.
- `GG_DISP2STATIC` is table-layout local and should remain valid once every TG
  dispatch table is synchronized from the template.
- Original hazard: `DISPATCH_GL`, `DISPATCH_J`, `TG_DISP2G`, and `TG_DISP2J`
  were main-TG-relative. Before `DISPATCH` points at arbitrary TGs, global/JIT
  slow paths must load through `tg->gl` and `g->jitp` instead of using fixed
  offsets.
- Completed prerequisite: x64 VM slow paths now load
  `global_State *` from `TGState.gl` and `jit_State *` from `g->jitp` instead
  of deriving them from fixed `DISPATCH` offsets. `vm_x64.dasc` no longer has
  `DISPATCH_GL`, `DISPATCH_J`, `TG_DISP2G`, or `TG_DISP2J` uses.
- Recording dispatch is TG-local. The global dispatch template no longer has a
  recording mode; `lj_dispatch_update()` overlays record/call hooks only onto
  the current TG when that TG holds the recorder token.
- Emitter prerequisite complete: fixed TG fields now use symbolic
  `DISPATCH_TG(...)` offsets (`jit_base`, `cur_L`, `tmptv`, `gl`) instead of
  recorder-TG pointer subtraction. Generic `dispofs()` addressing has been
  removed; arbitrary constants and object pointers now use absolute/RIP forms
  or a saved scratch fallback instead of `RID_DISPATCH`.
- `REF_NIL` GG-state FLOADs now use absolute/RIP/global-address forms, and the
  x64 exit patcher recognizes the resulting vmstate store patterns instead of
  scanning for `GG_OFS_TGDISP`.
- Transitional x64/POSIX safety guard still keeps secondary TGs from acquiring
  the recorder token as well as from entering `BC_JLOOP` mcode. This is now a
  validation guard only; the original target is to remove it after the
  non-dispatch emitter migration is exercised.
- Completed prerequisite in this slice: `lj_dispatch_update()` now requests
  `HS_REDISPATCH` when more than one TG is live, so attached TG dispatch tables
  refresh through their own safepoint acknowledgement instead of remote copying.
