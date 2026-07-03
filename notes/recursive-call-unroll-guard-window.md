# Recursive Call-Unroll Coverage

`m6_jit_recursive_call_unroll` used to assert exact recursive trace topology:
trace 1 had to be a return trace, an `up-recursion` trace had to appear within
a fixed window, and the second run could add only a small number of traces. That
caught one historical "TRACE 1 forever" regression, but it also made the suite
depend on recorder trace-number allocation and stitch topology.

Stock LuaJIT shows wide trace-number variability for this `fib(30)` workload,
so active CI should not pin link types, trace counts, or the position of the
first up-recursive trace. The current test keeps the observable parts: the
recursive function returns correct results, records at least one trace, and
continues to run repeated recursive calls within the normal timeout. The
timeout is a coarse behavioral smoke for the old pathological re-recording
failure; exact `-jv` topology remains a manual performance diagnostic.

When recursive trace retention changes, document the reason beside the recorder
or trace-retirement code and use benchmark data for throughput regressions.
Trace graph topology, generated dumps, and trace-number windows remain local
diagnostics.
