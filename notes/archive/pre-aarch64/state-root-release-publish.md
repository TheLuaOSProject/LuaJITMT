# State root release publication

After adding acquire helpers for immutable main/vm thread roots, the initial
state setup still used raw `setgcref()` stores for those roots and the legacy GC
root list head. This slice pairs the acquire readers with release publication in
startup code.

Changed in `lj_state.c`:
- initial `L->env` uses `setgcrefrel()`, matching later environment updates;
- `g->mainthref`, `g->vmthref`, and `g->gc.root` use `setgcrefroot()`;
- close-time root assertion reads `g->gc.root` with `gcref_acq()`.

The remaining raw `mainthread()`/`vmthread()` macro definitions are intentionally
left for legacy/static uses; runtime code uses acquire helpers where needed.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m5_state_owner.sh`
- `tools/ci/m4_threading_api.sh`
- `tools/ci/m3_gc2_paranoia.sh`
