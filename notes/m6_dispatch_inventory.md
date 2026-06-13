M6 x64 dispatch localization inventory:
- `DISPATCH` still starts from `GG.main_tg.dispatch` at x64 VM entry points.
  Switching it to the running `L->tg_hint->dispatch` is the remaining major
  §8.2 localization step.
- `GG_DISP2STATIC` is table-layout local and should remain valid once every TG
  dispatch table is synchronized from the template.
- Original hazard: `DISPATCH_GL`, `DISPATCH_J`, `TG_DISP2G`, and `TG_DISP2J`
  were main-TG-relative. Before `DISPATCH` points at arbitrary TGs, global/JIT
  slow paths must load through `tg->gl` and `g->jitp` instead of using fixed
  offsets.
- Completed prerequisite in this slice: x64 VM slow paths now load
  `global_State *` from `TGState.gl` and `jit_State *` from `g->jitp` instead
  of deriving them from fixed `DISPATCH` offsets. The only remaining
  `DISPATCH_GL` use in `vm_x64.dasc` is the transitional `TGPOLL` check of
  `g->gc2.hs_pending`, which must switch to `TGState.poll` together with the
  actual local-`DISPATCH` entry change.
- The JIT emitter also assumes `RID_DISPATCH` can address arbitrary constants
  relative to the recorder TG. Before trace execution is safe on secondary TGs,
  arbitrary `dispofs()` addressing must stop using `RID_DISPATCH`; only fixed
  `DISPATCH_TG(field)` offsets are valid for per-TG fields.
- Transitional x64/POSIX safety guard now keeps secondary TGs from acquiring
  the recorder token as well as from entering `BC_JLOOP` mcode. This preserves
  interpreter progress while preventing secondary TGs from producing traces
  with recorder-relative dispatch references.
- Completed prerequisite in this slice: `lj_dispatch_update()` now requests
  `HS_REDISPATCH` when more than one TG is live, so attached TG dispatch tables
  refresh through their own safepoint acknowledgement instead of remote copying.
