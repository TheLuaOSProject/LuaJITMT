# `jit.util.tracesnap` snapshot positions (2026-07-19)

Stock LuaJIT accepts `jit.util.tracesnap(tr, sn, getpos)`.  The lockless reader
cutover had retained only the two-argument result, which broke the stock API
contract and compatible tooling that requests the owning bytecode position.

The optional third argument is now handled with normal Lua truthiness.  Omitted
or false returns the snapshot table alone; truthy returns the table followed by
the exact snapshot PC offset.  The snapshot map and backwards function-header
scan are completed inside the same one-shot trace SMR/token admission.  Only a
bounded scalar offset survives that interval, and bytecode words are read with
acquire loads.  Allocation and table publication remain outside the admission.

The C fixture compares every published snapshot with an internal exact oracle,
requires a nonzero offset, covers omitted/false/true/truthy-zero forms, and
forces failed SMR admission to prove no result and no leaked reader/token.  The
concurrent publication/flush test now probes the paired result as well.

Focused validation passed `m6_jit_util_tracesnap_getpos`, a concurrent
`m6_jit_util_flush_race` run, and `tools/ci/nonblocking_jit_smr_gate.sh`.

While developing the fixture, repeatedly looking up the closure through a
global table after the hot workload could enter `lj_tab_wait_l` from the public
C API.  The fixture roots the closure directly instead.  This is evidence of
the already tracked structural-table/C-API wait debt, not a tracesnap reader
dependency; removing those waits remains b1.2.1 work.
