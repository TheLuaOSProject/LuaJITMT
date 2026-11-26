Buffer rewrite performance comparison, 2026-11-26

Baseline: clean commit 394159a5ca21ebc8be65bca055245dc2ca8e4526.
Candidate: /tmp/lj-buffer-review-20261126-jtn9tue9/gcc-v2/src/luajit,
including the dictionary-index fix and retained-storage streaming decoder.
Frozen candidate SHA256:
51f3ea3533df30ac00433be15c79a29bc56595110f4d3f6952a1ee4be99b2167
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
Executables are frozen in baseline/src/luajit and candidate-luajit-v2.
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
- pilot-candidate.csv is an unreported v1 smoke run; it is not in any summary.

Findings:
- Streaming decode is linear: the total count is 200,000 records in each row,
  while individual streams grow from 1,000 to 10,000 to 100,000 records. CPU
  time per record stays roughly constant. Candidate cost is about 0.95 us per
  record versus about 0.80 us for clean HEAD.
- The rewrite substantially slows JIT reset/put/get, string set/get and tiny
  appends. These are single-mutator measurements; they do not measure the
  correctness or progress benefits of coherent shared storage.
- This package contains no broad stock-suite diagnosis or shared-workspace
  edits. Stability testing is managed by the parent task separately.
