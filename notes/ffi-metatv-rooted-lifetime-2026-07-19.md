# Rooted FFI ctype metamethod reads (2026-07-19)

## Problem

The former `lj_ctype_metatv_snapshot()` sequence-checked CType topology, but it
read CTState metatable roots and table vectors as raw pointers and returned GC
values through unenumerated C-local `TValue`s. Parser sequencing does not retain
Lua objects or retired table storage, so a concurrent weak removal, table
generation change, or GC2 reclaim could invalidate the sampled metamethod.

## Current contract

`lj_ctype_metatv_rooted_try()` is bounded and nonwaiting. It opens GC2 SMR before
loading the current CTypeTab/topology, exact-leases each CTState/table/result
edge, uses held generation-checked table reads, rechecks the parser sequence,
and release-publishes a terminal result into a caller-owned TG root anchor.
Callers retain that anchor through their last dereference.

The status split is intentional:

- `CTBUSY`: cdef/parser topology changed or is currently being published.
- `RETRY`: SMR admission, table generation, or lease acquisition contended.
- `ABSENT` / `FOUND`: sequence-valid terminal results.

Interpreter wrappers release every lease, SMR reader, and result anchor before
waiting. Cdef remains an allowed control-plane wait; structural table retries
currently use `lj_tab_wait_l()` after all private authority has been dropped.
The recorder never waits: it aborts the recording turn on `CTBUSY`/`RETRY`.

Pointer-to-function lookup remains generic CType topology. Every function
pointer follows the shared miscmap/metatable hop; there is no signature, ABI,
argument-class, or result-class dispatch.

## Throwing paths

Table-valued cdata `__index` and `__newindex` values are transferred into the
existing Lua argument slot and published before the ctype anchor is released.
The generic rooted metamethod-chain helpers therefore run without a live
private ctype anchor. Constructor `__gc` values are likewise transferred to a
pre-reserved Lua stack slot before FINREG allocation/retry work.

Function-valued interpreter paths keep the anchor only through
`lj_meta_tailcall()`, which materializes the function in the continuation frame
without a semantic call. Recorder constant emission still occurs while the
anchor is live; recorder aborts are enclosed by the trace protected-call root
checkpoint and unwind to its saved anchor depth.

The focused fixture repeats caught table-chain loop errors and throwing nested
table metamethods, forces collection inside the nested functions, and verifies
the TG root-anchor depth after every inner `pcall`.

## Deliberate follow-up debt

- Restore tracing of table-valued cdata `__index` with a second rooted,
  nonwaiting lookup and a runtime table-generation/value guard. The recorder
  currently fails closed for that shape; interpreter semantics are unchanged.
- Replace the interpreter wrapper's post-release `lj_tab_wait_l()` retry with a
  fully helpable/yielding structural path as the generic table substrate gains
  one. This is outside the bounded ctype lookup helper.
