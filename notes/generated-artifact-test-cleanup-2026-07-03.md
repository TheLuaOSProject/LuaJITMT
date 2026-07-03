Generated-artifact test cleanup
================================

Removed the active test layer that failed CI based on LuaJIT dump text,
generated IR markers, helper names, or x64 mcode snippets. Those checks were
useful while bringing individual JIT paths online, but they made the suite
enforce implementation shape instead of the lockless invariant itself.

Current coverage keeps the observable requirements:

- runtime behavior and stock Lua/LuaJIT semantics for table stores, local-cell
  upvalues, FFI allocation/finalizers, IO, threading fast functions, and
  string buffers;
- trace-existence checks through `jit.util.traceinfo()` where the scenario is
  specifically about staying traceable;
- C fixtures and stress tests for ownership, publication, GC accounting,
  flush/race safety, and `mt_entering` behavior.

Implementation-route requirements such as why a path uses a helper, an acquire
load, a poll boundary, a TG-local buffer, or an inline allocation predicate
belong beside the implementation and in notes. They should not be reintroduced
as pass/fail checks over source text, dump text, helper spelling, or mcode
bytes. If a route matters for performance, cover it with benchmarks or focused
runtime counters; if it matters for safety, cover the behavior that would break
when the route is wrong.
