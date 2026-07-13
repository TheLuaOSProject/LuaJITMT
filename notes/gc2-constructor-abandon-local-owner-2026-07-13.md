# GC2 constructor abandon uses local owner authority

## Failure

The assertion build intermittently stopped in `lj_trace_free_unpublished()`
with `unpublished trace construction ownership lost` during threaded trace
flush churn. The trace still belonged exclusively to the recorder and had not
published a trace slot, retire epoch, or retire-list link.

A deterministic version of the race held the IDLE opportunistic SMR writer and
called the real constructor-abandon operation on that trace. The old path
reported `LOST` with this exact state:

```
phase=IDLE smr_reclaiming=1 smr_readers=0 huge=0 owner=1
flags=TRAVERSABLE|REGISTERED cell=2445 root=LINKING
lifetime=CONSTRUCT block=1 ready=1
```

Thus no second construction, sweep owner, or stale allocation had won. The
false result came from `lj_gc2_mem_registered_known()`: its tactical
`lj_gc2_smr_read_try()` correctly refused admission while the unrelated writer
was active, but constructor abandon incorrectly interpreted that ordinary
contention as loss of the exact allocation.

## Ownership rule

`lj_mem_abandon_gco_unpublished()` is not a general pointer-validation API.
Every caller retains the exact allocator-issued base and the unique
`CONSTRUCT+LINKING` owner until this cancellation. Published, discovered, or
stale pointers are outside its contract. That local construction lane already
pins the arena incarnation, so rediscovering the allocation through the global
TG/arena registry adds no authority and makes a nonblocking operation depend on
an unrelated SMR gate.

The implementation now validates the immutable arena owner with
`gc_constructor_allocd_at()`. This owner-local route recognizes the current TLS
TG or the state-lifetime-stable main TG without a registry scan:

- small allocations additionally require the exact cell start, a traversable
  arena, and the allocation-start bitmap before changing
  `LINKING+CONSTRUCT` to `NONE+LIVE`;
- huge allocations use only that owner's HugeTab and its full-slot
  `LINKING -> NONE` completion, preserving deferred-free folding and the
  `LIVE/SWEEP/LOST` result contract.

Generic destructor admission and published-object validation remain unchanged;
they still require the appropriate SMR or exact-reclaimer authority.

## Regression

`t-jit-trace-retire` now holds the IDLE SMR writer with zero readers and checks
the real abandon operation for both allocation classes. It proves that:

- a dedicated small compact trace moves from `LINKING+CONSTRUCT` to
  `NONE+LIVE` without changing the typed GC2 activation, then is directly
  freed at its exact allocation size. The separate semantic-retirement trace
  remains in `LINKING+CONSTRUCT`, so its later `lj_trace_free_unpublished()`
  call performs the one and only abandon allowed by the API contract;
- a huge root-construction allocation cancels through its owner-local HugeTab
  under the same writer contention, also without changing activation. Huge
  cancellation is one-shot, so the fixture then directly frees the rootless
  exact allocation instead of repeating abandon.

Verification:

```
LUA=src/luajit tools/ci/lua_test.sh m5_jit_trace_publish
```

The trace-vector, mcode-retirement, trace-retirement, and Lua trace-publication
checks all pass.
