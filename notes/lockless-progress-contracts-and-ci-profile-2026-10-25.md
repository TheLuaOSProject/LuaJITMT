# Lockless progress contracts and focused-test profiles (2026-10-25)

## Progress claims

“Lockless” describes the synchronization mechanism, not a per-operation latency
promise. In particular, do not call table access, GC publication, or collection
wait-free as a group.

The `lj_tab_getscalar_rooted_try` primitive is a single bounded attempt. It
admits the table and any external vectors with small-object leases, validates
the captured generation and source roots, visits at most the captured hash
mask, and either publishes a scalar result or refuses while preserving the
output. It does not call `lj_tab_wait_l`, `lj_tab_wait_no_l`, yield, or enter a
safepoint. Contention or a changed generation can therefore make an individual
call return refusal; the caller/interpreter may retry later. The exact native
regression pauses the idle reclaimer after JIT quiescence and checks that 1,000
numeric hits plus one valid missing-key refusal complete without either table
wait counter changing.

`lj_gc2_small_lease_try` is likewise an admission attempt, not a wait: it can
decline a missing/closing registry entry, a failed remote-admission CAS, or an
arena whose lifetime/sweep metadata no longer authorizes the body. The
test-only counters distinguish attempts, admitted leases, ordinary refusals,
and rescue retries. A successful attempt is not a promise that later table
generation validation will succeed; the enclosing read must still validate
its snapshot before consuming the copied scalar.

Table-rescan membership is a unique per-table reservation paired with one
aggregate pending credit. A competing reservation can lose the table-state
CAS; only its provisional credit is then returned. A publisher or consumer may
retry its state CAS while another actor changes that same table state. This is
lock-free coordination, not wait-free progress for each participant. The
focused fixture pauses a publisher at INSTALLING, cancels that table's
membership, and proves another table can reserve and clear while the publisher
remains paused. It then checks the publisher settles the cancelled credit and
the aggregate returns to its starting value. The fixture also measures exact
add/sub and conflict counts for duplicate membership and independent-table
contention; it does not claim a universal throughput bound or change
production ordering.

## Instrumented native measurement

Run `JOBS=2 tools/ci/lockless_progress_contract.sh` on native macOS ARM64. The
script builds a test-helper profile, compiles the fixture against that exact
archive, checks architecture and archive identity before execution, and
restores the selected ARM64 bootstrap profile afterward. Test counters and
hooks compile only with `LJ_GC2_TEST_HELPERS` / `LJ_TAB_TEST_HELPERS`; these
measurements are not release-build performance claims.

On the final 2026-10-25 native run, the stalled-reclaimer case reported 1,001
scalar attempts (1,000 hits and one bounded miss), 1,001 admitted leases, no
lease refusals, and zero table wait calls. The four-thread independent-table
rescan case completed 40,000 exact credit additions and 40,000 subtractions in
2,283,000 ns (about 17.5 million add/sub pairs per second on that run). Wall
time is machine/load dependent. The serial duplicate case observed two
reservations, one duplicate conflict, two adds, two subs, and a pending count
returned to its starting value. Lease rescue-retry and state-CAS retry counters
were zero in these schedules; the instrumentation reports those counters so a
future contention schedule can distinguish retry work from ordinary declines.
The stalled-publisher schedule separately observed two reservations, two adds,
two subs, and successful independent-table completion before release.

## Focused ARM64 profile helper

`tools/ci/lib/arm64_profile.sh` centralizes the native/required-native branch,
build lock, profile-specific build and restoration, exact archive/runtime/
fixture architecture checks, compiler and flags, and timed fixture and Lua
test runners. The profile identifier hashes the contract label, target, target
flags, compiler, deployment target, XCFLAGS, SDK root, and resulting archive
and Lua-runtime hashes. Fixture runs are rejected if the archive, runtime, or
fixture/profile digest changed after the build. Set
`LJ_TEST_REQUIRE_NATIVE=1` to make a non-native host fail instead of skip, and
use `JOBS=2` to cap focused builds. The migrated phi-spill and numeric-spill
contracts both passed on native ARM64 with this helper.

The helper does not replace semantic, corruption, or lifecycle checks. Each
contract remains responsible for proving its own behavior and compiling its
own fixture. A shared profile only makes the binary being tested and the
conditions used to produce it explicit and reproducible.

## Non-test timing sample

`JOBS=2 tools/ci/arm64_release_scalar_read_bench.sh` builds a separate
non-test ARM64 bootstrap profile and times one million direct calls to the
production rooted scalar helper. The current ARM64 bootstrap still requires
`LUAJIT_DISABLE_JIT`; this sample measures the C helper without assertions or
test instrumentation and is not evidence about the future JIT fast path. The
2026-10-25 combined-worktree run measured 81.25 ns/read (12.3 million reads/s)
with profile `c500d6f02aa163dd295ba1c554d11f00ef1cfd3022c4f2fd7002c2aaa2d78ad7` on the current
host. Treat this as an initial local reference, not a portable performance
target; the profile and result are printed together for future comparisons.
The complete logs are `/tmp/ljmt-review-lockless_progress_contract.log` and
`/tmp/ljmt-review-release-scalar.log`.

## Hit/miss latency sampling

The release helper benchmark now exercises array, fractional-number hash,
string and boolean hits, plus array and hash misses. Each case verifies one
million hit/refusal outcomes and result preservation, then records 10,000
individual call samples. It reports raw p50/p95/p99/max and a separate empty
timer-pair baseline; it does not subtract unrelated quantiles. Darwin's Mach
absolute clock avoids the one-microsecond quantization observed with
`CLOCK_MONOTONIC` on this host. Its reported tick was 125/3 ns.

An exploratory release run (production `a836be4b`, shared profile plumbing from
`92393d92`) measured the following. Other isolated builds were active, so this
is methodology/coverage evidence and a local reference, not a controlled
before/after optimization claim. Raw individual samples include timer overhead
and scheduling; their tails are quantized by the 41.67 ns clock tick.

| Case | ns/read throughput | Raw p50 | Raw p95 | Raw p99 |
| --- | ---: | ---: | ---: | ---: |
| Array hit | 82.96 | 83 | 83 | 125 |
| Numeric hash hit | 140.74 | 125 | 166 | 250 |
| String hit | 190.94 | 208 | 208 | 291 |
| Boolean hit | 141.34 | 125 | 458 | 500 |
| Array miss | 76.00 | 83 | 83 | 83 |
| Hash miss | 185.95 | 166 | 208 | 291 |

The timer-pair baseline was p50=0 and p99=41 ns. These isolated direct helper
samples complement the resize/retirement and stalled-publisher contracts;
they do not measure their tail latency. Log:
`/tmp/ljmt-review-read-latency-mach.log`, profile
`7d7937dde82a0ac24db5cac693dc93a637e2e797b5b5d7042ac2a5fe3830a0f8`.

## x86-64/Rosetta counterpart

The same release C-helper driver at `cc7b6899` ran in an ad-hoc-signed x86-64
build with `-arch x86_64 -mcx16 -DLUAJIT_DISABLE_JIT`. All six cases verified
one million outcomes and 10,000 individual samples. Array/numeric-hash/string/
boolean hits measured 66.65/104.79/123.69/101.03 ns per call; array/hash misses
59.40/112.34 ns. Raw p99 values were 167/125/167/125/84/167 ns respectively;
the timer-pair p99 was 42 ns. Rosetta reports a 1/1 Mach timebase but the
observed samples still reflect the host clock. This is translated execution
on Apple Silicon, not a native Intel versus ARM performance comparison.
Log: `/tmp/ljmt-review-x64-scalar-read-bench.log`.

`sh tools/ci/x64_atomic_contract.sh` now reuses the CAS and concurrent table-pair
fixtures with their x86-64 implementations. Both pass under Rosetta; separate
object inspection requires inline CMPXCHG16B in the primitive and complete
GCtab-pair wrapper and rejects atomic-runtime imports. ARM64 retains its
separate CASPAL/LL-SC and per-helper exact-pair checks. No atomic implementation
or memory order changed. Log: `/tmp/ljmt-review-x64-atomic.log`.

## Scalar helper latency during resize and retirement

`tests/t-lockless-progress-metrics.c` now includes a deterministic stale-vector
case and a bounded concurrent resize sample. In the deterministic case, the
test-only scalar hook resizes an externally allocated array after the helper
has copied its value but before generation revalidation. The held array lease
keeps the old allocation available through that check; the helper returns a
refusal without changing its output, and the caller's next attempt succeeds.
This proves retirement is deferred while the read lease is held; it does not
claim that reclamation has completed at refusal time.

The contention case uses a real attached Lua reader actor while the main actor
publishes alternating 128/256-element array generations. The reader performs
4,000 logical reads, retries a refused single try at caller level up to 16
times, and counts any logical reads that exhaust that budget instead of
assuming every race settles within 16 attempts. Each attempt starts with a
sentinel output; a hit must replace it with 2.5 and a refusal must preserve it.
The fixture times each helper attempt with `mach_absolute_time()` on Apple
platforms and `CLOCK_MONOTONIC` elsewhere. Success and refusal samples are
reported separately as raw instrumented p50/p95/p99/max;
they include assertion/test-helper and timer overhead and are not release
performance targets. `caller-retries` counts refused try attempts retried by
the fixture. `lease_rescue_retry` is reported separately as an internal GC2
admission retry counter. The recorded runs had up to 512 resize calls, samples
during the resize window, and a queued retired array generation at the end of
each sample.

On native ARM64, profile
`2fa5d1afdcedcfbfb92864a795e01413730e6ef4b9f3b99591059d71e3a8f3ee` produced
7,050 attempts: 4,000 hits and 3,050 refusals retried by the caller, with no
exhausted logical reads. There were 7,042 attempts during the resize window.
All 21,150 small-lease attempts were admitted, with zero lease refusals or
rescue retries. Raw success latency was 458/792/875/958 ns
(p50/p95/p99/max); refusal latency was 583/833/875/9,541 ns.

On native arm64e/BTI, the profile also defines `LUAJIT_ENABLE_CET_BR`, so
`LJ_ABI_BRANCH_TRACK` is enabled for the VM/JIT as well as compiler BTI. Profile
`86e43ff0ca6ae236c2f0c9bb4cd846be55b38c3cdcf733dae8f7ae64e405e7c2` produced
6,962 attempts: 3,999 hits and 2,963 refusals, of which 2,962 were retried by
the caller; one logical read exhausted the 16-attempt budget. There were 6,767
attempts during the resize window. All 20,886 small-lease attempts were
admitted, with zero lease refusals or rescue retries. Raw success latency was
458/750/834/959 ns; refusal latency was 542/792/875/1,125 ns.

Reproduce on native Apple Silicon with:

```sh
JOBS=2 LJ_TEST_REQUIRE_NATIVE=1 TMPDIR=/private/tmp/ljmt-progress-tmp \
  LJ_LOCKLESS_PROGRESS_ARCH=arm64 sh tools/ci/lockless_progress_contract.sh
JOBS=2 LJ_TEST_REQUIRE_NATIVE=1 TMPDIR=/private/tmp/ljmt-progress-tmp \
  LJ_LOCKLESS_PROGRESS_ARCH=arm64e sh tools/ci/lockless_progress_contract.sh
```

The script verifies the
archive/runtime/fixture architecture and exact profile for each run, then
restores the ARM64 bootstrap plus experimental-JIT assertion profile.
Results characterize this bounded instrumented schedule only; they do not
establish wait-freedom or a universal latency bound.
