# Integrated Linux performance and scalar fallback costs

The final arena/scalar runtime completes the unmodified full JIT harness with
a fork/stock geometric mean of **1.320666406** across all 15 workloads. The
interpreter again reaches its 180-second limit after six rows, so it supplies
no complete-suite aggregate. These results leave the original performance
acceptance gates open.

## Frozen full harness

All 224 tracked source and DynASM inputs match
`23c0c753b10e22924a0feb66463ca77289d0e9b6`. The normal executable is the one
used for combined functional validation, SHA-256
`4914c147ac254179610c3aeb8ba847fdc215febcbb96af81afb365487b2a1c37`.
It contains the final coalescing, JIT retirement, empty-spare reuse, and scalar
read changes. It excludes both wide-stamp experiments and the later direct
positive rooted-read candidate.

The pinned stock revision remains
`b925b3e3fc6771171602323b45fbe9fb8fc90369`, executable SHA-256
`d399449cc8cee4b0c600104a4a66fd44eeeac276c0f8571ce8204744041b5e34`.
Both builds are normal static runtimes without assertions, test hooks,
sanitizers, or profilers. The harness SHA-256 remains
`ebd0b8d53b6e7a340c90c45ad33d9bdd47acbd5418890d593d6aae127ef926a9`.

Fresh processes run stock JIT, fork JIT, stock interpreter, fork interpreter,
each on CPU 30 with a 180-second limit, `BENCH_SCALE=1`, and GC mode unset.
Every row is the minimum CPU time of five internal rounds with collection
before each round. This is one process per runtime/mode on a shared host;
functional work may run on CPUs 0–15 and frequency is not exclusively fixed.
It is a full-harness pilot, not seven independent performance repetitions.

| JIT workload | Stock ns/op | Fork ns/op | Fork / stock |
| --- | ---: | ---: | ---: |
| New string-key insertion | 114.67 | 1,794.69 | 15.65 |
| Closure creation/upvalue mutation | 73.24 | 1,598.75 | 21.83 |
| Coroutine switching | 26.69 | 236.62 | 8.87 |
| Existing-key stores | 1.98 | 1.83 | 0.92 |
| Existing-key reads | 2.05 | 2.07 | 1.01 |

The preceding coalescing pilot reported a 1.425195179 JIT geometric mean and
4,055.45 ns per closure. Its runtime predates the public MARK-scope repair and
both newer optimizations. Those separately timed full pilots do not isolate
each change's causal contribution; the allocator's matched closure pairs are
recorded with that change.

| Process | Wall seconds | Rows | Outcome |
| --- | ---: | ---: | --- |
| Stock JIT | 7.989 | 15/15 | Exit 0 |
| Fork JIT | 52.141 | 15/15 | Exit 0 |
| Stock interpreter | 31.218 | 15/15 | Exit 0 |
| Fork interpreter | 180.003 | 6/15 | Timeout, exit -9 |

The interpreter reports existing stores at 734.56 versus stock 12.66 ns/op,
new insertion at 3,729.75 versus 167.92, and hash reads at 528.75 versus 46.67.
The nine rows beginning with `tab_read_existing` are missing and remain empty
in the comparison CSV. Every process is terminal; executable hashes match
after all runs. `bench/linux-integrated-performance-2026-09-05/` preserves
the build/measurement identities, exact commands, raw output, comparison,
aggregate status, and driver. Original build artifacts remain under the
absolute paths in its metadata.

## Matched unsupported scalar-path diagnosis

The full pilot suggested a cost from unsuccessfully attempting the scalar
path before entering the general allocating metamethod chain. A separate
matched experiment uses the original scalar normal/control trees: every
tracked runtime input agrees except the meta call to the scalar helper. Both
trees are based on `d680421c` and exclude the later empty-spare optimization.
The before executable is `b91a597eb07e22250a54d3035299f2fea119654e26179fbe0304a238c6d8ae95`;
the after executable is `71e4061bbf9ff703df8aa57742e5823e225e12402cc888202d0478bee1abe7d3`.

Seven alternating fresh-process pairs per case run the same unmodified
harness filtered to that row, with JIT off and `BENCH_SCALE=0.005`. Its
permanent 8,192-key graph remains present and each row keeps five internal
rounds with GC enabled. Each process has a 30-second bound on CPU 30; all 56
processes exit 0 and final executable hashes match. This measures filtered
path costs at the smaller loop size, not full-suite history or application
performance.

| Filtered interpreter case | Before median ns/op | After median ns/op | Paired geometric ratio |
| --- | ---: | ---: | ---: |
| Existing-key stores | 652.79 | 733.41 | 1.116 |
| Hash reads | 455.37 | 571.37 | 1.255 |
| Existing-key reads | 1,074.16 | 1,267.79 | 1.178 |
| Array read/write | 248.56 | 167.99 | 0.672 |

The first three workloads contain Huge vectors or GC-valued key-array reads,
which the scalar helper deliberately refuses. Those refusals add work before
the original general read. The array workload benefits from positive scalar
hits. These observations qualify the earlier small-hit wins: they are not a
uniform improvement across ordinary table operations.

`bench/scalar-fallback-diagnosis-2026-09-05/` preserves every source identity,
the exact one-call control patch, normal build commands, all paired commands
and results, and raw output. The next candidate reuses the existing bounded
rooted reader for positive broader hits before allocating chain roots. Its
failure path must leave all original inputs unchanged, including aliased
outputs, and its GC result must retain the existing lifetime/publication
proof. The non-SMR scalar path remains necessary when the reclaimer is paused.
No candidate correctness or speed result is claimed by this diagnosis.
