# FNEW SSB fixture reopen capacity

The pre-existing `t-jit-fnew-bump.c` failure at the positive two-slot setup is a
fixture assumption. No production change is needed for this boundary.

`lj_gc2_preserve_abort_to_idle` deliberately retains active and published SSB
work. The next MARK start requests a flush while holding the worker token. If
the TG's second built-in node is still published, no free node exists; recycling
that node requires the same worker token and cannot occur inside MARK start.
The reopened MARK can therefore legitimately retain a full active SSB. The
no-drain FNEW certificate must continue to refuse this full buffer atomically.

A native ARM64 extraction of the unchanged boundary fixture failed the same
capacity assertion as the earlier Rosetta run. An explicit ordinary owner
`lj_gc2_flush_ssb` after reopening MARK makes the complete extracted boundary
pass. The fixture now prepares capacity this way before its positive two-slot
case; its foreign-owner, one-slot, and full-buffer rejection assertions remain.

`tests/t-gc2-ssb-preserve-reopen.c` separately proves the retained-pressure case.
It keeps a real published predecessor plus a full active node across abort and
restart, verifies both queue identities and entries, checks no-drain refusal,
and verifies that the normal owner flush preserves the full node and permits
later pair publication. It runs before the FNEW fixture in `m6_jit_fnew_bump`.
Native ARM64 validation used an assertion-enabled experimental JIT archive;
the fixture itself needs no private test-helper macro or native trace execution.

The later persistent root-snapshot fixture also failed natively after its
fixed 64 one-item MARK-close attempts. Its setup creates 20,000 rooted tables,
while `gc2_fixpoint_round` deliberately defers the root snapshot until older
queued work is drained (except owner NEEDSCAN handoffs).

Review then found that upstream commit `8cea705d` already repairs both setup
assumptions. Its root-snapshot setup explicitly drains the previous frontier
and requests native-lease exit before each scheduled close attempt. That
unchanged upstream setup now passes the native extraction, including all
snapshot-state and phase assertions. No production state was forced.

The other source/test changes from that upstream commit are also adopted:
allocator identity is inspected only through a test-only eligibility wrapper;
real allocation fallback is exercised through the existing MT-entry contender
instead of lying about the allocator. Constructor-completion assertions remain
enabled. The new test wrapper is absent from ordinary builds. The upstream
commit's stored build artifacts and historical report are not copied.

Validation used a clean native ARM64 experimental/assert build with both
function and table test helpers. Eighteen unchanged portable functions extracted
from the repaired fixture pass, including the real TNEW fallback, direct
allocator predicates and fallback, completed constructor states, and SSB
boundaries. The full fixture also compiles cleanly for x86-64, but its x86-64
machine-code execution remains unverified while translated test processes are
stuck. This is not a full FNEW-suite pass.


After translated execution recovered, a fresh assertion-enabled x86-64 build
at `57ae11b2` passed the complete fixture under Rosetta, both with its default
selection and with `LJ_TEST_TRACED_FNEW=1` enabling the additional traced cases.
The portable SSB-preserve regression also passed. Logs are
`/tmp/luajitmt-x64-fnew.log`, `/tmp/luajitmt-x64-fnew-traced.log`, and
`/tmp/luajitmt-x64-capture-fixtures.log`. This supersedes the earlier execution
limitation for this fixture; it does not resolve the separate captured-iterator
JIT failure described in the checkpoint note.
