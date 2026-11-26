Final buffer rewrite v4 performance comparison, 2026-11-26

Baseline: clean commit 394159a5ca21ebc8be65bca055245dc2ca8e4526.
Candidate: /tmp/lj-buffer-review-20261126-jtn9tue9/gcc-v4/src/luajit,
including the dictionary-index fix and decode consumption with its held input
lease. The experimental v3 tail append path was removed; append and putstr
match v2.
Frozen candidate SHA256:
162491d8216fcb4faacf45bc99442bd1e5008a9b7796aa679807287b764cf756
All binary and input hashes are recorded in metadata.json.

Both builds use GCC 14.2.0, static default configuration:
  make -C src -j5 BUILDMODE=static CC=gcc TARGET_STRIP=:
The baseline was built here from:
  git archive 394159a5ca21ebc8be65bca055245dc2ca8e4526 Makefile src dynasm
baseline-input.tar contains those exact sources; baseline-build.log is complete.
candidate-input.tar.gz contains the candidate's corresponding source files;
candidate-input.sha256 records each source hash.

Reproduce in this directory:
  python3 run.py
  python3 run-append-long.py
  python3 summarize.py
Executables are frozen in baseline/src/luajit and candidate-luajit-v4.
The report archive also includes baseline-luajit for convenience; to reproduce
from the archive, extract baseline-input.tar into baseline/ and rebuild it.

Method:
- One process at a time, pinned with taskset -c 8.
- Three paired samples of all eleven workloads, both warmed JIT and -joff.
- Baseline/candidate order reverses on alternating samples.
- Each workload is warmed twice after jit.flush(); a full collection and
  preparation occur outside the timed region before the measured iteration.
- JIT hotloop=20 and hotexit=10 in both binaries; no other tuning.
- Reported primary time is os.clock process CPU time. CLOCK_MONOTONIC wall
  time is also recorded for every row. CPU topology is in metadata.json.
- Complete payload assertions, per-read checks, expected lengths and decode
  checksums run for every workload. All passed.
- The normal put/get loops produced traces. Method decode is not recordable
  in these builds and ran in the interpreter even in the JIT-enabled process.
- Fixed FIFO input preparation is outside timing. Streaming decode includes
  each per-stream set(input), then all record decodes. Encoded records are
  nine-byte Lua numbers with value 41.

Because the initial JIT append baseline rows were shorter than one millisecond,
append-long.lua supplements them with five paired JIT samples. Each row sums
ten one-million-append intervals; resets and complete content verification are
outside those intervals. Use these longer rows for the small-append ratio.

Artifacts:
- results.csv: all 132 main timing rows, including checksums and trace counts.
- append-long-results.csv: all 20 supplemental timing rows.
- summary.csv / summary.txt: every workload and mode, medians and ranges.
- paired-ratios.csv: every individual paired comparison.
- Per-invocation stdout/stderr and runner logs preserve the raw results.
- versions.csv / versions.txt compare v2, v3 and final v4 for every row.
- v2-summary.csv and v3-summary.csv preserve those comparison inputs.

The final findings and all absolute values are in summary.txt and versions.txt.
This is the accepted stability candidate; remaining performance debt is retained
in the report rather than hidden by selecting only favorable workloads.
Earlier packages remain unchanged:
  /tmp/lj-buffer-perf-20261126-dt90tksg-v2.tar.gz
  /tmp/lj-buffer-perf-v3-20261126-v8v1ezme.tar.gz
