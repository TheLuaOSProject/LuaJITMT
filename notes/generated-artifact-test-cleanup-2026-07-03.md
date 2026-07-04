Generated-artifact test cleanup
================================

Removed the active test layer that compared LuaJIT internal generated output.
Those checks were useful while bringing individual JIT paths online, but the
suite now covers the lockless invariant itself.

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
belong beside the implementation and in notes. If a route matters for
performance, cover it with benchmarks or focused runtime counters; if it
matters for safety, cover the behavior that would break when the route is
wrong.

Follow-up: bytecode dump tests now use dump/load execution as the observable
contract. Active tests must not parse dump bytes, patch generated opcodes, or
compare exact `string.dump()` blobs; malformed-layout rules belong beside the
bytecode reader/writer unless they can be exercised through a real public
artifact.
