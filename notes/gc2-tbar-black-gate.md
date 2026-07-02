# GC2 TBAR key barrier split

Traced numeric table stores can emit `TBAR` for string-key stores even when the
stored value is not collectable. On x64 the inline legacy half of `TBAR` already
uses the table black bit as the once-per-cycle gate: the first barrier clears
black and queues the table on `grayagain`; later barriers skip until traversal
makes it black again.

The GC2 table-rescan half cannot simply share the legacy black-bit gate for all
stores: a GC2-marked old table is not guaranteed to have the legacy black bit at
the point a trace runs. That matters for value stores and for key-only stores
that revive a collectable key. But routing every string-key numeric store through
`lj_gc2_barrier_tab_g()` requeues the already-marked table into SSB on each
iteration. A `t["k"..i] = i` loop therefore left mark completion with one grey
table revisit per iteration.

The recorder now distinguishes the two cases. Value/table barriers keep
`IR_TBAR.op2 == REF_NIL` and lower to `lj_gc2_barrier_tab_g()`, so non-legacy-black
GC2-marked tables still reach the table-rescan/remembered-set path. Key-only
barriers carry the key TRef in `IR_TBAR.op2` and lower to
`lj_gc2_barrier_key_g()`, which marks or remembers the key edge directly under
`TG.mark_active` without requeueing the whole table. The legacy black-table
repair remains inline and black-bit gated for both forms.

Follow-up stability fix: `lj_gc2_barrier_key_g()` now exits for weak-key
tables. Key-only TBAR is a strong-key edge repair; weak-key tables use the
existing weak-write path when the store helper has to preserve entries during
P_WEAK. The JIT barrier must not turn a weak key into a strong edge merely
because the traced value store was numeric or nil.

The regression guard is `tools/ci/lua_test.sh m6_jit_tbar_gc2_black_gate`.

Assert-build follow-up:

- `IR_TBAR` now declares the key operand as a real optional ref. Unkeyed table
  barriers use `REF_NIL`; key-only barriers carry the key ref.
- This fixes `LUA_USE_ASSERT` recorder validation for primitive-value
  `__newindex` table stores, where the keyed barrier is required but the old IR
  mode still claimed `op2` was unused.
- Validated with `m6_jit_tbar_gc2_black_gate`, `m3_gc2_paranoia`,
  `m6_jit_barrier_xpoll`, `m6_jit_table_store_helper`, and stock tests.
