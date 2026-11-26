# Free-bin capacity: performance and retained-memory controls

All 104 performance invocations pass with empty stderr. The candidate exposes all free capacity through ordinary frees, substantially reduces bulk-free cost, and improves the focused allocation cycles. The initial 4 KiB timing spread and a later potentially overlapping follow-up are preserved. A final repeat with the CPU 8/9 physical core reserved shows a narrow improvement. These Linux x86-64 controls compare the preceding small-shrink runtime with the bin-capacity candidate; they do not establish stock parity or whole-runtime lock freedom.

Baseline: committed `c106cd7bb109e9705c6b753f794001627320ab7f`, immutable `/tmp/lj-plain-shrink-final-20261126-lxy_jr_l/gcc`. Library SHA256 `0e514cd5c35218c6ff012b60589739c3f49e28fd0272b4beff3c84e42d0ed894`; LuaJIT SHA256 `aa15b67e1a1764714dd0d2e82623be9b53fe3f29b24d92db295faab19fb16dcf`.

Candidate: immutable `/tmp/lj-plain-bin-capacity-20261126-wyqryfj8/gcc`. Library SHA256 `2a29b71710786af7ee2f8a06eb402e1923ea72e31fba6d4d7ee390d05a7b0b28`; LuaJIT SHA256 `f6e978cb0875791b216df3608530e3d1180b121dc457c48e11f87fd66212365d`. Only `src/lj_arena.c` differs among runtime source inputs. Both builds use the same recorded GCC mode and flags.

Ordinary PLAIN allocate/touch/free cycles use the unchanged preceding `hot.c`, five adjacent alternating baseline/candidate process pairs per size, 500,000 measured cycles after 5,000 warmup cycles. Times below are independent process medians; ratios are medians and full ranges of adjacent pairs, not ratios of independent medians.

| Bytes | Baseline ns/cycle | Candidate ns/cycle | Median paired ratio | Full ratio range |
| --- | ---: | ---: | ---: | ---: |
| 64 | 1369.162 | 293.841 | 0.2145 | 0.1758–0.2338 |
| 256 | 596.456 | 307.974 | 0.5187 | 0.3287–0.8802 |
| 4096 | 751.230 | 708.747 | 0.9613 | 0.7598–1.3056 |

The broad initial 4 KiB spread prompted a bounded follow-up using the same frozen executables and 2,000,000 cycles per process. Root subsequently identified possible concurrent diagnostic/compilation activity on SMT sibling CPU 9 during that first longer set. Those ten samples remain explicitly labeled `potential_sibling_overlap`: independent medians 736.078/691.491 ns, median paired ratio 0.9081, range 0.8257–0.9855. They are not treated as an isolated confirmation.

After root and the diagnostic agent confirmed CPU 9 idle and moved further work to CPUs 0–7, five fresh longer pairs ran on CPU 8 with core 4 reserved. Independent medians are 722.285/691.273 ns; paired ratios are 0.9604,0.9537,0.9573,0.9569,0.9624, median 0.9573 and range 0.9537–0.9624. Every pair favors the candidate by roughly 4%. UTC start/end times and CPU topology are recorded. Neither follow-up is pooled into the primary table; every original raw sample remains. The original 84-row matrix completed before the sibling diagnostic according to message chronology, rather than a continuous CPU 9 activity trace.

The new bulk-free control allocates real 1 KiB bodies separated by live 16 B guards and consumes the final bump tail through actual allocations. It separately times owner frees and refill allocations; bin census and address/content checks occur outside the timed loops. Three pairs per kind/count preserve the same workload between versions.

| Targets | Kind | Baseline ns/free | Candidate ns/free |
| --- | --- | ---: | ---: |
| 1024 | PLAIN | 1122.6 | 309.1 |
| 4096 | PLAIN | 4051.1 | 319.7 |
| 9000 | PLAIN | 9362.5 | 313.8 |
| 1024 | TRAVERSABLE | 1899.3 | 218.8 |
| 4096 | TRAVERSABLE | 7213.0 | 221.6 |
| 9000 | TRAVERSABLE | 23709.4 | 229.5 |

At 9,000 targets, paired free-cost ratios are 0.03348 (range 0.03341–0.03416) for PLAIN and 0.00969 (0.00954–0.00991) for TRAVERSABLE. Baseline retains/reuses only 8,193 targets and grows from 170 to 185 arenas; candidate retains/reuses all 9,000 with 170 arenas throughout. Every smaller case reuses all targets in both versions. Refills pass all payload and guard checks; their timing and full paired variation are in `summary.json` and `bulk-summary.csv`.

The unchanged retained-shrink workload runs three paired samples of each maximum configuration:8,192 original 4 KiB blocks or 2,048 original 16 KiB blocks, all shrunk to 16 B, then filled out with 1 KiB allocations. Both versions still retain 33,554,432 physical bytes after shrink, against logical sizes of 131,072/32,768 B. Their refill footprints are identical in arena count:1,062/1,176 arenas. The live-shrink retention tradeoff remains, including up to 1,024 times the new logical size in the 16 KiB-to-16 B case.

After freeing every original and refill allocation, both versions report zero allocated bytes and zero block starts. Ordinary candidate bins now account for complete usable arena capacity without rebuild or transfer:

| Original size | Arenas | Baseline bin bytes | Candidate bin bytes | Bump bytes | Complete usable bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| 4 KiB | 1062 | 8,797,056 | 59,127,680 | 4,480 | 59,132,160 |
| 16 KiB | 1176 | 8,840,832 | 65,462,912 | 16,768 | 65,479,680 |

Candidate bin plus bump bytes equal exactly `arenas × 55,680` in every sample. Median bulk-free stage CPU times fall from 2.432235 s to 0.011200 s for the 4 KiB configuration, and 1.334668 s to 0.011658 s for 16 KiB. Mappings remain owned: refill/all-freed RSS stays around 69/76 MiB in both versions. This change recovers reusable inventory; it does not unmap those owned arenas or alter the separate dead-worker/FINREG retention problem.

The exact prior public Lua buffer workload passes three alternating pairs of 800,000 reset/put/get/byte-equality cycles per process with automatic GC. Median cycle times are 595.614/564.325 ns; median paired ratio 0.9475, range 0.8301–0.9603. Sampled work RSS spans 4,812–5,024 KiB baseline and 4,912–5,036 KiB candidate; final quiescent Lua byte counts match by paired sample. This is a bounded single-mutator control, not a multiworker memory-bound proof.

All timed processes were pinned to CPU 8. Frequency, shared-cache and host interference were not controlled; full paired variation is reported. The matrix contains 30 original hot,36 bulk-free/refill,12 retained-shrink,6 public buffer,10 potentially overlapping longer hot and 10 reserved-core longer hot invocations. Each raw stdout/stderr, command, dependency list, input and executable hash is preserved. `measurements.json`, `long-hot-measurements.json` and `long-hot-isolated-measurements.json` are raw rows; `summary.json` and CSV files contain summaries. `METHODOLOGY.md` records the protocol and timing caveat. Runtime source/build manifests were checked before measurements and reverified during final packaging. CPU 8/9 is released.
