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
