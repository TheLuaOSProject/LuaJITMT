# JIT flush/full-GC active stack stability (2026-07-04)

## Finding

A poisoned benchmark sequence (`jit.flush()`, explicit full collections, a
closure allocation loop, then `threading.gcstats()`/`string.format`) could
corrupt live source literals. A compact reproducer showed the stock semantics
failure directly: two strings with identical bytes (`"ns="`) compared unequal.

The immediate cause was a live proto constant being swept from the string table
while its old pointer remained reachable from the active call frame. A later
substring/format operation re-interned the same bytes as a different `GCstr`,
breaking LuaJIT's pointer-equality assumption for interned strings.

## Fix

The legacy stack scanner now treats frame functions and function/proto stack
values as root edges even when their legacy color is already non-white. This is
needed because SMR preservation can keep a root body alive without proving that
its children have been visited in the current legacy mark cycle.

The active executing stack is also no longer shrunk during GC. Current C/JIT
return state can still name the existing stack range; the stack may shrink after
it stops being the active TG stack.

JIT flush retirement roots were tightened at the same time:

- retired trace vectors and mcode retire records are marked across publication;
- retired trace bodies preserve KGC operands and snapshot/start protos;
- minor GC2 root scans now include the same JIT roots as major scans.

## Regression Coverage

Added `m6_jit_flush_gc_current_stack`, which keeps stock string equality
assertions in the formerly failing sequence.

Passing focused checks:

- `tools/ci/lua_test.sh m6_jit_flush_gc_current_stack m6_jit_recursive_call_unroll m6_jit_flush_hs m6_jit_tmpbuf_thread_format`
- `tools/ci/lua_test.sh m6_jit_fnew_bump`
- `make -C src clean`, `make -C src -j$(nproc)`
- `git diff --check`

`tools/ci/lua_test.sh m9_m10_gc` still fails in `m9_bench_stock_compare` on
`tab_store_existing` at 4.65x vs stock with a 3.0x threshold. Earlier subcases
in that suite passed; this is the existing table-store performance gap, not the
active-stack/string-literal stability failure.
