# JIT CGET Numeric-For Integer Shape

The `tab_hash_write` benchmark regressed mostly because side traces inside the
function-shaped loop recorded source-local `CGET` of the numeric-for variable
as `SLOAD NUM`, then lowered `i % 8192` through `FPMATH floor` and `TOSTR NUM`.
Stock LuaJIT records the same loop as integer `BAND` plus `TOSTR INT`.

`rec_for_ext_cget()` restores that shape only for the visible `FORL_EXT` slot
of an active numeric `for` loop and only when `lj_opt_narrow_forl()` says the
runtime start/stop/step values are integer-safe. It converts the already
recorded `CGET` value with the normal checked int-from-number conversion, so
true floating loops keep the numeric modulo path and exits inside the loop do
not reload an advanced raw FORL slot.

The focused coverage is `m6_jit_tmpbuf_thread_format`, which now dumps a
function-local `for i = 1, n` table-write loop and requires side traces to use
`BAND` and `TOSTR INT` with no `FPMATH`/`TOSTR NUM` widening.

An attempted pre-MT private `NEWREF` insertion helper was not kept. After the
assembler call-site was made active, the helper could spin on a simple
preinterned-key table-write loop. The safe performance target for this slice is
the recorder type-shape bug above, not a second table insertion protocol.
