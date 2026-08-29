# JIT pre-MT direct array reads, 2026-07-03

The recorder now treats published table array reads as direct only while MT has
not started entering. `rec_idx_tab_direct_array()` returns true for trace-local
`TNEW`/`TDUP` tables and for pre-MT traces; after `mt_entering` or `mt_active`
is visible, published table reads keep the active-MT helper/header safety
policy.

The safety boundary is the same one used by primitive table stores:
`mt_entering || mt_active`. Before that boundary, there is no secondary Lua
thread that can resize a table concurrently with traced code. The first
threading activation flushes all existing traces before worker Lua code runs, so
old direct-array traces do not survive into the shared-table phase.

This removes the pre-MT separated-array `TabArrayHdr.asize`/`XLOAD` cost from
ordinary stock-shaped single-threaded traces without weakening active-MT table
semantics. `m6_jit_aref_pair_boundary` now checks both sides of the boundary:
direct `TAB_ARRAY`/`TAB_ASIZE`/`ALOAD` IR before activation and
`lj_tab_gettv_forjit()` routing after activation.
