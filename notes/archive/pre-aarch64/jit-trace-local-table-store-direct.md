# JIT trace-local table-store direct lowering

Date: 2026-07-02

After `mt_active` is latched, x64 JIT table stores normally route through the
helper/CAS path so shared-table writes revalidate forwarded generations,
retiring storage, weak/metatable state, and CAS loss. That is still required
for non-trace-local tables and for `NEWREF` insertions.

The direct path is now restored for the narrow case where the store target is a
`TNEW`/`TDUP` table that has not escaped the trace before the `ASTORE`/`HSTORE`.
The assembler checks the IR between the allocation and the store and refuses
direct lowering if it sees `NEWREF`, an XPOLL/XBAR boundary, a call, or a store
that publishes the new table as a value. A table assigned to an upvalue before
the field store therefore keeps the helper/CAS route.

This keeps the stock-style lowering for private construction stores while
preserving the shared-table no-tear/revalidation contract. The behavior is
covered by `m6_jit_table_store_helper`: active-MT trace-local TDUP stores must
not contain `lock cmpxchg` or `lj_tab_storetv_forjit*`, while escaped
trace-local stores must still contain the helper/CAS fallback.
