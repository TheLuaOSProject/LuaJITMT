# Free-bin capacity regression witness

Frozen baseline commit: `c106cd7bb109e9705c6b753f794001627320ab7f`.
Frozen baseline GCC tree: `/tmp/lj-plain-shrink-final-20261126-lxy_jr_l/gcc`.
Source and binary identities are in `baseline-identities.json`; the fixture and compile/run commands are preserved here. Baseline source was compared byte-for-byte with the committed source, including the final shrink header correction.

The fixture creates 9,000 real 1 KiB allocations separated by live 16 B guards. It consumes the final bump tail using real allocations, frees the targets, and checks exact available free cells and target addresses. It then allocates 9,000 blocks and verifies unique original-address reuse, payloads, guard bytes and unchanged arena count. Capacity cases never use rebuild, transfer, cursor edits or synthetic links.

Capacity sources: owner-local PLAIN and TRAVERSABLE, foreign-thread remote publication PLAIN and TRAVERSABLE, and PLAIN late intents retained behind one genuine admission per arena. The latter includes duplicate local/deferred/foreign intent and verifies all payload bytes until admissions leave. Managed remote publication is duplicated before drain; terminal local free is repeated before reuse for all cases. Ordinary PLAIN queued remote publication is invoked once per target, preserving its existing single-owner free contract.

Search cases use 8,192 too-small 1 KiB runs ahead of a real 2 KiB run in the same coarse bin. Normal frees place the fitting node exactly at position 8,193, which the fixture validates. No rebuild is needed: baseline insertion truncates the previous list only beyond 8,192, then prepends one new node. Candidate success also checks that a subsequent complete unsuccessful search retains all nonfitting nodes.

The separately labeled defensive-cycle case intentionally constructs a two-node allocated/stale prefix cycle for insertion and a real-free-node nonfit cycle for search. It requires bounded completion and safe fallback; this is not evidence of real production corruption.

| Case | Frozen baseline | Final GCC full-library candidate |
| --- | --- | --- |
| Five capacity/source variants | 8,193/9,000 targets retained and reused; 524,352/581,915 cells; arenas 170 to 185; exit 1 | 9,000/9,000 retained and reused; 581,915/581,915 cells; arenas 170 to 170; PASS |
| PLAIN and TRAVERSABLE search | Real fit at position 8,193 missed; all 8,192 nonfits dropped; arenas 155 to 156; exit 1 | Exact fit returned; all 8,192 nonfits retained; arenas 155 to 155; PASS |
| Synthetic defensive cycles | PASS | PASS |

All seven baseline failures are ordinary exit 1 with complete diagnostics, not signals or watchdogs. Each baseline case has its own `.log` and `.json` with exact command, executable hash, elapsed/CPU time and peak RSS. They completed in 0.094 to 0.249 seconds for capacity and 0.126 to 0.204 seconds for search; maximum recorded RSS was 26,020 KiB. The initial candidate standalone all-cases smoke completed in 0.123 seconds. These are diagnostic run durations, not a paired performance comparison: baseline and candidate smoke linking differ.

All processes were pinned to CPU 8, with a 1 GiB address-space ceiling, disabled core dumps, 60-second external timeout and 120-second internal alarm. Root owns final candidate builds, independent compiler/assert checks and suite integration. Final full-library candidate results are in `candidate-results.json` and eight per-case `.log`/`.json` pairs. All eight pass; capacity/search cases completed in 0.017 to 0.024 seconds with the same fixture and compiler flags as baseline. These single invocations remain diagnostics, not the separate paired performance matrix.

Final candidate GCC library SHA256: `2a29b71710786af7ee2f8a06eb402e1923ea72e31fba6d4d7ee390d05a7b0b28`. LuaJIT SHA256: `f6e978cb0875791b216df3608530e3d1180b121dc457c48e11f87fd66212365d`. The complete build manifest is `candidate-build.json`; all declared source and output hashes were verified.
