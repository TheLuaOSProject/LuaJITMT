# Progress report - 2026-06-28 threading CI behavior

Historical note: this report predates the current invariant-testing model. The
current guidance is `notes/ci-invariant-testing.md`: behavior fixtures and
generated artifacts cover observable semantics; implementation-only rules live
in comments and notes.

Scope: x86_64/Linux `v2.1` lockless LuaJIT fork. Project priority remains
safety, stability, and Lua semantics over LuaJIT performance parity when those
goals conflict.

## Status estimate

Overall completion remains roughly 65-75%.

- Runtime lockless substrate: 70-80%.
- Threading/channel/shutdown behavior coverage: 75-85%.
- Test/CI migration from legacy wrappers to behavior fixtures:
  50-60%.
- FFI concurrency and fallback-lock cleanup: 55-65%.
- Weak/finalizer/generational GC completion: 50-65%.
- Release-quality soak and benchmark readiness: 45-55%.

Near-term remaining effort is still measured in focused weeks, not days:
roughly 3-5 engineering weeks to release-quality confidence if no deep GC/FFI
ordering bug appears. A significant FFI or weak/finalizer ordering bug would
add 1-3 weeks.

## Done in this slice

- Fixed the `m4_threading_capi` baseline hang.
- Converted the join-result STOPREQ implementation-shape assertion into behavior coverage:
  `tests/t-threading-capi.c` now publishes a fresh STOPREQ from a helper thread
  only after the joining thread has entered the native wait.
- Added behavior coverage for blocked `threading.mutex:lock()` fresh STOPREQ
  delivery, replacing the old shell grep for bounded mutex futex waits.
- Added a `20s` timeout to `m4_threading_capi` so a future regression fails
  diagnostically instead of hanging CI.
- Later cleanup removed the pure `tools/ci/m4_threading_capi.sh` and
  `tools/ci/m4_threading_api.sh` aliases. Run these through
  `tools/ci/lua_test.sh m4_threading_api m4_threading_capi`.

The important fixture correction: setting sticky `TGF_STOPREQ` before entering
`luaMT_join()` was not a valid test for fresh STOPREQ semantics. The runtime is
supposed to ignore stale pre-existing STOPREQ state for blocking operations
that snapshot their baseline. The behavior test now interrupts after the wait
begins.

## Verification

Passed:

- `git diff --check`
- direct debug fixture:
  `cc -std=gnu99 -O0 -g -Wall -Wextra -Werror -mcx16 -Isrc tests/t-threading-capi.c src/libluajit.a -lm -ldl -pthread -o /tmp/lj_t-threading-capi-g && timeout 20s /tmp/lj_t-threading-capi-g`
- `tools/ci/m4_threading_api.sh`
- `tools/ci/m4_threading_capi.sh`
- `tools/ci/lua_test.sh m4_threading_shutdown`
- `tools/ci/lua_test.sh m4_threading_upvalue`
- `tools/ci/lua_test.sh m5_upvalue_publish_gc`

Note: the two wrapper tests were also accidentally started in parallel and
passed, but the clean verification above was rerun sequentially because this
repo can race build outputs when clean/building tests share a checkout.

## CI/test audit result

Removed/refactored now:

- M4 threading join-result wait assertions.
- M4 threading mutex bounded-wait assertion.
- M4 C API attach-order assertion, now covered by the existing entering
  attach/lua_close behavior fixture.

Historical high-priority legacy/static assertion areas:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh`: broad raw `CType` implementation-shape
  assertions; superseded by snapshot behavior fixtures and documented
  invariants.
- `tools/ci/m7_ffi_blocking.sh`: behavior coverage exists for callback
  blacklist and blocking API outcomes; the wrapper assertion was part of the
  old static cleanup.
- `tools/ci/m5_profile_stop_native.sh`: C fixture covers most semantics; keep
  at most a narrow cleanup-order lint.
- Historical `tools/ci/m5_tab_store_waits.sh`, `m5_metadata_store_waits.sh`,
  and `m5_gc_waits.sh` helper-name rules are documented; keep expanding
  contention behavior tests for the same risks.
- M6 mcode/native STOPREQ and JIT IO wrappers that once pinned helper names
  have been superseded by behavior and generated-output checks.
- Large GC/finalizer wrapper behavior belongs in suite cases and C fixtures,
  not lint helpers.

Current rule: observable semantics belong in behavior/generated-artifact tests.
Non-observable memory-order, ABI, or temporary migration contracts must be
documented near the helper or in notes.
and reviewed there; observable failures need behavior, stress, or generated
artifact coverage.

## Locks outside `ffi.cdef`

No broad global VM lock was found outside the intentionally exposed sync APIs.
Remaining coordination is mostly CAS ownership, futex/native parking, and
short sentinel claims.

Good reasons to keep:

- `threading.mutex`: user-facing synchronization primitive.
- Channel send/receive and thread join waits: blocking semantics plus STOPREQ
  and native-state delivery.
- Per-`lua_State` owner claims: prevent concurrent mutation of one coroutine.
- Safepoint handshake leadership: global GC/shutdown/redispatch/trace-flush
  coordination.
- GC2 worker lifecycle token: rare start/stop correctness path.
- GDBJIT descriptor lock: debugger-facing publication.
- Parser/FFI mutation serialization, including some non-`ffi.cdef` type graph
  readers that still use conservative fallback locking.

Best lockless work still worth doing:

- Reduce FFI read fallback locks one stable snapshot helper at a time.
- Add stronger behavior/stress coverage around table resize forwarding and
  table claim waits before trying to simplify wait paths.
- Keep traced FFI calls disabled until x64 trace-side native enter/leave and
  fresh STOPREQ behavior are proven.

Not worth doing now:

- Removing `threading.mutex`, channel/join waits, state owner claims, safepoint
  leadership, GDBJIT locking, or GC2 worker lifecycle serialization. Those are
  semantic safety mechanisms, not performance-only locks.
